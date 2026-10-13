package sbx

// Options carries the CLI-provided parameters for `sbx create`.
type Options struct {
	Backend  string
	Image    string
	Name     string
	Kernel   string // kernel name (e.g. "vmlinux") or path; empty = default
	CPUs     int
	MemoryMB int
	Port     int // 0 = auto-allocate
	// Args holds backend-specific create parameters passed through verbatim
	// from the CLI (any unknown --key=value / --key value on `create`).
	// Values are raw strings.
	Args map[string]string
}

// Info is the persisted per-sandbox state record. The JSON schema matches the
// legacy C++ implementation so existing ~/.sbx/vms/*.json files keep working.
type Info struct {
	ID            string `json:"id"`
	Name          string `json:"name"`
	Backend       string `json:"backend"`
	Image         string `json:"image"`
	ContainerName string `json:"containerName"`
	AgentHost     string `json:"agentHost"`
	AgentPort     int    `json:"agentPort"`
	AgentToken    string `json:"agentToken"`
	GuestIP       string `json:"guestIp"`
	TapDevice     string `json:"tapDevice"`
	SlotLock      string `json:"slotLock"`
	PublishPort   int    `json:"publishPort"`
	Status        string `json:"status"`
	PID           int    `json:"pid"`
	PIDFile       string `json:"pidFile"`
	// Meta holds backend-specific runtime metadata (resource ids, endpoints).
	// Persisted alongside the fixed fields and restored on load so stop/remove
	// can recover the backend's own resources.
	Meta map[string]string `json:"meta"`
}

// NumaAllocation is the result of NUMA-aware CPU+memory placement.
type NumaAllocation struct {
	CPUs      string // comma list of host cpu ids, empty = no cpu binding
	Mems      string // comma list of numa node ids, empty = no mem binding
	CrossNUMA bool
}
