package sbxhost

import (
	"fmt"
	"net"
	"testing"
)

func TestHostIPFor(t *testing.T) {
	tests := []struct {
		slot int
		want string
	}{
		{0, "198.19.0.1"},
		{1, "198.19.0.5"},
		{10, "198.19.0.41"},
		{63, "198.19.0.253"},
		{64, "198.19.1.1"},
		{255, "198.19.3.253"},
		{256, "198.19.4.1"},
		{MaxNetworkSlots - 1, "198.19.255.253"},
	}
	for _, tt := range tests {
		got := HostIPFor(tt.slot)
		if got != tt.want {
			t.Errorf("HostIPFor(%d) = %q, want %q", tt.slot, got, tt.want)
		}
		if net.ParseIP(got) == nil {
			t.Errorf("HostIPFor(%d) returned invalid IP: %q", tt.slot, got)
		}
	}
}

func TestGuestIPFor(t *testing.T) {
	tests := []struct {
		slot int
		want string
	}{
		{0, "198.19.0.2"},
		{1, "198.19.0.6"},
		{10, "198.19.0.42"},
		{63, "198.19.0.254"},
		{64, "198.19.1.2"},
		{255, "198.19.3.254"},
		{256, "198.19.4.2"},
		{MaxNetworkSlots - 1, "198.19.255.254"},
	}
	for _, tt := range tests {
		got := GuestIPFor(tt.slot)
		if got != tt.want {
			t.Errorf("GuestIPFor(%d) = %q, want %q", tt.slot, got, tt.want)
		}
		if net.ParseIP(got) == nil {
			t.Errorf("GuestIPFor(%d) returned invalid IP: %q", tt.slot, got)
		}
	}
}

func TestMACFor(t *testing.T) {
	tests := []struct {
		slot int
		want string
	}{
		{0, "AA:FC:00:00:00:01"},
		{1, "AA:FC:00:00:00:02"},
		{10, "AA:FC:00:00:00:0B"},
		{254, "AA:FC:00:00:00:FF"},
		{255, "AA:FC:00:00:01:00"},
		{256, "AA:FC:00:00:01:01"},
		{511, "AA:FC:00:00:02:00"},
		{MaxNetworkSlots - 1, "AA:FC:00:00:40:00"},
	}
	for _, tt := range tests {
		got := MACFor(tt.slot)
		if got != tt.want {
			t.Errorf("MACFor(%d) = %q, want %q", tt.slot, got, tt.want)
		}
		if _, err := net.ParseMAC(got); err != nil {
			t.Errorf("MACFor(%d) returned invalid MAC: %q (%v)", tt.slot, got, err)
		}
	}
}

func TestSlotIPConsistency(t *testing.T) {
	// Host and guest IPs must be in the same /30 subnet (base slot*4), and each
	// slot must own a distinct subnet.
	seen := map[string]bool{}
	for slot := 0; slot < MaxNetworkSlots; slot += 997 { // sampled, incl. boundaries
		host := net.ParseIP(HostIPFor(slot))
		guest := net.ParseIP(GuestIPFor(slot))
		if host.To4() == nil || guest.To4() == nil {
			t.Fatalf("invalid IPs for slot %d: host=%s guest=%s", slot, host, guest)
		}
		// Widen each octet to uint32 before shifting: a byte operand's shift
		// result is a byte, so `To4()[0]<<24` truncates to 0 and the base
		// would be the last octet alone — a check that passes without ever
		// looking at the upper octets.
		o := host.To4()
		netBase := uint32(o[0])<<24 | uint32(o[1])<<16 | uint32(o[2])<<8 | uint32(o[3])
		netBase--
		key := fmt.Sprintf("%08x", netBase&^3)
		if seen[key] {
			t.Errorf("slot %d: subnet %s already used", slot, key)
		}
		seen[key] = true
	}
	// MaxNetworkSlots - 1 is the last usable /30 in 198.19.0.0/16.
	if net.ParseIP(GuestIPFor(MaxNetworkSlots - 1)).To4()[3] != 254 {
		t.Errorf("last slot does not end at 198.19.255.254/30")
	}
	if _, err := net.ParseMAC(MACFor(MaxNetworkSlots - 1)); err != nil {
		t.Errorf("last slot MAC invalid: %v", err)
	}
}
