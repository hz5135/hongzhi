package sbx

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// Save atomically writes the state file (write .tmp + rename) so concurrent
// list/remove can never observe a half-written JSON.
func (i Info) Save() error {
	if err := Mkdirs(filepath.Join(HomeDir(), "vms")); err != nil {
		return err
	}
	data, err := json.MarshalIndent(i, "", "  ")
	if err != nil {
		return err
	}
	data = append(data, '\n')
	path := StatePath(i.ID)
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, data, 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

// Load reads a sandbox by id, falling back to a name match across all states.
// Returns an empty Info when nothing matches.
func Load(id string) Info {
	path := StatePath(id)
	data, err := os.ReadFile(path)
	if err == nil {
		return parseInfo(data, id)
	}
	for _, cand := range List() {
		if cand.Name == id {
			return cand
		}
	}
	return Info{}
}

func parseInfo(data []byte, id string) Info {
	var i Info
	if err := json.Unmarshal(data, &i); err != nil {
		return Info{}
	}
	if i.ID == "" {
		i.ID = id
	}
	if i.Meta == nil {
		i.Meta = map[string]string{}
	}
	return i
}

// List returns all sandboxes sorted by id.
func List() []Info {
	dir := filepath.Join(HomeDir(), "vms")
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil
	}
	var out []Info
	for _, e := range entries {
		name := e.Name()
		if !strings.HasSuffix(name, ".json") || name == "." || name == ".." {
			continue
		}
		id := strings.TrimSuffix(name, ".json")
		if i := Load(id); i.ID != "" {
			out = append(out, i)
		}
	}
	sort.Slice(out, func(a, b int) bool { return out[a].ID < out[b].ID })
	return out
}

// BackendOpt resolves a backend-specific option. Precedence:
//  1. Options.Args[key]
//  2. SBX_<BACKEND>_<KEY> env var (backend/key uppercased)
//  3. def fallback ("" when omitted)
func BackendOpt(args map[string]string, backend, key string, def ...string) string {
	if v, ok := args[key]; ok && v != "" {
		return v
	}
	envName := "SBX_" + strings.ToUpper(backend) + "_" + strings.ToUpper(key)
	if v := os.Getenv(envName); v != "" {
		return v
	}
	if len(def) > 0 {
		return def[0]
	}
	return ""
}

// gBinDir is the directory containing the sbx binaries (dirname of argv[0]),
// set once at CLI startup via SetBinDir. Used to locate the default
// sbx-agent path.
var gBinDir string

// SetBinDir records the directory of the running sbx binary.
func SetBinDir(dir string) { gBinDir = dir }

// SandboxAgentBin returns the path to the sbx-agent binary (SBX_AGENT_BIN or
// <bin-dir>/sbx-agent).
// CheckAgentBinary verifies that the host-side agent the microVM backends
// inject into their rootfs template is still there. `make clean` removes the
// deployed copies next to the sbx binary; without this check the loss only
// surfaces when a create fails - after the pending rootfs template rebuild has
// already been paid for. Called from the backends' CheckEnv, so `sbx doctor`
// reports it up front.
func CheckAgentBinary() error {
	bin := SandboxAgentBin()
	if FileExists(bin) {
		return nil
	}
	return fmt.Errorf("sandbox agent binary not found at %s - build it with `make agent` "+
		"(or point SBX_AGENT_BIN at it)", bin)
}

func SandboxAgentBin() string {
	if e := os.Getenv("SBX_AGENT_BIN"); e != "" {
		return e
	}
	dir := gBinDir
	if dir == "" {
		dir = "."
	}
	return filepath.Join(dir, "sbx-agent")
}
