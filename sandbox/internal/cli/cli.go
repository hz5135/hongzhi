// Package cli implements the sbx command-line frontend. It keeps the exact
// same command surface and output format as the legacy C++ CLI so the
// agentbench host driver (src/main.cpp) and scripts/run-*.sh keep working.
package cli

import (
	"fmt"
	"os"
	"strings"

	"agentbench/sandbox/internal/sbx"
)

// Run dispatches an argv list (including argv[0]) to the matching command and
// returns the process exit code.
func Run(argv []string) int {
	if len(argv) < 2 {
		usage()
		return 1
	}
	sbx.SetBinDir(dirname(argv[0]))
	cmd := argv[1]
	args := argv[1:]
	switch cmd {
	case "stop":
		return cmdStop(args)
	case "remove":
		return cmdRemove(args)
	case "list":
		return cmdList()
	case "doctor":
		return cmdDoctor(args)
	case "rootfs":
		return cmdRootfs(args)
	case "help", "-h", "--help":
		usage()
		return 0
	}
	fmt.Fprintf(os.Stderr, "error: unknown command '%s'\n", cmd)
	usage()
	return 1
}

func dirname(path string) string {
	if i := strings.LastIndexByte(path, '/'); i >= 0 {
		return path[:i]
	}
	return "."
}

func backendListText() string {
	names := sbx.RegisteredBackends()
	return "(" + strings.Join(names, "|") + ")"
}

func usage() {
	fmt.Printf("Usage: sbx <command> [options]\n" +
		"\n" +
		"Commands:\n" +
		"  stop      <sandbox>...\n" +
		"  remove    <sandbox>...\n" +
		"  list\n")
	fmt.Printf("  doctor    [--backend %s]   verify backend environment\n",
		backendListText())
	fmt.Printf("  rootfs    build|status [--image IMG] [--force]\n" +
		"            the bootable guest rootfs the microVM backends use; built by\n" +
		"            `make rootfs`, never by a create (status: current template + fix)\n")
	fmt.Printf("\n" +
		"A sandbox may be referenced by its id/name (state under ~/.sbx/vms/)\n" +
		"or by explicit --host/--port/--token for agent connection.\n")
}

func atoi(s string) int {
	n := 0
	neg := false
	for i, c := range s {
		if i == 0 && c == '-' {
			neg = true
			continue
		}
		if c < '0' || c > '9' {
			break
		}
		n = n*10 + int(c-'0')
	}
	if neg {
		return -n
	}
	return n
}
