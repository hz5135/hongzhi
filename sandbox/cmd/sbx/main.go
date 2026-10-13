// sbx is the agentbench sandbox CLI. It provisions disposable isolated
// environments (Firecracker microVMs, or Docker containers) that run the
// benchmark suite through an HTTP agent.
package main

import (
	"os"

	"agentbench/sandbox/internal/cli"

	// Backend registration: each backend self-registers in its init(), so a
	// backend exists exactly when its package is imported here. This list is
	// the only place the set of available backends is written down — the CLI's
	// `--backend` choices and `doctor` both read the registry rather than a
	// name hardcoded into the CLI.
	_ "agentbench/sandbox/internal/backend/docker"
	_ "agentbench/sandbox/internal/backend/firecracker"
)

func main() {
	os.Exit(cli.Run(os.Args))
}
