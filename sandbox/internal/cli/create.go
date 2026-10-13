package cli

import (
	"fmt"
	"os"

	"agentbench/sandbox/internal/sbx"
	"agentbench/sandbox/internal/sbxhost"
)

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
