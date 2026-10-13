package cli

import (
	"errors"
	"fmt"
	"os"

	"agentbench/sandbox/internal/sbx"
	"agentbench/sandbox/internal/sbxhost"
)

// cmdCreate implements `sbx create --backend B --image IMG [opts...]`.
func cmdCreate(args []string) int {
	var opt sbx.Options
	opt.Args = map[string]string{}
	opt.CPUs = 2
	opt.MemoryMB = 4096
	i := 0
	next := func() string {
		if i+1 < len(args) {
			i++
			return args[i]
		}
		return ""
	}
	for ; i < len(args); i++ {
		arg := args[i]
		key, value := arg, ""
		if eq := indexByte(arg, '='); eq >= 0 {
			key, value = arg[:eq], arg[eq+1:]
		}
		take := func(v string) string {
			if v != "" {
				return v
			}
			return next()
		}
		switch key {
		case "--backend":
			opt.Backend = take(value)
		case "--image":
			opt.Image = take(value)
		case "--name":
			opt.Name = take(value)
		case "--kernel":
			opt.Kernel = take(value)
		case "--cpus":
			opt.CPUs = atoi(take(value))
		case "--memory":
			opt.MemoryMB = atoi(take(value))
		case "--port":
			opt.Port = atoi(take(value))
		default:
			if len(key) > 2 && key[0] == '-' && key[1] == '-' {
				// Unknown --key: pass through to the backend.
				opt.Args[key[2:]] = take(value)
			}
		}
	}
	if opt.Backend == "" {
		fmt.Fprintf(os.Stderr, "error: --backend required %s\n", backendListText())
		return 1
	}
	if opt.Image == "" {
		fmt.Fprintf(os.Stderr, "error: --image required\n")
		return 1
	}
	var info sbx.Info
	if err := sbx.Create(opt, &info); err != nil {
		fmt.Fprintf(os.Stderr, "error: %s\n", err)
		/* A create whose environment is not prepared (no guest rootfs template
		   for this image) fails the same way every time: retrying cannot help,
		   the fix is in the message. Say so with a distinct exit code. */
		if errors.Is(err, sbx.ErrRootfsNotPrepared) {
			return sbx.ExitRootfsNotReady
		}
		return 1
	}
	fmt.Printf("created sandbox %s (%s, image=%s)\n", info.Name, info.Backend, info.Image)
	if info.AgentHost != "" {
		fmt.Printf("  agent: http://%s:%d token=%s\n", info.AgentHost, info.AgentPort, info.AgentToken)
	}
	fmt.Printf("  id: %s\n", info.ID)
	// Machine-readable phase timing. `startup` is the whole bring-up up to the
	// guest's own init - provision + host_setup + boot, in the order they
	// happen - and is the number to compare with another sandbox's startup; the
	// three parts are printed next to it, so it can be taken apart again
	// (startup - provision - host_setup = boot). `userspace` is init -> the
	// agent answering, and `ready` is the host wall time from the VM bring-up
	// to a healthy agent (host_setup + boot + userspace), i.e. the sandbox's
	// usable latency. Backends without a step report -1 (treated as n/a).
	if info.Meta != nil {
		p, hs := info.Meta["provision_sec"], info.Meta["host_setup_sec"]
		s, b := info.Meta["startup_sec"], info.Meta["boot_sec"]
		u, r := info.Meta["userspace_sec"], info.Meta["ready_sec"]
		if p != "" || hs != "" || s != "" || b != "" || u != "" || r != "" {
			fmt.Printf("  timing: startup=%s provision=%s host_setup=%s boot=%s userspace=%s ready=%s\n",
				metaOrNA(s), metaOrNA(p), metaOrNA(hs), metaOrNA(b), metaOrNA(u), metaOrNA(r))
			if source := info.Meta["startup_source"]; source != "" {
				fmt.Println("  startup source:", source)
			}
		}
	}
	return 0
}

// metaOrNA renders an absent timing field as -1, which is how the create
// timing line and the harness both mean "not measured".
func metaOrNA(v string) string {
	if v == "" {
		return "-1"
	}
	return v
}

// cmdDoctor implements `sbx doctor [--backend B]`.
func cmdDoctor(args []string) int {
	only := ""
	for i := 0; i < len(args); i++ {
		arg := args[i]
		if (arg == "--backend" || arg == "-b") && i+1 < len(args) {
			only = args[i+1]
			i++
		} else if len(arg) > 10 && arg[:10] == "--backend=" {
			only = arg[10:]
		}
	}
	var backends []string
	if only != "" {
		backends = []string{only}
	} else {
		backends = sbx.RegisteredBackends()
	}
	fails := 0
	for _, backend := range backends {
		err := sbx.CheckEnv(backend)
		if err == nil {
			fmt.Printf("backend %-12s OK\n", backend)
		} else {
			fmt.Printf("backend %-12s FAIL: %s\n", backend, err)
			fails++
		}
	}
	/* The microVM backends boot a guest rootfs template that `make rootfs` builds
	   from the current image, so it belongs to the environment they need: report
	   it here rather than let the first create stop with the same message. */
	if microVMsIn(backends) {
		image := defaultRootfsImage()
		st := sbxhost.RootfsStatus(image, sbx.SandboxAgentBin())
		if st.BaseCurrent() && st.InjectedCurrent() {
			fmt.Printf("guest rootfs %-12s OK (%s)\n", image, sbxhost.ShortID(st.ImageID))
		} else {
			fmt.Printf("guest rootfs %-12s FAIL: %s\n", image, rootfsVerdict(st))
			fails++
		}
		/* These backends reach the agent directly, at the guest's own
		   198.19.x.y over the sandbox TAP, and the probe must therefore never
		   consult the environment's proxy (see sbx/agentTransport). Report
		   what the environment does set anyway: it is environment, which is
		   what the doctor is for, and a proxy that captured those probes is
		   the one cause of a failed bring-up with no visible symptom on the
		   host side at all - the VM alive, the TAP up and the guest's agent
		   already listening, while every probe times out. It is reported, not
		   failed on: with the probe fixed, a proxy is harmless here. */
		fmt.Printf("agent proxy  %s\n", envProxySummary())
	}
	if fails > 0 {
		return 1
	}
	return 0
}

// envProxySummary names the proxy variable the invoking environment sets for
// HTTP, if any. Host -> guest agent requests are always sent directly, so this
// is reported rather than acted on.
func envProxySummary() string {
	for _, k := range []string{"HTTP_PROXY", "http_proxy", "HTTPS_PROXY", "https_proxy"} {
		if v := os.Getenv(k); v != "" {
			return k + "=" + v + " set - host -> guest agent requests are sent directly, never through it"
		}
	}
	return "none set"
}

// microVMsIn reports whether the checked backends boot a guest rootfs template
// (the microVM backends do, the container backends do not).
func microVMsIn(backends []string) bool {
	for _, b := range backends {
		if b == "firecracker" {
			return true
		}
	}
	return false
}

func indexByte(s string, b byte) int {
	for i := 0; i < len(s); i++ {
		if s[i] == b {
			return i
		}
	}
	return -1
}
