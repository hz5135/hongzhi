package sbxhost

import (
	"fmt"
	"strconv"

	"github.com/coreos/go-iptables/iptables"

	"agentbench/sandbox/internal/sbx"
)

// portForwardRules returns the iptables ruleset that publishes the agent port
// on the host via DNAT (guest:9119). Mirrors the legacy ops list.
func portForwardRules(guest string, port int) [][]string {
	to := guest + ":9119"
	portStr := strconv.Itoa(port)
	return [][]string{
		{"nat", "PREROUTING", "-p", "tcp", "--dport", portStr, "-j", "DNAT", "--to-destination", to},
		{"nat", "POSTROUTING", "-p", "tcp", "-d", guest, "--dport", "9119", "-j", "MASQUERADE"},
		{"filter", "FORWARD", "-p", "tcp", "-d", guest, "--dport", "9119", "-j", "ACCEPT"},
		{"filter", "FORWARD", "-m", "state", "--state", "ESTABLISHED,RELATED", "-j", "ACCEPT"},
	}
}

func newIPTables() (*iptables.IPTables, error) {
	return iptables.NewWithProtocol(iptables.ProtocolIPv4)
}

// SetupPortForward installs the DNAT rules publishing the sandbox agent port.
func SetupPortForward(info sbx.Info) error {
	if info.PublishPort <= 0 {
		return nil
	}
	guest := info.GuestIP
	if guest == "" {
		guest = "198.19.0.2"
	}
	ipt, err := newIPTables()
	if err != nil {
		return err
	}
	rules := portForwardRules(guest, info.PublishPort)
	if err := ipt.AppendUnique(rules[0][0], rules[0][1], rules[0][2:]...); err != nil {
		return fmt.Errorf("iptables DNAT failed: %s", err)
	}
	for _, r := range rules[1:] {
		ipt.AppendUnique(r[0], r[1], r[2:]...)
	}
	return nil
}

// TearDownPortForward removes the DNAT rules for the sandbox.
func TearDownPortForward(info sbx.Info) {
	if info.PublishPort <= 0 {
		return
	}
	guest := info.GuestIP
	if guest == "" {
		guest = "198.19.0.2"
	}
	ipt, err := newIPTables()
	if err != nil {
		return
	}
	for _, r := range portForwardRules(guest, info.PublishPort) {
		ipt.DeleteIfExists(r[0], r[1], r[2:]...)
	}
}
