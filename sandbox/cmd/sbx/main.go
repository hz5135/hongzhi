// sbx is the agentbench sandbox CLI. It provisions disposable isolated
// environments and runs the benchmark suite in them through an HTTP agent.
//
// A backend self-registers with sbx.Register() from its package init(), and a
// backend exists exactly when its package is imported here — this import list
// is the only place the set of available backends is written down. Nothing else
// in the tree names a backend: internal/sbx dispatches on the name stored in the
// sandbox record, and the CLI's `--backend` choices and `doctor` both read the
// registry. Backend packages are imported for that side effect alone.
package main

import (
	"os"

	"agentbench/sandbox/internal/cli"
)

func main() {
	os.Exit(cli.Run(os.Args))
}
