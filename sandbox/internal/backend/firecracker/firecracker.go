// Package firecracker implements the Firecracker microVM sandbox backend.
//
// VM configuration is driven through the firecracker-go-sdk (a typed client
// for the Firecracker API over the jailer's Unix socket); the jailer itself is
// launched as a structured exec (no Go SDK exists for the jailer binary). Host
// plumbing (slot allocation, netlink TAP, go-iptables port forwarding, cached
// rootfs/injection, kernel resolution) is shared via internal/sbxhost.
package firecracker

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"

	"agentbench/sandbox/internal/sbx"
	"agentbench/sandbox/internal/sbxhost"
)

func init() {
	sbx.Register("firecracker", &Backend{})
}

// fcBin and jailerBin resolve the two binaries firecracker needs.
func fcBin() (string, error) {
	return sbxhost.ResolveBin("SBX_FIRECRACKER", "firecracker")
}
func jailerBin() (string, error) {
	return sbxhost.ResolveBin("SBX_JAILER", "jailer")
}

type Backend struct{}

func (b *Backend) Name() string { return "firecracker" }

func (b *Backend) CheckEnv() error {
	if err := sbx.CheckAgentBinary(); err != nil {
		return err
	}
	// The jailer applies cpu.max / memory.max / optional cpuset.* through a
	// cgroup v2 tree; fail fast if the host does not run v2 with those
	// controllers enabled instead of letting the jailer die mid-create.
	if err := sbxhost.CheckCgroupV2(); err != nil {
		return err
	}
	fcPath, berr := fcBin()
	if berr != nil {
		return berr
	}
	if st, serr := os.Stat(fcPath); serr != nil || st.Mode()&0o111 == 0 {
		return fmt.Errorf("firecracker binary not executable: %s", fcPath)
	}
	jailerPath, jerr := jailerBin()
	if jerr != nil {
		return jerr
	}
	if st, serr := os.Stat(jailerPath); serr != nil || st.Mode()&0o111 == 0 {
		return fmt.Errorf("jailer binary not executable: %s", jailerPath)
	}
	kernel, kerr := sbxhost.ResolveKernel("firecracker", "")
	if kerr != nil {
		return kerr
	}
	if !sbxhost.IsElfOrPe(kernel) {
		return fmt.Errorf("kernel is neither an ELF nor a PE (EFI) image: %s", kernel)
	}
	agentBin := sbx.SandboxAgentBin()
	if !sbx.FileExists(agentBin) {
		return fmt.Errorf("sandbox agent binary not found at %s (set SBX_AGENT_BIN)", agentBin)
	}
	return nil
}

func (b *Backend) Create(opt sbx.Options, out *sbx.Info) error {
	kernel, kerr := sbxhost.ResolveKernel("firecracker", opt.Kernel)
	if kerr != nil {
		return kerr
	}
	fcPath, ferr := fcBin()
	if ferr != nil {
		return ferr
	}
	jailerPath, jerr := jailerBin()
	if jerr != nil {
		return jerr
	}
	agentBin := sbx.SandboxAgentBin()
	if !sbx.FileExists(agentBin) {
		return fmt.Errorf("sandbox agent binary not found at %s (set SBX_AGENT_BIN)", agentBin)
	}
	// Provisioning (building/caching the 1 GB rootfs template) is a one-time
	// host-side cost shared by every sandbox; it must not be reported as
	// per-sandbox startup, so time it separately from the VM bring-up.
	tProvision := time.Now()
	rootfs, rerr := sbxhost.EnsureInjectedRootfs(opt.Image, agentBin)
	provisionSec := time.Since(tProvision).Seconds()
	if rerr != nil {
		return rerr
	}
	tStart := time.Now()
	slot, slotLock, serr := sbxhost.FindSlot()
	if serr != nil {
		return serr
	}
	tap := fmt.Sprintf("fhvm%d", slot)
	hostIP := sbxhost.HostIPFor(slot)
	guestIP := sbxhost.GuestIPFor(slot)
	if terr := sbxhost.SetupTap(tap, hostIP); terr != nil {
		sbxhost.ReleaseSlot(slotLock)
		return fmt.Errorf("tap setup: %w", terr)
	}

	info := &sbx.Info{}
	info.ID = sbx.RandomHex(8)
	info.Name = opt.Name
	if info.Name == "" {
		info.Name = info.ID
	}
	info.Backend = "firecracker"
	info.Image = opt.Image
	info.AgentToken = sbx.RandomHex(32)
	info.GuestIP = guestIP
	info.TapDevice = tap
	info.SlotLock = slotLock
	if opt.Port > 0 {
		info.PublishPort = opt.Port
	}
	info.AgentHost = guestIP
	info.AgentPort = 9119

	// Persist the state as "creating" BEFORE claiming more resources: if the
	// host is interrupted mid-create (Ctrl+C during rootfs copy / VM boot /
	// agent wait), `sbx remove <name>` can still Load() this record and tear
	// everything down. Without it, a killed create leaks its numa entry,
	// cgroup, TAP and slot because remove has nothing to find.
	info.Status = "creating"
	if serr := info.Save(); serr != nil {
		sbxhost.ReleaseSlot(slotLock)
		sbxhost.TearDownTap(tap)
		return fmt.Errorf("save state: %w", serr)
	}

	base := sbxhost.JailerBase()
	chrootDir := filepath.Join(base, "firecracker", info.ID)
	root := filepath.Join(chrootDir, "root")
	// Jailer-relative kernel file name: bzImage on x86_64, Image on aarch64.
	kernelName := sbxhost.DefaultKernelName("firecracker")

	// Any failure tears down every host resource claimed so far; success flips
	// the flag so the resources live on under the sandbox's ownership.
	success := false
	defer func() {
		if success {
			return
		}
		/* A failed bring-up is torn down so it cannot leak a VM, a tap or a
		 * slot - unless the caller asked to keep it for inspection, which is
		 * what is needed to look at a failure that does not repeat. */
		if os.Getenv("SBX_KEEP_FAILED") != "" && info.ID != "" {
			fmt.Fprintf(os.Stderr,
				"sandbox %s left in place for inspection (SBX_KEEP_FAILED=1):\n"+
					"  chroot %s\n  vm pid %d\n  tap %s\n"+
					"inspect it, then remove it with `sbx remove %s`\n",
				info.ID, chrootDir, sbxhost.FindVmPid(info.ID), tap, info.ID)
			sbxhost.ReleaseSlot(slotLock)
			return
		}
		sbxhost.TearDownPortForward(*info)
		sbxhost.TearDownTap(tap)
		if info.ID != "" {
			if vpid := sbxhost.FindVmPid(info.ID); vpid > 0 {
				sbxhost.KillPid(vpid)
			}
			if chrootDir != "" {
				os.RemoveAll(chrootDir)
			}
			/* A failed create must not leave a phantom record behind for
			 * `sbx list`/`remove` (the docker backend removes it too). */
			os.Remove(sbx.StatePath(info.ID))
			sbx.NumaRelease(info.ID)
		}
		sbxhost.ReleaseSlot(slotLock)
	}()

	var numa sbx.NumaAllocation
	if err := sbx.ResolvePlacement(opt, info.ID, &numa); err != nil {
		return err
	}

	// Per-VM jailer chroot: kernel + rootfs live in the jailer's root so
	// Firecracker (running jailed) can reach them by relative path.
	for _, d := range []string{filepath.Join(root, "kernel"), filepath.Join(root, "rootfs"), filepath.Join(root, "run")} {
		if mErr := sbx.Mkdirs(d); mErr != nil {
			return fmt.Errorf("mkdir: %w", mErr)
		}
	}
	if cErr := sbxhost.CopyFile(kernel, filepath.Join(root, "kernel", kernelName)); cErr != nil {
		return cErr
	}
	if cErr := sbxhost.CopyFile(rootfs, filepath.Join(root, "rootfs", "rootfs.ext4")); cErr != nil {
		return cErr
	}

	if sErr := spawnJailer(jailerPath, fcPath, info.ID, base, opt, numa); sErr != nil {
		return sErr
	}

	socketPath := filepath.Join(root, "run", "firecracker.socket")
	if !waitForSocket(socketPath, 10) {
		return fmt.Errorf("firecracker API socket not ready")
	}

	api := newFCAPI(socketPath)
	ctx := context.Background()

	/* `init=` is what makes `--boot-timer` (above) emit its `Guest-boot-time`
	 * record: the wrapper signals the boot device and then execs the real init,
	 * so the measured window ends where the guest's own init begins. Without it
	 * the device never speaks, and the startup metric falls back to the guest's
	 * kernel timestamp. The path is the one InjectAgentFiles installs. */
	bootArgs := fmt.Sprintf("console=ttyS0 quiet reboot=k panic=1 pci=off init=%s ip=%s::%s:255.255.255.252::eth0:off:1.1.1.1 agentbench.token=%s",
		sbxhost.GuestBootInitPath, guestIP, hostIP, info.AgentToken)
	if aErr := api.putBootSource(ctx, "kernel/"+kernelName, bootArgs); aErr != nil {
		return aErr
	}
	if aErr := api.putDrive(ctx, "rootfs", "rootfs/rootfs.ext4"); aErr != nil {
		return aErr
	}
	if aErr := api.putMachineConfig(ctx, opt.CPUs, opt.MemoryMB); aErr != nil {
		return aErr
	}
	if aErr := api.putNetworkInterface(ctx, "eth0", tap, sbxhost.MACFor(slot)); aErr != nil {
		return aErr
	}
	/* Port forwarding is host setup and must be complete before the guest boot
	 * clock starts; otherwise its iptables work is charged to userspace. */
	if pErr := sbxhost.SetupPortForward(*info); pErr != nil {
		return pErr
	}

	/* Sample the host clock immediately before asking the VMM to start, so
	 * host_setup_sec covers everything this function did while no guest was
	 * running: the slot and tap, the 1 GB rootfs copy into the jailer chroot,
	 * spawning the VMM, waiting for its API socket, programming it and setting
	 * up port forwarding. Only the instance-start request itself falls on the
	 * other side of the sample. */
	tInstanceStart := time.Now()
	if aErr := api.startInstance(ctx); aErr != nil {
		return aErr
	}

	info.PID = sbxhost.FindVmPid(info.ID)
	info.PIDFile = filepath.Join(root, "run", "firecracker.pid")

	timeoutSec := sbx.HealthTimeoutSec(60)
	healthy, health, last := sbx.WaitAgent(info.AgentHost, info.AgentPort, info.AgentToken, timeoutSec)
	tHealthy := time.Now()
	if !healthy {
		/* The guest's console is not captured anywhere (the jailer daemonizes
		 * firecracker, whose serial output then goes to /dev/null), so report
		 * the host-side state instead: it separates a crashed VM from a guest
		 * that is up but never served /health. */
		return fmt.Errorf(
			"agent did not become healthy at %s:%d within %ds: last /health: %s; "+
				"vm pid %d (%s), tap %s (%s), provision %.3fs, agent %s, kernel %s%s",
			info.AgentHost, info.AgentPort, timeoutSec, sbx.AgentWaitDetail(last),
			info.PID, pidState(info.PID), tap, linkState(tap), provisionSec,
			describeFile(agentBin), describeFile(kernel),
			sbxhost.LogTail(40, filepath.Join(root, "run", "firecracker.log"))+
				sbxhost.Ext4Tail(filepath.Join(root, "rootfs", "rootfs.ext4"), 40,
					"/var/log/sbx-boot.log", "/var/log/sbx-agent.log"))
	}

	if info.Meta == nil {
		info.Meta = map[string]string{}
	}
	info.Meta["provision_sec"] = strconv.FormatFloat(provisionSec, 'f', 6, 64)
	// Keep the agent-reported kernel timestamp as a diagnostic/fallback, but use
	// Firecracker's own boot timer for the guest's boot whenever it is present.
	// The health request is only the transport used to obtain the response;
	// guest userspace is not part of boot_sec.
	kernelInit := sbx.HealthBootSec(health)
	if kernelInit >= 0 {
		info.Meta["kernel_init_sec"] = strconv.FormatFloat(kernelInit, 'f', 6, 64)
	}
	bootLog := filepath.Join(root, "run", "firecracker.log")
	boot, haveBoot := firecrackerBootTimeSec(bootLog)
	if haveBoot {
		info.Meta["boot_sec"] = strconv.FormatFloat(boot, 'f', 6, 64)
		info.Meta["startup_source"] = "firecracker_boot_timer"
	} else if kernelInit >= 0 {
		boot, haveBoot = kernelInit, true
		info.Meta["boot_sec"] = strconv.FormatFloat(boot, 'f', 6, 64)
		info.Meta["startup_source"] = "guest_kmsg_fallback"
	}
	hostSetupSec := tInstanceStart.Sub(tStart).Seconds()
	info.Meta["host_setup_sec"] = strconv.FormatFloat(hostSetupSec, 'f', 6, 64)
	if haveBoot {
		/* startup_sec is the whole bring-up up to the guest's own init -
		 * provisioning, then host setup, then the guest's boot, in the order
		 * they happen - because a sandbox's start latency is what a caller
		 * compares, and stopping the clock at the boot request leaves out work
		 * the caller paid for. Its parts are reported next to it, so
		 * startup - provision - host_setup == boot_sec, and boot_sec is still
		 * nothing but the bare kernel boot. userspace_sec is the segment every
		 * other metric drops: the guest's own userspace, init -> the agent
		 * answering /health. */
		info.Meta["startup_sec"] = strconv.FormatFloat(provisionSec+hostSetupSec+boot, 'f', 6, 64)
		if userspace := sbx.HealthUserspaceSec(health); userspace >= 0 {
			info.Meta["userspace_sec"] = strconv.FormatFloat(userspace, 'f', 6, 64)
		} else {
			info.Meta["userspace_sec"] = strconv.FormatFloat(tHealthy.Sub(tInstanceStart).Seconds()-boot, 'f', 6, 64)
		}
	}
	info.Meta["ready_sec"] = strconv.FormatFloat(tHealthy.Sub(tStart).Seconds(), 'f', 6, 64)
	info.Status = "running"
	if sErr := info.Save(); sErr != nil {
		return fmt.Errorf("save state: %w", sErr)
	}
	*out = *info
	success = true
	return nil
}

func (b *Backend) Stop(id string) error {
	info := sbx.Load(id)
	// Kill the VM before dismantling the host side: the kernel then tears the
	// guest down (its charged memory, its vCPUs) while we remove the tap and the
	// bookkeeping, instead of after - remove waits for that teardown anyway.
	if info.PID > 0 {
		sbxhost.KillPid(info.PID)
		info.PID = -1
	}
	if info.TapDevice != "" {
		sbxhost.TearDownTap(info.TapDevice)
	}
	sbxhost.TearDownPortForward(info)
	info.Status = "stopped"
	info.Save()
	return nil
}

// reapCgroup removes the sandbox's cgroup after killing every task in it. The
// kernel rejects rmdir until the killed VM has been fully reaped, which can
// take longer under host contention. Wait up to four seconds so Ctrl+C cleanup
// does not delete the state file while leaving an ownerless cgroup behind.
func reapCgroup(cgDir string) {
	killFile := filepath.Join(cgDir, "cgroup.kill")
	if sbx.FileExists(killFile) {
		os.WriteFile(killFile, []byte("1"), 0o644)
	}
	for attempt := 0; attempt < 200; attempt++ {
		os.Remove(cgDir)
		if !sbx.DirExists(cgDir) {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
}

// sweepCgroups drops sandbox cgroups left by removes that did not wait for their
// VM to be reaped. "Empty" alone is not enough to identify one: Create persists
// the state file before spawnJailer runs and the jailer writes cpuset.cpus and
// cpu.max into its brand-new, still-empty cgroup - so a naive sweep races
// concurrent creates and deletes the cgroup out from under them (the jailer then
// fails with ENOENT on cpuset.cpus). A sandbox that is live, in flight or being
// removed always has a state file; only ids without one are leftovers.
func sweepCgroups() {
	root := filepath.Join("/sys/fs/cgroup", "firecracker")
	entries, err := os.ReadDir(root)
	if err != nil {
		return
	}
	for _, e := range entries {
		if !e.IsDir() {
			continue
		}
		if sbx.FileExists(sbx.StatePath(e.Name())) {
			continue
		}
		os.Remove(filepath.Join(root, e.Name()))
	}
}

func (b *Backend) Remove(id string) error {
	info := sbx.Load(id)
	if info.Status != "stopped" {
		b.Stop(id)
	}
	// Release the shared-resource accounting FIRST (NUMA ledger, slot lock),
	// so an interrupted remove can never leak them; the slow 1GB chroot
	// removal and the state-file delete come last — a leftover state file
	// makes `sbx remove` safely re-entrant for a full retry.
	sbx.NumaRelease(info.ID)
	sbxhost.ReleaseSlot(info.SlotLock)

	// Reaping the cgroup and deleting the chroot are independent, and the former
	// is the long pole (it waits for the kernel to finish tearing the guest
	// down). Overlap them so remove costs max(...) rather than their sum.
	cgDir := filepath.Join("/sys/fs/cgroup/firecracker", info.ID)
	done := make(chan struct{})
	go func() { reapCgroup(cgDir); close(done) }()

	chrootDir := filepath.Join(sbxhost.JailerBase(), "firecracker", info.ID)
	os.RemoveAll(chrootDir)
	<-done
	// The state file goes last: a leftover one makes `sbx remove` re-entrant.
	os.Remove(sbx.StatePath(info.ID))
	return nil
}

// spawnJailer launches the jailer which chroots and daemonizes Firecracker with
// cgroup v2 resource limits. Structured exec (argv slice, no shell).
// describeFile renders a path with its size, so a failure says which agent and
// kernel that sandbox was actually built from (SBX_AGENT_BIN and SBX_KERNEL
// change them per shell, which is otherwise invisible in a failure).
func describeFile(path string) string {
	st, err := os.Stat(path)
	if err != nil {
		return path + " (missing)"
	}
	return fmt.Sprintf("%s (%d bytes)", path, st.Size())
}

// pidState reports whether the VM process is still there, which tells a crash
// apart from a guest that is running but not serving.
func pidState(pid int) string {
	if pid > 0 && syscall.Kill(pid, 0) == nil {
		return "alive"
	}
	return "gone"
}

// firecrackerBootTimeSec parses Firecracker's --boot-timer log record. This is
// the official VM boot metric: the time from InstanceStart until the guest
// starts its init process. It deliberately does not include the agent or the
// /health readiness wait.
func firecrackerBootTimeSec(path string) (float64, bool) {
	data, err := os.ReadFile(path)
	if err != nil {
		return 0, false
	}
	text := string(data)
	const marker = "Guest-boot-time"
	pos := strings.LastIndex(text, marker)
	if pos < 0 {
		return 0, false
	}
	line := text[pos:]
	if end := strings.IndexByte(line, '\n'); end >= 0 {
		line = line[:end]
	}
	eq := strings.IndexByte(line, '=')
	if eq < 0 {
		return 0, false
	}
	fields := strings.Fields(line[eq+1:])
	if len(fields) == 0 {
		return 0, false
	}
	value, err := strconv.ParseFloat(fields[0], 64)
	if err != nil {
		return 0, false
	}
	unit := "us"
	if len(fields) > 1 {
		unit = strings.ToLower(fields[1])
	}
	switch unit {
	case "us":
		return value / 1e6, true
	case "ms":
		return value / 1e3, true
	case "s":
		return value, true
	default:
		return 0, false
	}
}

// linkState reports the host-side TAP's operstate: it is "up" once the guest's
// NIC is wired into the bridge side of the pair, so a "down" or missing TAP
// points at the host plumbing rather than at the guest.
func linkState(name string) string {
	b, err := os.ReadFile(filepath.Join("/sys/class/net", name, "operstate"))
	if err != nil {
		return "missing"
	}
	return strings.TrimSpace(string(b))
}

func spawnJailer(jailerPath, fcPath, id, base string, opt sbx.Options, numa sbx.NumaAllocation) error {
	sweepCgroups()
	args := []string{
		jailerPath,
		"--exec-file", fcPath,
		"--id", id,
		"--uid", "0", "--gid", "0",
		"--chroot-base-dir", base,
		"--daemonize", "--new-pid-ns",
	}
	if numa.CPUs != "" {
		args = append(args, "--cgroup", "cpuset.cpus="+numa.CPUs)
	}
	if numa.Mems != "" {
		args = append(args, "--cgroup", "cpuset.mems="+numa.Mems)
	}
	args = append(args,
		"--cgroup-version", "2",
		"--cgroup", fmt.Sprintf("cpu.max=%d 100000", opt.CPUs*100000),
		"--cgroup", fmt.Sprintf("memory.max=%d", (opt.MemoryMB+64)*1024*1024),
		"--",
		"--api-sock", "run/firecracker.socket",
		"--no-seccomp",
		/* firecracker's own log (API calls, device and vcpu errors). The guest
		 * console cannot be captured - the jailer daemonizes firecracker, whose
		 * serial output then goes to /dev/null - so this is the only VM-side
		 * record left when a sandbox does not come up. */
		"--log-path", "run/firecracker.log",
		"--level", "Info",
		"--boot-timer",
	)
	cmd := exec.Command(args[0], args[1:]...)
	if out, err := cmd.CombinedOutput(); err != nil {
		return fmt.Errorf("jailer failed: %s%s", string(out), err)
	}
	return nil
}
