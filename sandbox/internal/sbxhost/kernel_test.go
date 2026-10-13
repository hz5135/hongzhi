package sbxhost

import (
	"os"
	"path/filepath"
	"runtime"
	"testing"
)

func TestDefaultKernelName(t *testing.T) {
	tests := []struct {
		microvm string
		want    string
	}{
		{"firecracker", "bzImage"},      // default on amd64 (arm64 returns "Image")
		{"cloud-hypervisor", "bzImage"}, // default on amd64 (arm64 returns "Image")
		{"anything-else", "vmlinux"},
	}
	// On arm64, firecracker and cloud-hypervisor return "Image" instead of
	// "vmlinux"/"bzImage".
	if runtime.GOARCH == "arm64" {
		tests[0].want = "Image"
		tests[1].want = "Image"
	}
	for _, tt := range tests {
		got := DefaultKernelName(tt.microvm)
		if got != tt.want {
			t.Errorf("DefaultKernelName(%q) = %q, want %q", tt.microvm, got, tt.want)
		}
	}
}

func TestIsElf(t *testing.T) {
	dir := t.TempDir()

	elf := filepath.Join(dir, "vmlinux")
	os.WriteFile(elf, append([]byte{0x7f, 'E', 'L', 'F'}, make([]byte, 100)...), 0o644)
	if !IsElf(elf) {
		t.Error("IsElf returned false for ELF file")
	}

	pe := filepath.Join(dir, "Image")
	os.WriteFile(pe, append([]byte{'M', 'Z'}, make([]byte, 100)...), 0o644)
	if IsElf(pe) {
		t.Error("IsElf returned true for PE file")
	}

	empty := filepath.Join(dir, "empty")
	os.WriteFile(empty, []byte{}, 0o644)
	if IsElf(empty) {
		t.Error("IsElf returned true for empty file")
	}

	if IsElf("/nonexistent/path") {
		t.Error("IsElf returned true for nonexistent file")
	}
}

func TestIsPe(t *testing.T) {
	dir := t.TempDir()

	pe := filepath.Join(dir, "Image")
	os.WriteFile(pe, append([]byte{'M', 'Z'}, make([]byte, 100)...), 0o644)
	if !IsPe(pe) {
		t.Error("IsPe returned false for PE file")
	}

	elf := filepath.Join(dir, "vmlinux")
	os.WriteFile(elf, append([]byte{0x7f, 'E', 'L', 'F'}, make([]byte, 100)...), 0o644)
	if IsPe(elf) {
		t.Error("IsPe returned true for ELF file")
	}

	empty := filepath.Join(dir, "empty")
	os.WriteFile(empty, []byte{}, 0o644)
	if IsPe(empty) {
		t.Error("IsPe returned true for empty file")
	}

	if IsPe("/nonexistent/path") {
		t.Error("IsPe returned true for nonexistent file")
	}
}

func TestIsElfOrPe(t *testing.T) {
	dir := t.TempDir()

	elf := filepath.Join(dir, "vmlinux")
	os.WriteFile(elf, append([]byte{0x7f, 'E', 'L', 'F'}, make([]byte, 100)...), 0o644)
	if !IsElfOrPe(elf) {
		t.Error("IsElfOrPe returned false for ELF file")
	}

	pe := filepath.Join(dir, "Image")
	os.WriteFile(pe, append([]byte{'M', 'Z'}, make([]byte, 100)...), 0o644)
	if !IsElfOrPe(pe) {
		t.Error("IsElfOrPe returned false for PE file")
	}

	other := filepath.Join(dir, "other")
	os.WriteFile(other, []byte{0x00, 0x00, 0x00, 0x00}, 0o644)
	if IsElfOrPe(other) {
		t.Error("IsElfOrPe returned true for non-ELF/PE file")
	}

	if IsElfOrPe("/nonexistent/path") {
		t.Error("IsElfOrPe returned true for nonexistent file")
	}
}

func TestKernelFormatOK(t *testing.T) {
	dir := t.TempDir()

	elf := filepath.Join(dir, "vmlinux")
	os.WriteFile(elf, append([]byte{0x7f, 'E', 'L', 'F'}, make([]byte, 100)...), 0o644)

	pe := filepath.Join(dir, "Image")
	os.WriteFile(pe, append([]byte{'M', 'Z'}, make([]byte, 100)...), 0o644)

	other := filepath.Join(dir, "other")
	os.WriteFile(other, []byte{0x00}, 0o644)

	type testCase struct {
		name    string
		microvm string
		path    string
		want    bool
	}
	tests := []testCase{
		{"firecracker ELF", "firecracker", elf, true},
		{"firecracker PE", "firecracker", pe, true},
		{"firecracker invalid", "firecracker", other, false},
	}
	if runtime.GOARCH == "arm64" {
		// cloud-hypervisor on arm64: PE only
		tests = append(tests,
			testCase{"clh PE (arm64)", "cloud-hypervisor", pe, true},
			testCase{"clh ELF (arm64)", "cloud-hypervisor", elf, false},
			testCase{"clh invalid (arm64)", "cloud-hypervisor", other, false},
		)
	} else {
		// cloud-hypervisor on amd64: ELF or PE
		tests = append(tests,
			testCase{"clh ELF (amd64)", "cloud-hypervisor", elf, true},
			testCase{"clh PE (amd64)", "cloud-hypervisor", pe, true},
			testCase{"clh invalid (amd64)", "cloud-hypervisor", other, false},
		)
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := KernelFormatOK(tt.microvm, tt.path)
			if got != tt.want {
				t.Errorf("KernelFormatOK(%q, %q) = %v, want %v", tt.microvm, tt.path, got, tt.want)
			}
		})
	}
}
