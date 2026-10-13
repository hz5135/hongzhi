package sbxhost

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"

	"agentbench/sandbox/internal/sbx"
)

// DefaultKernelName returns the conventional kernel filename for a microVM
// family on the current architecture.
//
// Both firecracker and cloud-hypervisor load a `bzImage` on x86_64 and the
// arm64 build product `Image` (a PE/EFI image) on aarch64 — the same
// per-arch kernel name. Anything else (the container backends' default, or an
// unknown microVM type) falls back to `vmlinux`.
func DefaultKernelName(microvm string) string {
	if microvm == "cloud-hypervisor" || microvm == "firecracker" {
		if runtime.GOARCH == "arm64" {
			return "Image"
		}
		return "bzImage"
	}
	return "vmlinux"
}

// ResolveKernel resolves the kernel for a microVM family. Order:
// absolute path > ~/.sbx/kernels/<microvm>/<name> > SBX_KERNEL > legacy flat
// ~/.sbx/kernels/<name>. When name is empty the family's conventional filename
// (see DefaultKernelName) is used; for backwards compatibility, if that is
// missing under the family dir, `vmlinux` is tried as a fallback (the backend
// still format-checks the result).
func ResolveKernel(microvm, name string) (string, error) {
	kernelsDir := filepath.Join(sbx.HomeDir(), "kernels")
	sbx.Mkdirs(kernelsDir)
	kernelFile := DefaultKernelName(microvm)
	if name != "" {
		kernelFile = name
	}
	if strings.HasPrefix(kernelFile, "/") {
		if !sbx.FileExists(kernelFile) {
			return "", fmt.Errorf("kernel not found: %s", kernelFile)
		}
		return kernelFile, nil
	}
	if microvm != "" {
		per := filepath.Join(kernelsDir, microvm, kernelFile)
		if sbx.FileExists(per) {
			return per, nil
		}
		// Older deployments named the cloud-hypervisor kernel "vmlinux" too;
		// keep resolving it (the backend's format check still applies).
		if kernelFile != "vmlinux" {
			legacy := filepath.Join(kernelsDir, microvm, "vmlinux")
			if sbx.FileExists(legacy) {
				return legacy, nil
			}
		}
	}
	if e := os.Getenv("SBX_KERNEL"); e != "" {
		if !sbx.FileExists(e) {
			return "", fmt.Errorf("SBX_KERNEL points to a missing file: %s", e)
		}
		return e, nil
	}
	def := filepath.Join(kernelsDir, kernelFile)
	if sbx.FileExists(def) {
		return def, nil
	}
	return "", fmt.Errorf("kernel %q for microvm %q not found: place it at %s/kernels/%s/%s, set SBX_KERNEL, or use a legacy file at %s",
		kernelFile, microvm, sbx.HomeDir(), microvm, kernelFile, def)
}

// readHeader reads the first n bytes of a file.
func readHeader(path string, n int) []byte {
	f, err := os.Open(path)
	if err != nil {
		return nil
	}
	defer f.Close()
	buf := make([]byte, n)
	m, _ := f.Read(buf)
	return buf[:m]
}

// IsElf reports whether the file starts with the ELF magic.
func IsElf(path string) bool { return headerHas(readHeader(path, 4), 0x7f, 'E', 'L', 'F') }

// IsPe reports whether the file starts with the MZ (PE/EFI) magic.
func IsPe(path string) bool { return headerHas(readHeader(path, 2), 'M', 'Z') }

// IsElfOrPe reports whether the file is either an ELF or a PE (EFI) image, the
// two kernel formats Firecracker accepts.
func IsElfOrPe(path string) bool { return IsElf(path) || IsPe(path) }

// KernelFormatOK reports whether the kernel at path is acceptable for microvm
// on the current architecture. cloud-hypervisor on aarch64 requires a PE
// (EFI-stub) image (its loader is PE-only); every other family/arch accepts
// both ELF and PE images.
func KernelFormatOK(microvm, path string) bool {
	if microvm == "cloud-hypervisor" && runtime.GOARCH == "arm64" {
		return IsPe(path)
	}
	return IsElfOrPe(path)
}

func headerHas(b []byte, want ...byte) bool {
	if len(b) < len(want) {
		return false
	}
	for i := range want {
		if b[i] != want[i] {
			return false
		}
	}
	return true
}
