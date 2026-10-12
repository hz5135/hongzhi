# AgentBench

**AgentBench** is a benchmark tool to measure the performance of AI agent system.

---

## Features

- Simulate common micro-operations of AI agent system and measure the power of hardware.
- Simulate common micro-operations of multiple AI agents to evaluate the max parallelism.
- Measure the performance of a self-defined AI agent system with rich and typical scenarios.

---

## Quick Start

### Prerequisites

- C and C++ compiler (gcc/clang)
- make
- a static `libstdc++` (`libstdc++.a`) — the binary links with `-static-libstdc++` so it runs on hosts whose system libstdc++ is older than the compiler

No other dependencies — LuaJIT 2.1 and libcurl 8.11.0 are vendored under `third_party/` and built from source.

### Build

```bash
make agentbench   # the benchmark binary
make test         # smoke test (builds agentbench first)
```

`make agentbench` links LuaJIT with `-Wl,--export-dynamic`, which is what makes
the C kernels resolvable from Lua via `ffi.C`.

The binary is compiled for a **conservative portable ISA** by default
(`armv8-a` on aarch64, `x86-64` on x86_64), so it also runs where the CPU does
not expose every extension of the build host. Opt into the build host's own ISA
with `make MARCH=native`.

---

## Usage

### List Benchmarks

```bash
./agentbench list simulation     # the benchmarks this build contains
```

### Run a Simulation Benchmark (CPU)

Run `json_processing` with 4 threads:

```bash
./agentbench --mode=simulation -t 4 -b json_processing
```

### Run the Full Simulation Suite

Run every benchmark in the build:

```bash
./agentbench --mode=simulation -t 4
```

Run a subset:

```bash
./agentbench --mode=simulation -t 4 -b json_processing,tree_search -i 5
```

Output as JSON:

```bash
./agentbench --mode=simulation -b json_processing -j
```

### Passing Options to a Benchmark

```bash
./agentbench --mode=simulation -t 4 -b tree_search
```

(`tree_search` has no tunable knobs: its workload constants — `total_sets` and
the per-set sizes — are defined at the top of `lua/benchmarks/tree_search.lua`.)

(`json_processing`, `text_processing`, `diff_patch`, and `html_parsing` are the
same: their workload constants are defined at the top of the matching
`lua/benchmarks/*.lua`.)

### Command

``` Bash
[root@localhost ~]# agentbench --help
Usage:
agentbench --mode=simulation [options]

Benchmark suit for AI agent workloads

General options：
  -m, --mode=MODE  simulation for CPU benchmark of agent workloads
  --help  show this help message and exit

Simulation mode options:
  -b, --benchmarks=BENCHMARK_LIST  run only the named benchmark, separated by comma if mutiple (default: all simulation benchmarks)
  -i, --iterations=ITERATIONS  number of timed iterations per benchmark (default: 5)
  -t, --threads=THREADS  max threads for concurrent benchmarks (default: CPU count)
  -j, --json  output results as JSON
  -v, --verbose  show detailed per-iteration results
```

---

## Options

| Option        | Type   | Abbreviation | Default Value | Required | Enum                                                                                                                                                                                                                                                                 | Description                                                                                                                                                                                                 | Remarks                                                               |
| ------------- | ------ | ------------ | ------------- | -------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------- |
| mode          | string | -m           | -             | Y        | • simulation: works in simulation mode, tests locally only.                                                  | Benchmark work mode. The specified or all simulated operations will be executed.                                                             |                                                                       |
| benchmarks    | string | -b           | all           | N        | Simulation Mode:<br>• context_switching<br>• json_processing<br>• text_processing<br>• tree_search<br>• concurrent_dispatch<br>• memory_pressure<br>• subprocess_spawning<br>• diff_patch<br>• html_parsing<br>• schema_validation<br>• streaming_parse<br>• code_edit_apply<br>• agentic_loop | Benchmark suite (or micro-operations) to be run.                                                                                                                                                           | All benchmark suits will be run under the specified mode by default.   |
| iterations    | int    | -i           | 5             | N        | -                                                                                                                                                                                                                                                                    | Rounds to be run.                                                                                                                                                                                           | Only works under simulation mode.                                     |
| threads       | int    | -t           | CPU_COUNT     | N        | -                                                                                                                                                                                                                                                                    | Concurrent thread count.                                                                                                                                                                                   | • Only works under simulation mode.<br>• CPU core count by default.    |
| json          | -      | -j           | -             | N        | -                                                                                                                                                                                                                                                                    | Whether output is JSON format for the stdout.                                                                                                                                                              | • Only works under simulation mode.<br>• Output in human-readable format by default. |
| verbose       | -      | -v           | -             | N        | -                                                                                                                                                                                                                                                                    | Whether output all the mid-process log.                                                                                                                                                                    | • Only works under simulation mode.<br>• Only output the statistical result by default. |
| help          | string | -            | -             | N        | -                                                                                                                                                                                                                                                                    | Output the usage and description of all options.                                                                                                                                                           |                                                                       |

---

## Benchmarks

### Simulation Mode — 13 Atomic Scenarios

The design defines thirteen atomic scenarios (see
[`docs/designs/simulation_mode.md`](docs/designs/simulation_mode.md)). Each is
a **C kernel** (`src/benchmarks/*.c`) compiled into the binary and wrapped by a
Lua script (`lua/benchmarks/*.lua`) whose `event()` callback calls the kernel via
`ffi.C`. This series implements five of them; the Scope column says which.

| Benchmark | Description | Scope |
|-----------|-------------|-------|
| `context_switching` | Rapid coroutine/thread switching (agent think→tool→think loop) | later |
| `json_processing` | JSON serialization + deserialization of tool schemas, conversations, function-call round trips | this series |
| `text_processing` | Chunking + content hashing, regex-like extraction, line-by-line log parsing | this series |
| `tree_search` | MCTS + beam search (agent planning/reasoning) | this series |
| `diff_patch` | Line-by-line diff, unified diff generation, patch application | this series |
| `html_parsing` | DOM tree construction, CSS-like queries, HTML-to-markdown | this series |
| `schema_validation` | JSON Schema validation + malformed JSON repair | later |
| `streaming_parse` | SSE stream parsing with fragmented tool-call JSON accumulation | later |
| `code_edit_apply` | String search, uniqueness verification, replacement | later |
| `agentic_loop` | Full Glob → Grep → Read → Edit → Verify cycle with growing conversation state | later |
| `memory_pressure` | Context-window management: sliding-window attention, KV-cache insert/lookup, history compaction, burst alloc/dealloc | later |
| `concurrent_dispatch` | Parallel tool dispatch + shared-state aggregation (web search / file read / API call) | later |
| `subprocess_spawning` | Process fork/exec, pipe management, and output collection | later |

#### Single Agent vs Multiple Agents

The scenario set is selected from the thread count (an explicit `-b` list always
runs exactly what was requested):

- **Single agent** (`-t 1`) — the scenarios that model one agent, in a single
  thread.
- **Multiple agents** (`-t N`, N ≥ 2) — the scenarios that model several agents
  running concurrently, spread across N threads. The thread count defaults to
  the total number of CPU cores.

When `-t 1` the selection skips `context_switching`, `concurrent_dispatch` and
`subprocess_spawning` — the three scenarios that only mean something with several
agents. None of the five scenarios implemented here is one of them, so today
`-t 1` and `-t N` run the same set; the mechanism is in place for the planned
ones.

---

## Output

### Simulation Mode

```Bash
[root@localhost ~]# agentbench --mode=simulation

===========================================================================
  AgentBench v0.2
  Benchmark suit for AI agent workloads
===========================================================================

  CPU:         HJMicro(R) Hongmin(R) 9198 Processor
  Socket:      2
  NUMA:        2
  Cores:       256
  Platform:    Linux-5.10.211.v10+-aarch64-with-glibc2.28
  Arch:        aarch64
  Time:        2026-07-27 10:45:21

===========================================================================


  Benchmark               elapsed (sec)                ops
  ------------------------------------------------------------
  agentic_loop                   0.2490             317047
  json_processing                0.0510               4300
  subprocess_spawning            5.6700              25590
  text_processing                0.1540             130743
  tree_search                    3.8300            4653944
  context_switching              0.1530              30000
  concurrent_dispatch            0.1090               4000
  diff_patch                     0.3120              17727
  streaming_parse                0.2870              71578
  code_edit_apply                0.1450               4774
  html_parsing                   0.4380              52368
  schema_validation              0.5660              12710
  memory_pressure                2.5860             265182
  ------------------------------------------------------------
  Total elapsed                 14.5500
  Thread count:          256
```

Per benchmark the suite prints the wall-clock **elapsed** time and the number of
**ops** executed (JSON output adds an `"ops"` field per result). With `-i N` the
suite repeats N times and **elapsed is the accumulated total across all N
iterations** (ops stay the per-event count), so e.g. `-i 1000` shows the total
runtime of the 1000 runs.

---

## Weights

The weight of each scenario is its observed frequency under business scenarios
(as documented in `docs/designs/simulation_mode.md`).

| Benchmark | Weight |
|-----------|--------|
| Agentic Loop | 13% |
| JSON Processing | 10% |
| Subprocess Spawning | 9% |
| Text Processing | 8% |
| Tree Search | 8% |
| Context Switching | 7% |
| Concurrent Dispatch | 7% |
| Diff/Patch | 7% |
| Streaming Parse | 7% |
| Code Edit Apply | 7% |
| HTML Parsing | 6% |
| Schema Validation | 6% |
| Memory Pressure | 5% |

---

## CLI Reference

```
Usage:
  agentbench --mode=simulation [options]
  agentbench list simulation
  agentbench --version

General options:
      --version          show version and exit

Simulation mode options:
  -b, --benchmarks=LIST  run only the named benchmarks (default: all)
  -i, --iterations=N     suite iterations (default: 5); the reported elapsed is the
                         accumulated total across the N iterations
  -t, --threads=N        max threads for concurrent benchmarks (default: CPU count)
  -j, --json             output results as JSON
  -v, --verbose          show detailed per-iteration results

Benchmark tuning: any unrecognized `--key=value` argument
is passed through to the Lua scripts as `agentbench.opt.<key>`, overriding the
script defaults declared in `agentbench.cmdline.options`. The benchmarks
implemented here have no tunable knobs: their workload constants — `total_sets`
and the per-set sizes — are defined at the top of `lua/benchmarks/*.lua`. Edit
them there, no recompile needed; keep `total_sets` >= your core count so `-t` is
not capped. Keys that collide with a suite-level option (`iterations` = `-i`,
`threads` = `-t`) are consumed by getopt first, so rename the Lua-side option if
you need to override those. Numeric values are auto-converted; everything else
passes as a string.
```

---

## Architecture

```
┌────────────────────────────────────────────────────────────────┐
│                    agentbench (C binary)                        │
│                   embeds LuaJIT (libluajit)                     │
│                                                                │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │  Runner (src/run.cpp)                                    │  │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐       │  │
│  │  │ Worker 0    │  │ Worker 1    │  │ Worker N    │       │  │
│  │  │ LuaJIT state│  │ LuaJIT state│  │ LuaJIT state│       │  │
│  │  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘       │  │
│  │         │   thread_run   │                │               │  │
│  │         └──────┬─────────┴────────────────┘               │  │
│  │                │  ffi.C                                   │  │
│  │  ┌─────────────▼──────────────────────────────────────┐   │  │
│  │  │  C Kernels (src/benchmarks/*.c)                    │   │  │
│  │  │  run_tree_search, run_json_processing, ...                │  │  │
│  │  └────────────────────────────────────────────────────┘   │  │
│  └──────────────────────────────────────────────────────────┘  │
│                                                                │
│  Lua scripts (lua/benchmarks/*.lua) define event() callbacks   │
│  that call the C kernels.                                      │
└────────────────────────────────────────────────────────────────┘
```

---

## Contributing

Contributions are welcome! Please open an issue to discuss your ideas before submitting a PR.

---

## References

- [Sysbench](https://github.com/akopytov/sysbench) — Database benchmark tool (LuaJIT + C plugin architecture)
- [LuaJIT](https://luajit.org/) — Just-in-time compiler for Lua 5.1
- [AgentiCPUMark](https://github.com/apellegr/AgentiCPUMark): AgentiCPUMark is a CPU-oriented benchmark tool to measure the performance of AI agent operations. Although it is not designed for real AI agent systems, we consider it as an excellent case to evalute the performance of local environment roughly.
- [TPC-C](https://www.tpc.org/tpcc/): TPC-C is the most widely used online transaction processing benchmark. During designing the benchmark metrics, we referenced its principle a lot.  
- [TPC-H](https://www.tpc.org/tpch/): TPC-H is another famous benchmark which is often used to measure the performance of complex query systems such as big data workloads. It consists of 22 different kinds of queries to cover most typical scenarios.

---

## License

[GPL-v3.0](https://opensource.org/license/GPL-3.0)