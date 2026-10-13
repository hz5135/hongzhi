// Package sbxhost provides host-side plumbing shared by the microVM
// backends (firecracker, cloud-hypervisor): network slot allocation, TAP
// devices via netlink, iptables port forwarding, cached rootfs/injection and
// kernel resolution. It is backend-agnostic: policies (which image, which
// kernel, sizes) come from the calling backend.
package sbxhost

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"

	"agentbench/sandbox/internal/sbx"
)

// JailerBase returns the per-VM runtime directory root under ~/.sbx/jailer.
func JailerBase() string { return filepath.Join(sbx.HomeDir(), "jailer") }

// ResolveBin resolves a required binary: env var first, then
// ~/.sbx/bin/<name>. Missing files are an error, never silent.
func ResolveBin(envName, fileName string) (string, error) {
	if e := os.Getenv(envName); e != "" {
		if !sbx.FileExists(e) {
			return "", fmt.Errorf("%s points to a missing file: %s", envName, e)
		}
		return e, nil
	}
	def := filepath.Join(sbx.HomeDir(), "bin", fileName)
	if sbx.FileExists(def) {
		return def, nil
	}
	return "", fmt.Errorf("no %s found: set %s or place the binary at %s", fileName, envName, def)
}

// CopyFile copies src to dst, sharing extents with it when the filesystem can
// (reflink: XFS with reflink=1, btrfs - both files must live on that filesystem).
// Every sandbox gets its own ~1GB rootfs image copied out of the template, of
// which ~120MB is real data, so a reflinked copy turns the largest per-sandbox
// host-side cost into a metadata operation; the guest's writes then copy only
// the extents it touches. Filesystems without reflink (ext4 and friends) fail
// the attempt immediately and take the plain `cp -f` path, sparse holes and all.
func CopyFile(src, dst string) error {
	if _, err := exec.Command("cp", "-f", "--reflink=always", src, dst).CombinedOutput(); err == nil {
		return nil
	}
	// A refused reflink can still have created/truncated dst; start clean.
	os.Remove(dst)
	cmd := exec.Command("cp", "-f", src, dst)
	if out, err := cmd.CombinedOutput(); err != nil {
		return fmt.Errorf("cp %s -> %s: %s%s", src, dst, string(out), err)
	}
	return nil
}
