package sbxhost

import (
	"net"
	"os"

	"github.com/vishvananda/netlink"
)

// SetupTap creates a TAP device with the given host IP /30, brings it up and
// enables IPv4 forwarding, using netlink directly (no `ip` command).
func SetupTap(tap, hostIP string) error {
	if l, err := netlink.LinkByName(tap); err == nil {
		netlink.LinkDel(l)
	}
	la := netlink.NewLinkAttrs()
	la.Name = tap
	tuntap := &netlink.Tuntap{LinkAttrs: la, Mode: netlink.TUNTAP_MODE_TAP}
	if err := netlink.LinkAdd(tuntap); err != nil {
		return err
	}
	link, err := netlink.LinkByName(tap)
	if err != nil {
		return err
	}
	addr := &netlink.Addr{
		IPNet: &net.IPNet{IP: net.ParseIP(hostIP), Mask: net.CIDRMask(30, 32)},
	}
	if err := netlink.AddrAdd(link, addr); err != nil {
		return err
	}
	if err := netlink.LinkSetUp(link); err != nil {
		return err
	}
	os.WriteFile("/proc/sys/net/ipv4/ip_forward", []byte("1"), 0o644)
	// The host starts probing the guest (/health) as soon as the VM boots,
	// usually before the guest NIC is up. With the default 1 s ARP retransmit
	// the queued probe only reaches the guest on the next retransmit, adding a
	// full second to startup. Retransmit quickly so the guest becomes reachable
	// within ~100 ms of its interface coming up.
	os.WriteFile("/proc/sys/net/ipv4/neigh/"+tap+"/retrans_time_ms", []byte("100"), 0o644)
	return nil
}

// TearDownTap deletes the TAP device if it exists.
func TearDownTap(tap string) {
	if tap == "" {
		return
	}
	if l, err := netlink.LinkByName(tap); err == nil {
		netlink.LinkDel(l)
	}
}
