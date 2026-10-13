package sbxhost

import (
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"agentbench/sandbox/internal/sbx"
)

// MaxNetworkSlots is the largest number of concurrently networked sandboxes.
// Each slot owns a /30 inside 198.19.0.0/16 (4 addresses), so the ceiling is
// 65536/4 = 16384 slots (0..16383).
const MaxNetworkSlots = 1 << 14

// FindSlot atomically claims a free network slot via an O_CREAT|O_EXCL lock
// file under ~/.sbx/slots/, so concurrent creates never share a slot.
func FindSlot() (int, string, error) {
	dir := filepath.Join(sbx.HomeDir(), "slots")
	if err := sbx.Mkdirs(dir); err != nil {
		return -1, "", err
	}
	for slot := 0; slot < MaxNetworkSlots; slot++ {
		p := filepath.Join(dir, fmt.Sprintf("slot%d.lock", slot))
		f, err := os.OpenFile(p, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0o644)
		if err == nil {
			f.Close()
			return slot, p, nil
		}
	}
	return -1, "", fmt.Errorf("no free network slot")
}

func ReleaseSlot(lockPath string) {
	if lockPath != "" {
		os.Remove(lockPath)
	}
}

// ipFor returns the host/guest address of a slot's /30 subnet. The subnet base
// is slot*4 within 198.19.0.0/16, so adjacent slots never collide and the space
// is fully usable (slot 0 stays 198.19.0.1/.2 for backwards compatibility).
func ipFor(slot, off int) string {
	base := slot * 4
	return fmt.Sprintf("198.19.%d.%d", base>>8, base&0xff+off)
}

func HostIPFor(slot int) string  { return ipFor(slot, 1) }
func GuestIPFor(slot int) string { return ipFor(slot, 2) }

// MACFor encodes the slot into the last two bytes, so slots beyond 255 get a
// unique, valid unicast MAC (a single byte would wrap to 0x00 at slot 255).
func MACFor(slot int) string {
	n := slot + 1
	return fmt.Sprintf("AA:FC:00:00:%02X:%02X", byte(n>>8), byte(n))
}

// FindVmPid scans /proc/*/cmdline for a firecracker process of the given VM id
// (used by the firecracker backend, whose process is daemonized by the jailer).
func FindVmPid(id string) int {
	entries, err := os.ReadDir("/proc")
	if err != nil {
		return -1
	}
	for _, e := range entries {
		name := e.Name()
		if name == "" || name[0] < '0' || name[0] > '9' {
			continue
		}
		cmdline, cerr := os.ReadFile(filepath.Join("/proc", name, "cmdline"))
		if cerr != nil {
			continue
		}
		cmd := strings.ReplaceAll(string(cmdline), "\x00", " ")
		if strings.Contains(cmd, "firecracker") && strings.Contains(cmd, id) {
			pid, _ := strconv.Atoi(name)
			return pid
		}
	}
	return -1
}

// KillPid sends SIGKILL to the process (best effort).
func KillPid(pid int) {
	if pid <= 0 {
		return
	}
	if p, err := os.FindProcess(pid); err == nil {
		p.Kill()
	}
}
