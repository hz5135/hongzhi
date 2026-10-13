package cli

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"sync"
	"sync/atomic"
	"time"

	"agentbench/sandbox/internal/agentclient"
	"golang.org/x/term"
)

// cmdExec implements `sbx exec [sandbox] [--timeout MS] [-it] -- <cmd> [args...]`.
func cmdExec(args []string) int {
	// Remove -it/-t/--tty anywhere BEFORE the command separator so resolveAgent
	// sees a clean arg list. args[0] is the command name; the rest are scanned.
	// Stop at the first `--`: everything after it belongs to the guest command
	// and must be preserved verbatim (e.g. `agentbench ... -t 2`).
	clean := []string{args[0]}
	useTty := false
	for k := 1; k < len(args); k++ {
		if args[k] == "--" {
			clean = append(clean, args[k:]...)
			break
		}
		switch args[k] {
		case "-it", "-t", "--tty":
			useTty = true
			continue
		}
		clean = append(clean, args[k])
	}

	ep := &agentEndpoint{port: 9119}
	if !resolveAgent(clean, 1, ep) {
		return 1
	}

	var cmd []string
	afterSep := false
	timeoutMs := 0
	for k := 1; k < len(clean); k++ {
		arg := clean[k]
		if !afterSep && arg == "--" {
			afterSep = true
			continue
		}
		if !afterSep && arg == "--timeout" && k+1 < len(clean) {
			k++
			timeoutMs = atoi(clean[k])
			continue
		}
		if afterSep {
			cmd = append(cmd, arg)
		}
	}
	if len(cmd) == 0 {
		fmt.Fprintf(os.Stderr, "error: no command provided (use '--' separator)\n")
		return 1
	}

	body := map[string]interface{}{
		"cmd":  cmd[0],
		"args": cmd[1:],
	}
	if timeoutMs > 0 {
		body["timeoutMs"] = timeoutMs
	}
	if useTty {
		body["tty"] = true
	}
	bodyJSON, _ := json.Marshal(body)

	// Put stdin in raw mode for interactive tty sessions.
	origFd := int(os.Stdin.Fd())
	if useTty && term.IsTerminal(origFd) {
		if state, err := term.MakeRaw(origFd); err == nil {
			defer term.Restore(origFd, state)
		}
	}

	client := clientFor(ep)

	var sessionDone atomic.Bool
	var cmdReady atomic.Bool
	var cmdIDMu sync.Mutex
	sharedCmdID := ""

	if useTty {
		go func() {
			defer sessionDone.Store(true)
			var pending bytes.Buffer
			buf := make([]byte, 4096)
			for {
				if sessionDone.Load() {
					return
				}
				os.Stdin.SetReadDeadline(time.Now().Add(100 * time.Millisecond))
				n, err := os.Stdin.Read(buf)
				if n > 0 {
					pending.Write(buf[:n])
				}
				cmdIDMu.Lock()
				cid := sharedCmdID
				cmdIDMu.Unlock()
				if pending.Len() > 0 && cmdReady.Load() && cid != "" {
					client.ExecStdin(cid, pending.String())
					pending.Reset()
				}
				if err != nil {
					if ne, ok := err.(interface{ Timeout() bool }); ok && ne.Timeout() {
						continue
					}
					return
				}
			}
		}()
	}

	exitCode := 1
	rc, err := client.Exec(string(bodyJSON), func(ev agentclient.Event) bool {
		switch ev.Type {
		case "started":
			if useTty && ev.ID != "" {
				cmdIDMu.Lock()
				sharedCmdID = ev.ID
				cmdIDMu.Unlock()
				cmdReady.Store(true)
				if w, h, err := term.GetSize(origFd); err == nil && w > 0 && h > 0 {
					client.ExecResize(ev.ID, h, w)
				}
			}
		case "stdout":
			if useTty {
				os.Stdout.WriteString(ev.Data)
				os.Stdout.Sync()
			} else {
				fmt.Println(ev.Data)
			}
		case "stderr":
			if useTty {
				os.Stderr.WriteString(ev.Data)
				os.Stderr.Sync()
			} else {
				fmt.Fprintln(os.Stderr, ev.Data)
			}
		case "exit":
			exitCode = ev.ExitCode
		case "error":
			fmt.Fprintf(os.Stderr, "error: %s\n", ev.Error)
			exitCode = 125
		case "timeout":
			fmt.Fprintln(os.Stderr, "error: command timed out")
			exitCode = 124
		}
		return true
	})
	sessionDone.Store(true)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		return 1
	}
	if rc != 0 {
		fmt.Fprintf(os.Stderr, "error: cannot connect to agent at %s:%d (status %d)\n", ep.host, ep.port, rc)
		return 1
	}
	return exitCode
}
