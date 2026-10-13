package sbx

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"syscall"
)

// NumaLock is a cross-process lock around the allocations file so concurrent
// `sbx create` / `sbx remove` invocations cannot corrupt or lose numa.json.
type NumaLock struct {
	f *os.File
}

func NewNumaLock() (*NumaLock, error) {
	if err := Mkdirs(HomeDir()); err != nil {
		return nil, err
	}
	path := filepath.Join(HomeDir(), "numa.lock")
	f, err := os.OpenFile(path, os.O_CREATE|os.O_RDWR, 0o644)
	if err != nil {
		return nil, err
	}
	if err := syscall.Flock(int(f.Fd()), syscall.LOCK_EX); err != nil {
		f.Close()
		return nil, err
	}
	return &NumaLock{f: f}, nil
}

func (l *NumaLock) Close() {
	if l == nil || l.f == nil {
		return
	}
	syscall.Flock(int(l.f.Fd()), syscall.LOCK_UN)
	l.f.Close()
	l.f = nil
}

type nodeState struct {
	node       int
	allCpus    []int
	usedCpus   []int
	memTotalMB int64
	usedMemMB  int64
}

func (n *nodeState) freeCpus() []int {
	var out []int
	for _, cpu := range n.allCpus {
		if !containsInt(n.usedCpus, cpu) {
			out = append(out, cpu)
		}
	}
	return out
}

func (n *nodeState) freeCpuCount() int { return len(n.freeCpus()) }
func (n *nodeState) freeMemMB() int64  { return n.memTotalMB - n.usedMemMB }

func containsInt(v []int, x int) bool {
	for _, e := range v {
		if e == x {
			return true
		}
	}
	return false
}

func numaStatePath() string { return filepath.Join(HomeDir(), "numa.json") }

// parseCpuList parses a Linux cpu-list ("0-3,8" or "0-3") into sorted ids.
func parseCpuList(text string) []int {
	var out []int
	for _, part := range strings.Split(strings.TrimSpace(text), ",") {
		if part == "" {
			continue
		}
		lo, hi := 0, 0
		if dash := strings.Index(part, "-"); dash >= 0 {
			lo, _ = strconv.Atoi(part[:dash])
			hi, _ = strconv.Atoi(part[dash+1:])
		} else {
			v, _ := strconv.Atoi(part)
			lo, hi = v, v
		}
		for cpu := lo; cpu <= hi; cpu++ {
			out = append(out, cpu)
		}
	}
	return out
}

func readSysfs(path string) string {
	b, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	return string(b)
}

func onlineNodes() []int {
	return parseCpuList(readSysfs("/sys/devices/system/node/online"))
}

func nodeMemMB(node int) int64 {
	data := readSysfs(fmt.Sprintf("/sys/devices/system/node/node%d/meminfo", node))
	for _, line := range strings.Split(data, "\n") {
		t := strings.TrimSpace(line)
		pos := strings.Index(t, "MemTotal:")
		if pos < 0 {
			continue
		}
		fields := strings.Fields(t[pos+9:])
		if len(fields) == 0 {
			return 0
		}
		kb, _ := strconv.ParseInt(fields[0], 10, 64)
		return kb / 1024
	}
	return 0
}

func nodeCpus(node int) []int {
	return parseCpuList(readSysfs(fmt.Sprintf("/sys/devices/system/node/node%d/cpulist", node)))
}

func joinInts(v []int) string {
	var parts []string
	for _, e := range v {
		parts = append(parts, strconv.Itoa(e))
	}
	return strings.Join(parts, ",")
}

// compressCpus renders a sorted list of cpu ids as a compact cpu-list
// ("0-3,8-11"). Consecutive ids are collapsed into ranges; an empty input
// yields "".
func compressCpus(v []int) string {
	if len(v) == 0 {
		return ""
	}
	var b strings.Builder
	start, prev := v[0], v[0]
	for i := 1; i <= len(v); i++ {
		if i < len(v) && v[i] == prev+1 {
			prev = v[i]
			continue
		}
		if b.Len() > 0 {
			b.WriteByte(',')
		}
		if start == prev {
			fmt.Fprintf(&b, "%d", start)
		} else {
			fmt.Fprintf(&b, "%d-%d", start, prev)
		}
		if i < len(v) {
			start, prev = v[i], v[i]
		}
	}
	return b.String()
}

func numaEnabled() bool {
	v := strings.ToLower(os.Getenv("SBX_NUMA"))
	if v != "" {
		switch v {
		case "0", "off", "false", "no":
			return false
		}
	}
	return true
}

type allocEntry struct {
	CPUs  string `json:"cpus"`
	Mems  string `json:"mems"`
	MemMB int    `json:"mem_mb"`
}

// loadAllocations reads id -> allocEntry from numa.json.
func loadAllocations() map[string]allocEntry {
	out := map[string]allocEntry{}
	data, err := os.ReadFile(numaStatePath())
	if err != nil {
		return out
	}
	var root struct {
		Allocations map[string]allocEntry `json:"allocations"`
	}
	if json.Unmarshal(data, &root) != nil {
		return out
	}
	for k, v := range root.Allocations {
		out[k] = v
	}
	return out
}

func saveAllocations(allocs map[string]allocEntry) {
	if err := Mkdirs(HomeDir()); err != nil {
		return
	}
	root := struct {
		Allocations map[string]allocEntry `json:"allocations"`
	}{Allocations: allocs}
	data, _ := json.MarshalIndent(root, "", "  ")
	WriteFile(numaStatePath(), string(data)+"\n")
}

// buildNodes rebuilds per-node state from topology + persisted allocations.
func buildNodes() []nodeState {
	allocs := loadAllocations()
	var nodes []nodeState
	for _, node := range onlineNodes() {
		st := nodeState{node: node, allCpus: nodeCpus(node), memTotalMB: nodeMemMB(node)}
		nodes = append(nodes, st)
	}
	for _, alloc := range allocs {
		memMB := int64(alloc.MemMB)
		mems := parseCpuList(alloc.Mems)
		var perNode int64
		if len(mems) == 0 {
			perNode = memMB
		} else {
			perNode = memMB / int64(len(mems))
		}
		for i := range nodes {
			if containsInt(mems, nodes[i].node) {
				nodes[i].usedMemMB += perNode
			}
		}
		for _, cpu := range parseCpuList(alloc.CPUs) {
			for i := range nodes {
				if containsInt(nodes[i].allCpus, cpu) {
					nodes[i].usedCpus = append(nodes[i].usedCpus, cpu)
					break
				}
			}
		}
	}
	return nodes
}

// usableNodes restricts to the nodes selected via SBX_NUMA_NODES (e.g. "0,2").
func usableNodes(all []nodeState) ([]nodeState, string) {
	e := os.Getenv("SBX_NUMA_NODES")
	if e == "" {
		return all, ""
	}
	allowed := parseCpuList(e)
	var out []nodeState
	for _, n := range all {
		if containsInt(allowed, n.node) {
			out = append(out, n)
		}
	}
	if len(out) == 0 {
		return all, "SBX_NUMA_NODES '" + e + "' matches no online node"
	}
	return out, ""
}

// numaAllocate picks CPU+memory placement for a sandbox and persists it.
//
// Sandboxes are OVERCOMMITTED by default, on CPU and memory alike: the
// requested sizes only set the cgroup limits (cpu.max / memory.max), they are
// NOT exclusively accounted — actual guest usage is typically far below the
// declared size, so the total declared memory may exceed the physical RAM the
// same way VMs overcommit a cloud host. The kernel enforces the per-sandbox
// limit with a cgroup-scoped OOM kill if a guest really exceeds its memory.max.
//
// Placement keeps a guest NUMA-local and balances instances across nodes:
//
//   - Node selection spreads instances over every usable node (least declared
//     memory first), so a full machine uses all nodes' DRAM instead of packing
//     every instance onto whichever node was allocated first. No create ever
//     fails for capacity: nodes are not hard-capped, actual usage stays far
//     below the declared sizes.
//   - Both cpuset.cpus AND cpuset.mems are pinned to the chosen node. Pinning
//     memory alone (as a locality hint) while leaving vCPUs free lets vCPU
//     threads run on another node's cores and take remote memory accesses; the
//     cpu pin keeps the guest's threads and pages on the same node.
//
// Explicit --cpuset-cpus / --cpuset-mems placement (handled in
// ResolvePlacement) is honored verbatim for callers that want dedicated,
// accounted resources.
func numaAllocate(cpus, memoryMB int, id string, out *NumaAllocation) error {
	lock, lerr := NewNumaLock()
	if lerr != nil {
		return lerr
	}
	defer lock.Close()

	*out = NumaAllocation{}
	if !numaEnabled() {
		return nil
	}
	onlineCount := len(onlineNodes())
	nodes, uerr := usableNodes(buildNodes())
	if uerr != "" {
		return fmt.Errorf("%s", uerr)
	}
	if onlineCount < 2 && os.Getenv("SBX_NUMA_NODES") == "" {
		return nil
	}

	// Balance: prefer the node with the least (declared) memory so instances
	// spread across nodes rather than stacking on the first one. Ties break to
	// the lower node id.
	sort.Slice(nodes, func(a, b int) bool {
		if nodes[a].usedMemMB != nodes[b].usedMemMB {
			return nodes[a].usedMemMB < nodes[b].usedMemMB
		}
		return nodes[a].node < nodes[b].node
	})
	chosen := nodes[0]

	// Pin CPU and memory to the same node (true NUMA locality). See the
	// function comment for why the cpu pin is required alongside cpuset.mems.
	out.CPUs = compressCpus(chosen.allCpus)
	out.Mems = strconv.Itoa(chosen.node)
	out.CrossNUMA = false

	allocs := loadAllocations()
	allocs[id] = allocEntry{CPUs: out.CPUs, Mems: out.Mems, MemMB: memoryMB}
	saveAllocations(allocs)
	return nil
}

// NumaRelease returns a sandbox's NUMA allocation to the pool.
func NumaRelease(id string) {
	lock, err := NewNumaLock()
	if err != nil {
		return
	}
	defer lock.Close()
	allocs := loadAllocations()
	if _, ok := allocs[id]; ok {
		delete(allocs, id)
		saveAllocations(allocs)
	}
}

// ResolvePlacement resolves CPU/memory placement for a sandbox. Explicit
// placement (--cpuset-cpus / --cpuset-mems via CLI or SBX_<BACKEND>_ env) takes
// precedence verbatim with no NUMA bookkeeping; otherwise falls back to
// numaAllocate().
func ResolvePlacement(opt Options, id string, out *NumaAllocation) error {
	cpus := BackendOpt(opt.Args, opt.Backend, "cpuset-cpus")
	if cpus == "" {
		cpus = BackendOpt(opt.Args, opt.Backend, "cpuset_cpus")
	}
	if cpus == "" {
		cpus = BackendOpt(opt.Args, opt.Backend, "cpuset.cpus")
	}
	mems := BackendOpt(opt.Args, opt.Backend, "cpuset-mems")
	if mems == "" {
		mems = BackendOpt(opt.Args, opt.Backend, "cpuset_mems")
	}
	if mems == "" {
		mems = BackendOpt(opt.Args, opt.Backend, "cpuset.mems")
	}
	if cpus != "" || mems != "" {
		*out = NumaAllocation{CPUs: cpus, Mems: mems}
		return nil
	}
	return numaAllocate(opt.CPUs, opt.MemoryMB, id, out)
}
