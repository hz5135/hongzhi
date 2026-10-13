package sbx

import (
	"crypto/rand"
	"encoding/hex"
	"net"
	"os"
	"path/filepath"
	"strings"
)

// HomeDir returns the root of all host-side state (~/.sbx, override SBX_HOME).
func HomeDir() string {
	if v := os.Getenv("SBX_HOME"); v != "" {
		return v
	}
	home, err := os.UserHomeDir()
	if err != nil || home == "" {
		return ".sbx"
	}
	return filepath.Join(home, ".sbx")
}

func envOr(name, def string) string {
	if v := os.Getenv(name); v != "" {
		return v
	}
	return def
}

// TmpDir returns the temp dir for uploads/rootfs mounting (SBX_TMP_DIR, /tmp).
func TmpDir() string { return envOr("SBX_TMP_DIR", "/tmp") }

// RandomHex returns n hex characters of crypto randomness.
func RandomHex(n int) string {
	b := make([]byte, (n+1)/2)
	if _, err := rand.Read(b); err != nil {
		panic(err)
	}
	return hex.EncodeToString(b)[:n]
}

// FileExists reports whether path is an existing regular file.
func FileExists(path string) bool {
	st, err := os.Stat(path)
	return err == nil && !st.IsDir()
}

// DirExists reports whether path is an existing directory.
func DirExists(path string) bool {
	st, err := os.Stat(path)
	return err == nil && st.IsDir()
}

// Mkdirs creates path and any parents (like mkdir -p).
func Mkdirs(path string) error {
	return os.MkdirAll(path, 0o755)
}

// ReadFile returns the contents of a file (or an empty string on error).
func ReadFile(path string) string {
	b, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	return string(b)
}

// WriteFile writes data to path with 0644 perms.
func WriteFile(path string, data string) error {
	return os.WriteFile(path, []byte(data), 0o644)
}

// StatePath returns the per-sandbox state file path.
func StatePath(id string) string {
	return filepath.Join(HomeDir(), "vms", id+".json")
}

// FindFreePort asks the kernel for a free TCP port (race-free: bind then close).
func FindFreePort() int {
	ln, err := net.Listen("tcp4", "0.0.0.0:0")
	if err != nil {
		return 0
	}
	defer ln.Close()
	addr := ln.Addr().(*net.TCPAddr)
	return addr.Port
}

func trim(s string) string { return strings.TrimSpace(s) }

func split(s string, sep string) []string { return strings.Split(s, sep) }
