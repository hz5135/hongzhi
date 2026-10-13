package sbxhost

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// Cgroup2Root is where the unified cgroup v2 hierarchy is mounted.
const Cgroup2Root = "/sys/fs/cgroup"

// cgroupV2At verifies that the cgroup hierarchy rooted at root is a v2 mount
// and that the controllers the microVM backends need for resource bounding
// (cpu, memory, plus any extra) are enabled in root's subtree_control.
//
// Read-only by design: it never mounts cgroup v2 and never enables a missing
// controller — it reports what is missing and how to enable it, so failures
// surface as clear errors instead of mid-flight cgroup writes.
func cgroupV2At(root string, extra ...string) error {
	rootControllers := filepath.Join(root, "cgroup.controllers")
	subtree := filepath.Join(root, "cgroup.subtree_control")
	// A v2 hierarchy always exposes cgroup.controllers at the root; a v1 mount
	// has per-controller directories (cpu/, memory/, ...) instead.
	if _, err := os.Stat(rootControllers); err != nil {
		return fmt.Errorf("cgroup v2 not enabled: %s not found; the microVM backends need a unified cgroup v2 hierarchy to bound VM resources (mount it, e.g.: mount -t cgroup2 none %s)", rootControllers, root)
	}
	enabled := map[string]bool{}
	if data, err := os.ReadFile(subtree); err == nil {
		for _, f := range strings.Fields(string(data)) {
			enabled[strings.TrimPrefix(f, "+")] = true
		}
	}
	want := append([]string{"cpu", "memory"}, extra...)
	missing := []string{}
	for _, c := range want {
		if !enabled[c] {
			missing = append(missing, c)
		}
	}
	if len(missing) > 0 {
		return fmt.Errorf("cgroup v2 controllers not enabled for VM resource limits: missing %s from %s; enable them at the root, e.g.: echo '+%s' > %s",
			strings.Join(missing, ", "), subtree, strings.Join(missing, " +"), subtree)
	}
	return nil
}

// CheckCgroupV2 verifies the host runs cgroup v2 with the controllers the
// microVM backends need. extra lists optional controllers to also require
// (e.g. "cpuset" when the backend pins CPUs/memory).
func CheckCgroupV2(extra ...string) error {
	return cgroupV2At(Cgroup2Root, extra...)
}
