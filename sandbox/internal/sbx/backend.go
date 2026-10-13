package sbx

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"sort"
	"strconv"
	"time"
)

// Backend is the interface implemented by each sandbox backend
// (docker, gvisor, firecracker).
type Backend interface {
	Name() string
	Create(opt Options, out *Info) error
	Stop(id string) error
	Remove(id string) error
	// CheckEnv verifies the host environment this backend needs (binaries,
	// kernel, daemon). Called before create so failures surface as clear
	// errors instead of mid-flight crashes. Default: pass.
	CheckEnv() error
}

// ErrRootfsNotPrepared marks a create that cannot proceed because the guest
// rootfs template has not been built for the current image (or is stale). The
// fix is a build - `make rootfs`, i.e. `sbx rootfs build` - not a retry, so the
// CLI reports it with its own exit code (ExitRootfsNotReady) and a driver can
// tell it apart from a bring-up failure that is worth retrying.
var ErrRootfsNotPrepared = errors.New("guest rootfs not prepared")

// ExitRootfsNotReady is the process exit code of a create whose environment is
// not prepared (see ErrRootfsNotPrepared): distinct from 1 (a bring-up failure)
// and from 124 (the caller's own timeout).
const ExitRootfsNotReady = 3

// Registry of registered backends. Backends self-register via Register() in an
// init() call; cmd/sbx blank-imports the backend packages to trigger it.
var registry = map[string]Backend{}

// Register adds a backend to the registry under the given name.
func Register(name string, b Backend) { registry[name] = b }

func getBackend(name string) Backend { return registry[name] }

// RegisteredBackends returns the sorted list of backend names.
func RegisteredBackends() []string {
	out := make([]string, 0, len(registry))
	for name := range registry {
		out = append(out, name)
	}
	sort.Strings(out)
	return out
}

// CheckEnv verifies the environment for the given backend.
func CheckEnv(backend string) error {
	impl := getBackend(backend)
	if impl == nil {
		return fmt.Errorf("unknown backend: %s", backend)
	}
	return impl.CheckEnv()
}

// Create provisions a sandbox via the selected backend.
func Create(opt Options, out *Info) error {
	backend := getBackend(opt.Backend)
	if backend == nil {
		return fmt.Errorf("unknown backend: %s", opt.Backend)
	}
	if err := backend.CheckEnv(); err != nil {
		return err
	}
	return backend.Create(opt, out)
}

// Stop tears down the runtime side of a sandbox (VM kill, TAP teardown).
func Stop(id string) error {
	info := Load(id)
	if info.ID == "" {
		return fmt.Errorf("no such sandbox: %s", id)
	}
	backend := getBackend(info.Backend)
	if backend == nil {
		return fmt.Errorf("unknown backend: %s", info.Backend)
	}
	if err := backend.Stop(info.ID); err != nil {
		return err
	}
	NumaRelease(info.ID)
	return nil
}

// Remove tears a sandbox down fully (cgroup, chroot, slot, state file).
func Remove(id string) error {
	info := Load(id)
	if info.ID == "" {
		return fmt.Errorf("no such sandbox: %s", id)
	}
	backend := getBackend(info.Backend)
	if backend == nil {
		return fmt.Errorf("unknown backend: %s", info.Backend)
	}
	if err := backend.Remove(info.ID); err != nil {
		return err
	}
	NumaRelease(info.ID)
	return nil
}

// WaitAgent polls GET /health on the agent until it returns 200 or times out,
// returning the response body (which carries the guest's boot timing) on
// success.
func WaitAgent(host string, port int, token string, timeoutSec int) (bool, string, Response) {
	headers := map[string]string{"Authorization": "Bearer " + token}
	tStart := time.Now()
	deadline := tStart.Add(time.Duration(timeoutSec) * time.Second)
	var last Response
	for time.Now().Before(deadline) {
		r := agentHTTPGet(host, port, "/health", headers)
		if r.Status == 200 {
			return true, r.Body, r
		}
		last = r
		/* A successful poll dates readiness to the moment it was sent, so the
		 * poll interval is the resolution of everything measured through here:
		 * a flat 100 ms sleep rounds every sample up to the next 100 ms
		 * boundary, which is the same order as the boot it is being used to
		 * measure. Poll finely while the wait is still boot-shaped, then back
		 * off so a guest that never comes up does not turn the timeout into a
		 * request storm. */
		time.Sleep(pollInterval(time.Since(tStart)))
	}
	return false, "", last
}

// pollInterval is the /health poll interval to use after waiting this long:
// fine enough to time a microVM boot, coarse once the wait can no longer be
// one.
func pollInterval(waited time.Duration) time.Duration {
	switch {
	case waited < 500*time.Millisecond:
		return 2 * time.Millisecond
	case waited < 2*time.Second:
		return 20 * time.Millisecond
	default:
		return 100 * time.Millisecond
	}
}

// AgentWaitDetail renders the last /health response of a failed WaitAgent, so a
// failure says whether the guest never answered - a transport error, i.e. the
// agent never started or the guest is not reachable - or answered with an error
// status, i.e. the agent is up but not ready.
func AgentWaitDetail(r Response) string {
	if r.Err != nil {
		return r.Err.Error()
	}
	if r.Status == 0 {
		return "no response"
	}
	return fmt.Sprintf("HTTP %d", r.Status)
}

// HealthTimeoutSec is the /health budget to use: the backend's own default
// unless SBX_HEALTH_SEC overrides it. A guest only gets the vCPU quota the
// sandbox was created with, so on a loaded host its boot - and with it the
// time the agent needs to answer - can stretch well past a warm-start second.
func HealthTimeoutSec(fallback int) int {
	if v := os.Getenv("SBX_HEALTH_SEC"); v != "" {
		if n, err := strconv.Atoi(v); err == nil && n > 0 {
			return n
		}
	}
	return fallback
}

// HealthBootSec extracts the guest's instance-start -> init latency (seconds)
// from a /health response body; -1 when absent.
func HealthBootSec(body string) float64 {
	var h struct {
		BootSec float64 `json:"boot_sec"`
	}
	if json.Unmarshal([]byte(body), &h) != nil || h.BootSec <= 0 {
		return -1
	}
	return h.BootSec
}

// HealthUserspaceSec extracts the guest-monotonic init -> agent latency from
// /health; -1 when the guest agent predates this metric or the value is absent.
func HealthUserspaceSec(body string) float64 {
	var h struct {
		UserspaceSec float64 `json:"userspace_sec"`
	}
	if json.Unmarshal([]byte(body), &h) != nil || h.UserspaceSec <= 0 {
		return -1
	}
	return h.UserspaceSec
}

// HealthAgentStartUnixSec extracts the agent process start wall-clock time from
// /health. It is used to align the process start with Docker's StartedAt,
// whose timestamp is also wall-clock based; -1 when absent.
func HealthAgentStartUnixSec(body string) float64 {
	var h struct {
		AgentStartUnixSec float64 `json:"agent_start_unix_sec"`
	}
	if json.Unmarshal([]byte(body), &h) != nil || h.AgentStartUnixSec <= 0 {
		return -1
	}
	return h.AgentStartUnixSec
}

// Response is a minimal HTTP response for the agent client helpers used by the
// backend layer.
type Response struct {
	Status int
	Body   string
	Err    error
}
