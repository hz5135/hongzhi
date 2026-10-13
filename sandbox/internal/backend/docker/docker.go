// Package docker implements the docker sandbox backend using the official
// Docker Engine SDK (no docker CLI / command-line concatenation).
package docker

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"strconv"
	"time"

	"github.com/docker/docker/api/types/container"
	"github.com/docker/docker/client"
	"github.com/docker/go-connections/nat"

	"agentbench/sandbox/internal/sbx"
)

func init() {
	sbx.Register("docker", New(""))
}

// Backend provisions an agent container from the given image using the Docker
// SDK. The same type powers the gvisor backend (which passes a non-empty
// `runtime`, e.g. "runsc").
type Backend struct {
	runtime string
}

// New returns a backend that launches containers under the given OCI runtime
// (empty = Docker's default).
func New(runtime string) *Backend { return &Backend{runtime: runtime} }

func newClient() (*client.Client, error) {
	return client.NewClientWithOpts(client.FromEnv, client.WithAPIVersionNegotiation())
}

func (b *Backend) Name() string {
	if b.runtime != "" {
		return "gvisor"
	}
	return "docker"
}

func (b *Backend) CheckEnv() error {
	cli, cerr := newClient()
	if cerr != nil {
		return fmt.Errorf("docker daemon not reachable: %w", cerr)
	}
	defer cli.Close()
	if _, verr := cli.ServerVersion(context.Background()); verr != nil {
		return fmt.Errorf("docker daemon not reachable: %w", verr)
	}
	if b.runtime != "" {
		info, ierr := cli.Info(context.Background())
		if ierr != nil {
			return fmt.Errorf("docker info failed: %w", ierr)
		}
		if _, ok := info.Runtimes[b.runtime]; !ok {
			if daemonJSONRegisters(b.runtime) {
				return fmt.Errorf("docker has not loaded the '%s' runtime yet: /etc/docker/daemon.json "+
					"registers it, but the daemon reads its runtime list at startup only - restart it "+
					"(systemctl restart docker)", b.runtime)
			}
			return fmt.Errorf("docker has no '%s' (gVisor) runtime registered; add it to /etc/docker/daemon.json:\n"+
				"  {\"runtimes\": {\"%s\": {\"path\": \"/usr/local/bin/runsc\"}}}\n"+
				"and restart docker", b.runtime, b.runtime)
		}
	}
	return nil
}

// CgroupDriver reports the cgroup driver the daemon is using ("systemd" or
// "cgroupfs"). The gvisor backend needs it: runsc sets up the container's
// cgroup itself and cannot do that through a systemd older than 244.
func (b *Backend) CgroupDriver() (string, error) {
	cli, err := newClient()
	if err != nil {
		return "", err
	}
	defer cli.Close()
	info, err := cli.Info(context.Background())
	if err != nil {
		return "", err
	}
	return info.CgroupDriver, nil
}

// daemonJSONRegisters reports whether the daemon config already names `runtime`.
// It tells "never configured" apart from "configured, daemon not restarted":
// the runtime list is read at startup only, so adding an entry to the file has
// no effect until dockerd is restarted. Best effort - a missing or unparseable
// file just means "no hint available".
func daemonJSONRegisters(runtime string) bool {
	data, err := os.ReadFile(daemonJSONPath)
	if err != nil {
		return false
	}
	var cfg struct {
		Runtimes map[string]json.RawMessage `json:"runtimes"`
	}
	if json.Unmarshal(data, &cfg) != nil {
		return false
	}
	_, ok := cfg.Runtimes[runtime]
	return ok
}

// daemonJSONPath is the default daemon config file; a daemon started with
// --config-file elsewhere only loses the hint, not the check.
const daemonJSONPath = "/etc/docker/daemon.json"

func (b *Backend) Create(opt sbx.Options, out *sbx.Info) error {
	info := &sbx.Info{}
	info.ID = sbx.RandomHex(8)
	info.Name = opt.Name
	if info.Name == "" {
		info.Name = info.ID
	}
	info.Backend = b.Name()
	info.Image = opt.Image
	info.ContainerName = "sbx-" + info.ID
	info.AgentHost = "127.0.0.1"
	info.AgentPort = opt.Port
	if info.AgentPort <= 0 {
		info.AgentPort = sbx.FindFreePort()
	}
	info.AgentToken = sbx.RandomHex(32)
	tStart := time.Now()

	var numa sbx.NumaAllocation
	if err := sbx.ResolvePlacement(opt, info.ID, &numa); err != nil {
		return err
	}

	// Persist as "creating" before touching the daemon so an interrupted
	// create is still discoverable and removable (see firecracker backend).
	info.Status = "creating"
	if err := info.Save(); err != nil {
		sbx.NumaRelease(info.ID)
		return fmt.Errorf("save state: %w", err)
	}

	cli, cerr := newClient()
	if cerr != nil {
		return fmt.Errorf("docker daemon not reachable: %w", cerr)
	}
	defer cli.Close()
	ctx := context.Background()

	runtime := b.runtime
	if b.runtime != "" {
		// gvisor: honor `--runtime=<name>` passthrough, else SBX_GVISOR_RUNTIME.
		runtime = sbx.BackendOpt(opt.Args, "gvisor", "runtime", runtime)
	}
	hostCfg := &container.HostConfig{
		Resources: container.Resources{
			NanoCPUs:   int64(opt.CPUs) * 1_000_000_000,
			Memory:     int64(opt.MemoryMB) * 1024 * 1024,
			CpusetCpus: numa.CPUs,
			CpusetMems: numa.Mems,
		},
		PortBindings: nat.PortMap{
			nat.Port("9119/tcp"): []nat.PortBinding{{HostIP: "0.0.0.0", HostPort: strconv.Itoa(info.AgentPort)}},
		},
		Runtime: runtime,
	}
	config := &container.Config{
		Image:        opt.Image,
		Env:          []string{"SBX_AGENT_TOKEN=" + info.AgentToken, "SBX_AGENT_PORT=9119"},
		ExposedPorts: nat.PortSet{nat.Port("9119/tcp"): {}},
	}

	resp, cerr := cli.ContainerCreate(ctx, config, hostCfg, nil, nil, info.ContainerName)
	if cerr != nil {
		sbx.NumaRelease(info.ID)
		os.Remove(sbx.StatePath(info.ID))
		return fmt.Errorf("docker create failed: %w", cerr)
	}
	containerID := resp.ID
	/* Match the microVM boundary: host setup ends immediately before the start
	 * request, and boot begins with the request itself so daemon/runtime setup
	 * performed by ContainerStart is not lost between segments. */
	tContainerStart := time.Now()
	if err2 := cli.ContainerStart(ctx, containerID, container.StartOptions{}); err2 != nil {
		cli.ContainerRemove(ctx, containerID, container.RemoveOptions{Force: true})
		sbx.NumaRelease(info.ID)
		os.Remove(sbx.StatePath(info.ID))
		return fmt.Errorf("docker start failed: %w", err2)
	}

	timeoutSec := sbx.HealthTimeoutSec(30)
	healthy, health, last := sbx.WaitAgent(info.AgentHost, info.AgentPort, info.AgentToken, timeoutSec)
	if !healthy {
		cli.ContainerRemove(ctx, containerID, container.RemoveOptions{Force: true})
		sbx.NumaRelease(info.ID)
		os.Remove(sbx.StatePath(info.ID))
		return fmt.Errorf("agent did not become healthy on port %d within %ds: last /health: %s",
			info.AgentPort, timeoutSec, sbx.AgentWaitDetail(last))
	}

	info.Status = "running"
	info.Meta = map[string]string{"containerID": containerID}
	info.Meta["provision_sec"] = "0.000000"
	hostSetupSec := tContainerStart.Sub(tStart).Seconds()
	info.Meta["host_setup_sec"] = strconv.FormatFloat(hostSetupSec, 'f', 6, 64)

	/* The agent process start wall clock and the timestamp sampled immediately
	 * before ContainerStart are in the same host clock domain for Docker and
	 * gVisor. Their difference is the full runtime boot segment, including the
	 * Docker API, OCI runtime and (for gVisor) sentry setup. */
	userspace := sbx.HealthUserspaceSec(health)
	agentStartUnix := sbx.HealthAgentStartUnixSec(health)
	if agentStartUnix >= 0 {
		sec := int64(agentStartUnix)
		nsec := int64((agentStartUnix - float64(sec)) * 1e9)
		agentStart := time.Unix(sec, nsec)
		boot := agentStart.Sub(tContainerStart).Seconds()
		if boot >= 0 {
			info.Meta["boot_sec"] = strconv.FormatFloat(boot, 'f', 6, 64)
			info.Meta["startup_sec"] = strconv.FormatFloat(hostSetupSec+boot, 'f', 6, 64)
			info.Meta["startup_source"] = b.Name() + "_container_start"
		}
	}
	if userspace >= 0 {
		info.Meta["userspace_sec"] = strconv.FormatFloat(userspace, 'f', 6, 64)
	}
	if startup, err := strconv.ParseFloat(info.Meta["startup_sec"], 64); err == nil && userspace >= 0 {
		info.Meta["ready_sec"] = strconv.FormatFloat(startup+userspace, 'f', 6, 64)
	}
	if err3 := info.Save(); err3 != nil {
		return fmt.Errorf("save state: %w", err3)
	}
	*out = *info
	return nil
}

func (b *Backend) Stop(id string) error {
	info := sbx.Load(id)
	if info.ContainerName == "" {
		return fmt.Errorf("no container for sandbox %s", id)
	}
	cli, cerr := newClient()
	if cerr != nil {
		return fmt.Errorf("docker daemon not reachable: %w", cerr)
	}
	defer cli.Close()
	ctx := context.Background()
	timeout := 5
	if sErr := cli.ContainerStop(ctx, info.ContainerName, container.StopOptions{Timeout: &timeout}); sErr != nil {
		cli.ContainerKill(ctx, info.ContainerName, "KILL")
	}
	info.Status = "stopped"
	info.Save()
	return nil
}

func (b *Backend) Remove(id string) error {
	info := sbx.Load(id)
	cli, cerr := newClient()
	if cerr == nil {
		cli.ContainerRemove(context.Background(), info.ContainerName, container.RemoveOptions{Force: true})
		cli.Close()
	}
	os.Remove(sbx.StatePath(info.ID))
	return nil
}
