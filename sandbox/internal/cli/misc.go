package cli

import (
	"fmt"
	"os"

	"agentbench/sandbox/internal/sbx"
)

func cmdList() int {
	infos := sbx.List()
	if len(infos) == 0 {
		fmt.Println("no sandboxes")
		return 0
	}
	fmt.Printf("%-12s %-16s %-12s %-12s %-8s %-22s\n", "ID", "NAME", "BACKEND", "STATUS", "PORT", "IMAGE")
	for _, i := range infos {
		fmt.Printf("%-12s %-16s %-12s %-12s %-8d %-22s\n", i.ID, i.Name, i.Backend, i.Status, i.AgentPort, i.Image)
	}
	return 0
}

func cmdStop(args []string) int {
	if len(args) < 2 {
		fmt.Fprintf(os.Stderr, "error: sandbox id required\n")
		return 1
	}
	rc := 0
	for i := 1; i < len(args); i++ {
		if err := sbx.Stop(args[i]); err != nil {
			fmt.Fprintf(os.Stderr, "error: %s\n", err)
			rc = 1
		} else {
			fmt.Printf("stopped sandbox %s\n", args[i])
		}
	}
	return rc
}

func cmdRemove(args []string) int {
	if len(args) < 2 {
		fmt.Fprintf(os.Stderr, "error: sandbox id required\n")
		return 1
	}
	rc := 0
	for i := 1; i < len(args); i++ {
		if err := sbx.Remove(args[i]); err != nil {
			fmt.Fprintf(os.Stderr, "error: %s\n", err)
			rc = 1
		} else {
			fmt.Printf("removed sandbox %s\n", args[i])
		}
	}
	return rc
}
