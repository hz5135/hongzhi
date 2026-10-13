// Package cli implements the sbx command-line frontend. It keeps the exact
// same command surface and output format as the legacy C++ CLI so the
// agentbench host driver (src/main.cpp) and scripts/run-*.sh keep working.
package cli

import (
	"fmt"
	"os"
	"strings"

	"agentbench/sandbox/internal/agentclient"
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
	case "health":
		return cmdHealth(args)
	case "exec":
		return cmdExec(args)
	case "upload":
		return cmdUpload(args)
	case "download":
		return cmdDownload(args)
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

// agentEndpoint is a resolved agent connection (from a sandbox id or explicit
// --host/--port/--token).
type agentEndpoint struct {
	id    string
	host  string
	port  int
	token string
}

// resolveAgent scans args starting at index i for either a sandbox id (loaded
// from ~/.sbx state) or explicit --host/--port/--token, stopping at "--" or
// the first non-flag token that is not the id. On success it updates i to the
// position after the consumed arguments.
func resolveAgent(args []string, i int, ep *agentEndpoint) bool {
	direct := false
	for ; i < len(args); i++ {
		arg := args[i]
		if arg == "--" {
			break
		}
		if arg == "--host" && i+1 < len(args) {
			ep.host = args[i+1]
			i++
			direct = true
			continue
		}
		if arg == "--port" && i+1 < len(args) {
			ep.port = atoi(args[i+1])
			i++
			direct = true
			continue
		}
		if arg == "--token" && i+1 < len(args) {
			ep.token = args[i+1]
			i++
			direct = true
			continue
		}
		if strings.HasPrefix(arg, "-") && arg != "-" {
			break
		}
		if !direct && ep.id == "" {
			ep.id = arg
			continue
		}
		break
	}
	if !direct {
		if ep.id == "" {
			fmt.Fprintf(os.Stderr, "error: no sandbox id or --host given\n")
			return false
		}
		info := sbx.Load(ep.id)
		if info.ID == "" {
			fmt.Fprintf(os.Stderr, "error: no state file for '%s'\n", ep.id)
			return false
		}
		ep.host = info.AgentHost
		ep.port = info.AgentPort
		ep.token = info.AgentToken
		if ep.host == "" || ep.token == "" {
			fmt.Fprintf(os.Stderr, "error: state for '%s' has no agent endpoint\n", ep.id)
			return false
		}
	} else if ep.token == "" {
		fmt.Fprintf(os.Stderr, "error: no --token given\n")
		return false
	}
	return true
}

func clientFor(ep *agentEndpoint) *agentclient.Client {
	return agentclient.New(ep.host, ep.port, ep.token)
}

func backendListText() string {
	names := sbx.RegisteredBackends()
	return "(" + strings.Join(names, "|") + ")"
}

func usage() {
	fmt.Printf("Usage: sbx <command> [options]\n" +
		"\n" +
		"Commands:\n" +
		"  health    [sandbox] | --host H --port P --token T\n" +
		"  exec      [sandbox] | --host H --port P --token T [--timeout MS] [-it] -- <cmd> [args...]\n" +
		"  upload    [sandbox] -- <file>... [-d dest]\n" +
		"  download  [sandbox] <remote> [-o local]\n")
	fmt.Printf("  stop      <sandbox>...\n" +
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
