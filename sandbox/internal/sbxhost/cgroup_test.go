package sbxhost

import (
	"os"
	"path/filepath"
	"testing"
)

// writeCgroupRoot builds a fake cgroup v2 hierarchy under dir:
//
//	dir/cgroup.controllers
//	dir/cgroup.subtree_control
func writeCgroupRoot(t *testing.T, controllers, subtree string) string {
	t.Helper()
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "cgroup.controllers"), []byte(controllers), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "cgroup.subtree_control"), []byte(subtree), 0o644); err != nil {
		t.Fatal(err)
	}
	return dir
}

func TestCgroupV2At(t *testing.T) {
	// v2 with the required controllers enabled.
	dir := writeCgroupRoot(t, "cpuset cpu memory io", "cpuset cpu memory io")
	if err := cgroupV2At(dir); err != nil {
		t.Errorf("cgroupV2At(v2 with cpu+memory) = %v, want nil", err)
	}
	if err := cgroupV2At(dir, "cpuset"); err != nil {
		t.Errorf("cgroupV2At(..., cpuset) = %v, want nil", err)
	}

	// v2 but a required controller is not enabled.
	dir = writeCgroupRoot(t, "cpuset cpu io", "cpuset cpu io")
	if err := cgroupV2At(dir); err == nil {
		t.Error("cgroupV2At(missing memory) = nil, want error")
	}

	// v2 but the extra (optional) controller is not enabled.
	dir = writeCgroupRoot(t, "cpu memory", "cpu memory")
	if err := cgroupV2At(dir, "cpuset"); err == nil {
		t.Error("cgroupV2At(..., missing cpuset) = nil, want error")
	}

	// v1-style / non-cgroup hierarchy: no cgroup.controllers file.
	dir = t.TempDir()
	if err := cgroupV2At(dir); err == nil {
		t.Error("cgroupV2At(no cgroup.controllers) = nil, want error")
	}

	// subtree_control unreadable/missing is not fatal by itself (still v2),
	// but the controllers are reported as missing.
	dir = t.TempDir()
	os.WriteFile(filepath.Join(dir, "cgroup.controllers"), []byte("cpu memory"), 0o644)
	if err := cgroupV2At(dir); err == nil {
		t.Error("cgroupV2At(no subtree_control) = nil, want error")
	}
}
