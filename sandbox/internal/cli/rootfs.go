package cli

import (
	"fmt"
	"os"
	"strings"

	"agentbench/sandbox/internal/sbx"
	"agentbench/sandbox/internal/sbxhost"
)

// defaultRootfsImage is the image `sbx rootfs` works on when --image is not
// given: the same default the harness honours through SBX_IMAGE.
func defaultRootfsImage() string {
	if v := os.Getenv("SBX_IMAGE"); v != "" {
		return v
	}
	return "agentbench:latest"
}

// cmdRootfs implements `sbx rootfs build|status`.
//
// The guest rootfs the microVM backends boot (rootfs.ext4 + the agent-injected
// injected.ext4) is built here - `make rootfs` runs this - and cached per
// image:tag under ~/.sbx/registry/rootfs/. Creating a sandbox only ever copies
// that finished template: building one is tens of seconds of one-time work
// (docker build -> docker export -> mkfs.ext4), which must not land inside a
// benchmark run, and under `-n N` every parallel create would otherwise wake its
// own build. It is a separate step from `make image` because either side can
// change on its own (a rebuilt image, or a rebuilt sbx-agent).
func cmdRootfs(args []string) int {
	sub := ""
	image := defaultRootfsImage()
	force := false
	for i := 1; i < len(args); i++ {
		arg := args[i]
		switch {
		case arg == "--image" || arg == "-i":
			if i+1 < len(args) {
				i++
				image = args[i]
			}
		case strings.HasPrefix(arg, "--image="):
			image = strings.TrimPrefix(arg, "--image=")
		case arg == "--force" || arg == "-f":
			force = true
		case arg == "build" || arg == "status":
			sub = arg
		}
	}
	if sub == "" {
		fmt.Fprintf(os.Stderr, "usage: sbx rootfs build|status [--image IMG] [--force]\n")
		return 1
	}

	agentBin := sbx.SandboxAgentBin()
	if sub == "status" {
		st := sbxhost.RootfsStatus(image, agentBin)
		printRootfs(st, rootfsVerdict(st))
		if st.BaseCurrent() && st.InjectedCurrent() {
			return 0
		}
		return 1
	}

	build, err := sbxhost.PrepareRootfs(image, agentBin, force)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %s\n", err)
		return 1
	}
	verdict := "up to date"
	if build.BaseRebuilt || build.InjectedRebuilt {
		verdict = fmt.Sprintf("rebuilt in %.1fs (base %.1fs, agent %.1fs)",
			build.BaseSec+build.InjectSec, build.BaseSec, build.InjectSec)
	}
	printRootfs(build.State, verdict)
	return 0
}

func printRootfs(st sbxhost.RootfsState, verdict string) {
	fmt.Printf("guest rootfs for %s: %s\n", st.Image, verdict)
	fmt.Printf("  image     %s\n", sbxhost.ShortID(st.ImageID))
	fmt.Printf("  cache     %s\n", st.CacheDir)
	fmt.Printf("  base      rootfs.ext4   %s  %s\n", humanSize(st.BaseSize), rootfsBaseNote(st))
	fmt.Printf("  injected  injected.ext4 %s  %s\n", humanSize(st.InjectedSize), rootfsInjectedNote(st))
}

// rootfsVerdict is the one-line state of a template, with the command that
// changes it.
func rootfsVerdict(st sbxhost.RootfsState) string {
	switch {
	case st.BaseID == "":
		return "missing - run `make rootfs`"
	case !st.BaseCurrent():
		return fmt.Sprintf("stale (built from image %s) - run `make rootfs`",
			sbxhost.ShortID(st.BaseID))
	case !st.InjectedCurrent():
		return "the injected template is stale - run `sbx rootfs build`"
	}
	return "up to date"
}

func rootfsBaseNote(st sbxhost.RootfsState) string {
	if st.BaseID == "" {
		return "never built"
	}
	note := "from image " + sbxhost.ShortID(st.BaseID)
	if !st.BaseCurrent() {
		return note + " (stale: " + sbxhost.ShortID(st.ImageID) + " now)"
	}
	return note
}

func rootfsInjectedNote(st sbxhost.RootfsState) string {
	if !sbx.FileExists(st.Injected) {
		return "never built"
	}
	note := "sbx-agent " + sbxhost.ShortID(st.InjectedHash)
	if st.InjectedID != st.BaseID {
		return note + " (stale: base is " + sbxhost.ShortID(st.BaseID) + ")"
	}
	if !st.InjectedLayoutCurrent() {
		return note + " (stale: injected by an older layout)"
	}
	if st.AgentHash != "" && st.InjectedHash != st.AgentHash {
		return note + " (stale: sbx-agent is " + sbxhost.ShortID(st.AgentHash) + " now)"
	}
	return note
}

func humanSize(n int64) string {
	if n <= 0 {
		return "-"
	}
	const (
		kib = 1024
		mib = 1024 * kib
		gib = 1024 * mib
	)
	switch {
	case n >= gib:
		return fmt.Sprintf("%.2f GiB", float64(n)/float64(gib))
	case n >= mib:
		return fmt.Sprintf("%.1f MiB", float64(n)/float64(mib))
	default:
		return fmt.Sprintf("%d KiB", n/kib)
	}
}
