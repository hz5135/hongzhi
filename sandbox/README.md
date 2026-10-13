# sbx — agentbench sandbox layer

`sbx` is the sandbox layer for agentbench: it creates disposable, isolated
environments (**Firecracker microVMs** or **Docker containers**) and runs the
benchmark suite inside them through an HTTP agent, so `exec` / `upload` /
`download` act on a live sandbox instead of the host.

The host-side CLI is **Go** and drives the backends through their official SDKs
— the Docker Engine SDK, `firecracker-go-sdk`, **netlink** for TAP devices,
**go-iptables** for port forwarding — rather than shelling out to
`docker`/`ip`/`iptables`. The in-guest **`sbx-agent`** is C++ (see
[Layout](#layout)). Apart from Go module downloads, `make` builds everything.

## Build

```sh
make             # agentbench + sbx + sbx-agent into the repo root
make -C sandbox  # sandbox/bin only
```

Go modules are fetched via `GOPROXY` (default `https://proxy.golang.org,direct`,
overridable: `make -C sandbox sbx GOPROXY=...`).

## How a run works

1. `sbx create` provisions the environment and boots the **agent** (an HTTP
   server) inside it. For Firecracker that means: allocate a TAP slot, copy the
   cached guest rootfs, spawn the jailed microVM, configure it over its API
   socket, wait for `/health`. The rootfs template is built by `make rootfs`,
   never by a create — see [Rootfs provisioning](#rootfs-provisioning).
2. `sbx exec <id> -- <cmd>` sends an HTTP request; the agent forks the command
   and streams stdout/stderr + exit code back as NDJSON.
3. `sbx remove <id>` tears everything down — VM, TAP, cgroup, NUMA record, slot
   lock, state file.

### What a create times

`sbx create` prints one machine-readable `timing:` line. `startup` and `ready`
are the two totals; each is followed by the parts it sums, in the order they
happen:

| field | covers |
| --- | --- |
| `provision` | building/caching the guest rootfs template — one-time, shared by every sandbox |
| `host_setup` | everything the host does before a guest exists: slot, TAP, the rootfs copy into the jailer chroot, the VMM spawn and its API |
| `boot` | instance start → guest init, i.e. the guest's own kernel boot |
| `userspace` | guest init → the agent answering `/health` (systemd + the agent) |

    startup = provision + host_setup + boot     # up to the guest's init
    ready   = host_setup + boot + userspace     # up to a usable sandbox

`startup` is the start latency (it includes work done before the VM existed) and
`ready` the usable latency — compare like with like. `boot` is the only field
comparable with a published boot time. The totals overlap (`host_setup` and
`boot` are in both), so never add a part to a total that already contains it. A
segment a backend cannot measure is `-1` (n/a); a total needing it is n/a too,
not silently short a part.

## CLI

```sh
sbx doctor [--backend <backend>]   # verify backend environment (default: all)
sbx rootfs build|status [--image IMG] [--force]
                                   # the guest rootfs template the microVMs boot
                                   # (built by `make rootfs`, never by a create)
sbx create  --backend <backend> --image IMG \
            [--name N --cpus N --memory MB --port P] \
            [--<backend-arg>=V ...]
sbx health  <name-or-id> | --host H --port P --token T
sbx exec    <name-or-id> -- <cmd> [args...]
sbx upload  <name-or-id> -- <file>... [-d dest]
sbx download <name-or-id> <remote> [-o local]
sbx stop    <name-or-id>
sbx remove  <name-or-id>
sbx list
```

`<backend>` is any registered backend — run `sbx help` to see the current set,
which is derived from the Go backend registry at runtime, not hard-coded.
Backends self-register in their package `init()` and are linked in via `cmd/sbx`
(see [Adding a backend](#adding-a-backend-drop-in)).

On `create`, any unrecognized `--key=value` / `--key value` is passed through to
the backend verbatim (stored in `Options.args`); backends read their own keys
with `backendOpt()` and ignore the rest.

Two keys are recognized by every backend for **explicit placement**:

- `--cpuset-cpus=LIST` — pin the sandbox to an exact set of host CPUs
  (e.g. `--cpuset-cpus=0-127`); requests **dedicated cores**, recorded in
  `numa.json`. Without it, vCPUs are overcommitted, pinned to the NUMA node
  chosen for memory, and bounded only by the `cpu.max` quota from `--cpus N`.
- `--cpuset-mems=LIST` — pin memory to specific NUMA nodes (e.g.
  `--cpuset-mems=0`); requests **dedicated nodes**. Without it, memory is
  overcommitted (only `memory.max` applies) and instances are balanced across
  nodes.

They translate to Docker `--cpuset-cpus/--cpuset-mems` or the jailer cgroup
`cpuset.cpus/cpuset.mems`, and compose with `--cpus N` (still the size/quantity):
`--cpuset-cpus=0-127 --cpus 2` means "any of cores 0-127, 2 vCPUs" — not a
conflict. The same values can come from `SBX_<BACKEND>_CPUSET_CPUS` /
`SBX_<BACKEND>_CPUSET_MEMS`.

A sandbox is referenced by id/name (state under `~/.sbx/vms/`) or by explicit
`--host/--port/--token`. The default spec is **2 vCPU / 4096 MiB**; `--port N` on
the firecracker backend additionally publishes the agent port via iptables DNAT.

> `sbx exec` runs the command directly (`execvp`, no shell). Wrap compound
> commands with `bash -c "..."`.

## Configuration & the `~/.sbx` state directory

All host-side state lives under `~/.sbx` (override with `SBX_HOME`); nothing is
configured globally. You provision the binaries/kernel once and `sbx` finds
everything from here. `sbx doctor` validates it.

```
~/.sbx/
  bin/                 firecracker, jailer, sbx-agent
                       (or SBX_FIRECRACKER / SBX_JAILER / SBX_AGENT_BIN)
  kernels/<microvm>/   bzImage (x86_64) / Image (aarch64) — the per-family defaults
  jailer/firecracker/  per-VM chroots + firecracker sockets (created at runtime)
  registry/rootfs/<image>/<tag>/   guest rootfs template: rootfs.ext4 + injected.ext4
  slots/               O_EXCL lock files, one per network slot (0..16383)
  vms/                 per-sandbox state: <id>.json (created at runtime)
  numa.json            CPU/mem NUMA allocation ledger
  numa.lock            cross-process lock guarding numa.json
```

### One-time setup for the firecracker backend

```sh
# 1) firecracker + jailer binaries
mkdir -p ~/.sbx/bin
cp /path/to/firecracker /path/to/jailer ~/.sbx/bin/

# 2) kernel — one per microVM family, with a per-family, per-arch default name
mkdir -p ~/.sbx/kernels/firecracker
cp /path/to/bzImage ~/.sbx/kernels/firecracker/bzImage   # x86_64 (or ln -s)
cp /path/to/Image   ~/.sbx/kernels/firecracker/Image    # aarch64 (or ln -s)
#    The official startup metric needs CONFIG_DEVMEM=y in that kernel: a create
#    boots the guest with init=/usr/local/bin/sbx-boot-init, which signals
#    firecracker's --boot-timer by writing one byte to its boot device (MMIO at
#    1 GiB on aarch64, 3 GiB on x86_64) through /dev/mem. Without CONFIG_DEVMEM
#    the sandbox still boots and stays correct, but reports the guest's own
#    kernel timestamp (`startup source: guest_kmsg_fallback`).

# 3) sandbox agent binary (built by `make`)
cp ./sbx-agent ~/.sbx/bin/

# 4) verify. The microVM backends bound each VM through a cgroup v2 tree, so the
#    host must run the unified cgroup v2 hierarchy with the cpu/memory
#    controllers enabled at the root (cgroup v1-only hosts are rejected).
sbx doctor --backend firecracker
```

#### Common deployment failures

A failed create prints `error: failed to create sandbox ... (rc=N)` and/or
`sandbox ... : no timing data`. The host command's output is silenced, so run
`sbx create --backend firecracker ...` or `sbx doctor --backend firecracker`
directly to see the real error. `rc=3` means the rootfs template is not built for
the current image — see [Rootfs provisioning](#rootfs-provisioning).

| Symptom | Cause / fix |
|---|---|
| `~/.sbx` empty, or `no firecracker found: set SBX_FIRECRACKER or place the binary at ...` | binaries/kernel not provisioned — do steps 1–3 |
| `failed to dial "/run/containerd/containerd.sock"`, docker create hangs | daemon down: `systemctl restart containerd && systemctl start docker` |
| `jailer failed: Failed to write to /sys/fs/cgroup/cgroup.subtree_control: Invalid argument` | cgroup v2 root lacks `cpu`: `echo '+cpu +cpuset +io +memory +hugetlb +pids +rdma +misc' > /sys/fs/cgroup/cgroup.subtree_control` |
| `Failed to exec into Firecracker: No such file or directory`, or `firecracker API socket not ready` | the firecracker binary is a dynamically linked (glibc) build and the jailer chroot has no glibc loader. Use the static (musl) build — `file ~/.sbx/bin/firecracker` must say "statically linked" (~3.2 MB vs ~4.7 MB). Never use a distro-installed firecracker. |
| `no timing data` although create succeeded, and the inner run dies with `Illegal instruction` (exit 132) | the guest runs the **host-built** binary, so a `MARCH=native` build using instructions the Firecracker vCPU lacks (SVE/SVE2) crashes. Rebuild portable and re-ship: `make agentbench && make image && make rootfs`; verify with `objdump -d ./agentbench \| grep -ciE 'sve\|whilelo\|psel\|ld1rq'` (must print 0). Never set `MARCH=native` for anything that runs in a guest. |
| stale `~/.sbx/vms/*.json` or cgroups from another host | `sbx remove <id> 2>/dev/null; rm -f ~/.sbx/vms/*.json` |

#### Recommended smoke test

```sh
make sbx agent
./scripts/build-image.sh          # portable ISA by default, safe for Firecracker
sbx doctor --backend firecracker
./agentbench --mode=simulation -i 1 -s -B firecracker   # expect total_elapsed + per-benchmark stats
```

### Choosing a kernel

MicroVM backends load the guest kernel themselves (no bootloader), and each
backend/architecture combination accepts a specific set of formats. `sbx doctor`
validates the resolved kernel and rejects unusable ones.

| Backend / arch | Loader | Default file | Accepted formats |
|---|---|---|---|
| firecracker x86_64 | FC's own loader | `bzImage` | ELF (`0x7f ELF`) or PE/EFI (`MZ`) |
| firecracker aarch64 | FC's own loader | `Image` | ELF (`0x7f ELF`) or PE/EFI (`MZ`) |
| cloud-hypervisor x86_64 | ELF loader | `bzImage` | ELF (incl. `bzImage`); PE/EFI |
| cloud-hypervisor aarch64 | PE loader only | `Image` | PE/EFI-stub only (`MZ`) |

- **The default file name is per-family and per-architecture**
  (`sbxhost.DefaultKernelName`): `bzImage` on x86_64 and `Image` on aarch64, for
  both microVM families. Deploy the right name under
  `~/.sbx/kernels/<microvm>/`. A legacy `vmlinux` there is still honoured as a
  fallback, subject to the same format check.
- **Raw `vmlinuz` (compressed zImage/bzImage) with no `MZ`/ELF header does not
  boot without a bootloader** and is rejected. The `bzImage` mainstream distros
  ship is EFI-stub (`MZ`-headed) and passes.
- Use a kernel with `virtio-net`, `virtio-blk`, `CONFIG_NET_NS` and an init
  system (systemd) — rootfs provisioning installs `systemd-sysv` for exactly
  this.
- Resolution order: `~/.sbx/kernels/<microvm>/<default>` first, then
  `SBX_KERNEL`. An `SBX_KERNEL` pointing at a missing file is an error, never
  silent.
- Running several microVM families side by side: keep one kernel per family under
  `~/.sbx/kernels/` using the per-microVM layout — `sbx` never guesses which
  kernel a family should boot. A legacy flat `~/.sbx/kernels/vmlinux` is a
  last-resort fallback.

### Rootfs provisioning

The rootfs the microVM backends boot is built **with the image** and cached under
`~/.sbx/registry/rootfs/<image>/<tag>/`:

```sh
make image            # docker build -t agentbench:latest
make rootfs           # the guest rootfs template (sbx rootfs build)
sbx rootfs status     # what is cached, and whether it is current

# docker build (adds systemd + the guest tools) -> docker export -> mkfs.ext4;
# rootfs.ext4 is invalidated when the source image id changes
```

Only the build makes it. A create that finds the template missing, stale or built
from another image stops with `guest rootfs not prepared: ...` (exit code 3,
`sbx.ExitRootfsNotReady`) rather than building one — that is tens of seconds of
one-time work which must not land in a benchmark run, and under `-n N` every
parallel create would start its own. Once the template exists, a microVM run
needs no Docker daemon at all.

`agentbench` images carry the agent in `ENTRYPOINT` (`docker/Dockerfile`), so
**docker** needs no provisioning. **firecracker** injects the agent as
`sbx-agent.service` (`/usr/local/bin/sbx-agent`) into the cached template — a
fully static injection: the agent binary, systemd unit, `resolv.conf` and
hostname are baked into `injected.ext4` once, and every VM copies that template
into its own chroot (no per-VM mount). The per-VM token goes on the kernel
command line (`agentbench.token=...`, read via `sbx-agent --token-from-cmdline`)
rather than into each rootfs. The template is invalidated when the base image id
or the `sbx-agent` hash changes.

## Backends

This build ships **firecracker** and **docker**. **cloud-hypervisor** and
**gvisor** are planned; each is one drop-in package under `internal/backend/`
plus a blank import in `cmd/sbx/main.go`, so neither needs a change to the
dispatch layer or the CLI.

- **docker** — a container from the given image. The image's `ENTRYPOINT` is the
  sandbox agent, so the container serves requests as soon as it is created.
  Resources are bounded with `--cpus/--memory/--cpuset-cpus/--cpuset-mems`.
- **firecracker** — a microVM with TAP networking (one /30 per slot inside
  `198.19.0.0/16`: subnet base `slot*4`, host `base+1`, guest `base+2` — slot 0
  is guest `198.19.0.2`, slot 1 is `198.19.0.6`; up to 16384 slots), kernel and
  jailer pinned via `SBX_FIRECRACKER` / `SBX_JAILER` / `SBX_KERNEL`, systemd and
  the agent injected into the cached rootfs template. The VM is jailed
  (`jailer --daemonize`), cgroup v2 bounded (`cpu.max`, `memory.max`, optional
  `cpuset.*`) and NUMA pinned.
- **cloud-hypervisor** (planned) — a cloud-hypervisor microVM driven purely
  through its typed REST API (`vm.create`/`vm.boot`). Host plumbing (TAP,
  cached rootfs, kernel resolution) is shared with firecracker via `sbxhost`.
  cgroup v2 bounded and NUMA pinned; there is no jailer. Per-arch kernel:
  `Image` (PE/EFI-stub) on aarch64, `bzImage` (ELF) on x86_64.
- **gvisor** (planned) — the docker model under gVisor's `runsc` OCI runtime
  (`--runtime=runsc`, overridable via `SBX_GVISOR_RUNTIME`). It embeds the docker
  backend and overrides only its environment check, so it carries one extra
  platform requirement: gVisor's sentry supports 4 KiB host pages only, and
  `runsc` dies at start on a 64 KiB-page kernel.

Every backend runs an environment check before `create`, so misconfiguration
surfaces as a clear error instead of a mid-flight crash:

- docker: `docker` CLI present + daemon reachable
- firecracker: `firecracker` / `jailer` exist and are executable, cgroup v2 with
  the `cpu`/`memory` controllers enabled, kernel is a valid ELF or PE (EFI)
  image, sandbox agent binary is available

### Adding a backend (drop-in)

Backends are plugins behind a minimal Go interface (`internal/sbx/backend.go`):

```go
type Backend interface {
    Name() string
    Create(opt Options, out *Info) error
    Stop(id string) error
    Remove(id string) error
    CheckEnv() error
}
```

A backend self-registers once in its package `init()` with
`sbx.Register("name", impl)`. Adding an `internal/backend/<name>/` package that
calls `Register` and blank-importing it from `cmd/sbx/main.go` is enough —
`sbx doctor`/`create`/`stop`/`remove` pick it up, and `sbx help`/CLI errors list
it automatically. No changes to `internal/cli/`, the dispatch layer or the
Makefile are needed.

Plumbing available to every implementation:

- **create args** — unknown `--key=value` flags land in `Options.Args`; read them
  with `sbx.BackendOpt(args, name, key, def)`, which falls back to
  `SBX_<BACKEND>_<KEY>` then `def`.
- **runtime state** — store resource refs (ids, endpoints) in `Info.Meta`
  (`map[string]string`); it is persisted in the state JSON and restored on load,
  so `stop`/`remove` recover the backend's own resources.
- **agentless** — a backend need not boot `sbx-agent` / wait for `/health`; then
  `create` prints no `agent:` line. `exec`/`upload`/`download` still need one.
- **SDKs** — a backend links what it needs in its own package; no shared Makefile
  plumbing.
- **networking / NUMA** — reuse `internal/sbx` (`ResolvePlacement`,
  `NumaAllocation`) and `internal/sbxhost` (slots, TAP, iptables, kernel,
  rootfs). The microVM backends share `sbxhost`; container backends do not.

The `Backend` lifecycle is driven entirely by the dispatch layer in
`internal/sbx` (`Create`/`Stop`/`Remove`), which dispatches on the registered
backend name and handles `Info` persistence, NUMA release and cleanup
bookkeeping.

## Implementation

```
┌──────────────────────────────  host  ────────────────────────────────────┐
│   sbx CLI   (cmd/sbx + internal/cli/)                                    │
│     create / exec / upload / download / stop / remove / list             │
│     doctor / rootfs                                                      │
│       │ JSON state              │ HTTP (to the guest agent)              │
│       ▼                         ▼                                        │
│   Backend registry (internal/sbx + internal/backend)                     │
│   ┌────────────┐   ┌──────────────────────────────┐                      │
│   │  docker    │   │  firecracker                 │                      │
│   │ docker SDK │   │  firecracker-go-sdk + jailer │                      │
│   └─────┬──────┘   └──────────────┬───────────────┘                      │
│         │                         │                                      │
│         │                    host plumbing (internal/sbxhost):           │
│         │                    slot → TAP fhvmN, iptables DNAT :P,         │
│         │                    kernel, cached guest rootfs                 │
│         ▼                         ▼                                      │
│   container                 microVM (kernel + rootfs.ext4)               │
│   ENTRYPOINT:               systemd → sbx-agent.service                  │
│   agentbench-agent               → sbx-agent  HTTP :9119                 │
│         └──────────────┬──────────┘                                      │
│                        ▼                                                 │
│               sbx-agent  (C++, src/agent/) runs INSIDE the sandbox       │
│                 GET  /health          no auth                            │
│                 POST /exec[/…]        NDJSON stream                      │
│                 POST /files/write     upload (tar.gz)                    │
│                 POST /files/read      download                           │
│                 … every other route needs Bearer <token>                 │
└──────────────────────────────────────────────────────────────────────────┘
```

### Firecracker `create` pipeline

```
create
 ├─ slot alloc: findFreeSlot() — O_CREAT|O_EXCL on ~/.sbx/slots/slotN.lock
 │     (atomic claim, N 0..16383, released on remove) → tap fhvmN, /30 in
 │     198.19.0.0/16
 ├─ setupTap (netlink): LinkAdd(TAP) + addr /30 + link up + ip_forward=1
 ├─ NUMA: NumaAllocate() records the placement and pins cpuset.cpus + cpuset.mems
 │     to the least-loaded node (guarded by ~/.sbx/numa.lock). Fully
 │     overcommitted: no capacity checks — sizes only set the cpu.max /
 │     memory.max quotas. Explicit --cpuset-cpus / --cpuset-mems are honored
 │     verbatim for dedicated placement.
 ├─ rootfs: ensureInjectedRootfs() — copy of the cached injected.ext4 built by
 │     `make rootfs` (`sbx rootfs build`); a missing/stale template stops the
 │     create (rc=3) with the command that builds it. Injecting an agent whose
 │     hash changed is the only work left here (a local copy; the
 │     <cache>/.ensure.lock flock serializes it)
 ├─ copy kernel + injected.ext4 into the per-VM chroot (no mount)
 ├─ jailer spawn (structured exec): jailer --exec-file firecracker --id <id>
 │     --chroot-base-dir ... --daemonize --new-pid-ns --cgroup-version 2
 │     (cpu.max / memory.max / cpuset.*) -- --api-sock run/firecracker.socket
 ├─ fc-go-sdk (over jailer api socket): PUT /boot-source, /drives/rootfs,
 │     /machine-config, /network-interfaces/eth0, POST /actions InstanceStart
 │     (boot_args carries agentbench.token=<token>)
 ├─ setupPortForward (go-iptables): DNAT (only when --port P)
 └─ waitAgent(): poll GET /health until 200 (or timeout)
```

On any failure the pipeline jumps to a single `cleanup` that kills the spawned
VM, removes the TAP, the chroot, the NUMA reservation and the slot lock — so a
partially created sandbox never leaks processes or state.

### Agent HTTP API

| Method | Path | Purpose |
|---|---|---|
| GET  | `/health` | liveness probe |
| POST | `/exec` | run a command; streams `started`/`stdout`/`stderr`/`exit` events as NDJSON; `POST /exec/<id>/stdin|/resize` for tty, `POST /exec/<id>/kill` to kill |
| POST | `/files/write` | extract a tar payload into the guest (`X-Extract-Dir`) |
| POST | `/files/read` | pack guest paths into a tar and stream it back |

All requests require `Authorization: Bearer <token>`. The token is random per
VM, passed on the firecracker kernel command line or via `SBX_AGENT_TOKEN` for
container backends, and stored in the state file.

### Teardown

`stop` kills the VM process and frees the TAP/port-forward; `remove`
additionally writes `cgroup.kill`, deletes the chroot, retries `rmdir` on the
cgroup, releases the NUMA allocation and the slot lock, and unlinks the state
file. State files are written atomically (`.tmp` + rename), so concurrent
`list`/`remove` can never observe a half-written JSON.

## NUMA-aware placement

On multi-NUMA hosts (enabled by default; `SBX_NUMA=0` disables) each sandbox is
pinned so CPU and memory stay on the same node:

- multiple sandboxes **pack onto one node** before a new node is used; a request
  only **spans nodes** when no single node fits the spec
- restrict usable nodes with `SBX_NUMA_NODES=0,2`
- allocations are tracked in `~/.sbx/numa.json` (released on stop/remove) under
  a cross-process file lock (`~/.sbx/numa.lock`)
- explicit `--cpuset-cpus` / `--cpuset-mems` pin an exact CPU/memory set (see the
  [CLI](#cli) section) and skip this bookkeeping

## Environment variables

| Variable | Default | Purpose |
|---|---|---|
| `SBX_HOME` | `~/.sbx` | root of all host-side state |
| `SBX_KERNEL` | per-family/arch default (`bzImage` on x86_64 / `Image` on aarch64) | absolute kernel path override |
| `SBX_FIRECRACKER` | `~/.sbx/bin/firecracker` | firecracker binary |
| `SBX_JAILER` | `~/.sbx/bin/jailer` | jailer binary |
| `SBX_AGENT_BIN` | `<bin>/sbx-agent` | sandbox agent binary |
| `SBX_TMP_DIR` | `/tmp` | temp dir for uploads/rootfs mounting |
| `SBX_IMAGE` | `agentbench:latest` | default image for `agentbench -s` and `sbx rootfs` |
| `SBX_BACKEND` | `firecracker` | default backend for `agentbench -s` |
| `SBX_GVISOR_RUNTIME` | `runsc` | OCI runtime for the **planned** gvisor backend; read only when a backend is constructed with a non-empty runtime, which today's `docker` backend is not |
| `SBX_CPUS` | `2` | default vCPUs for `agentbench -s` |
| `SBX_MEMORY_MB` | `4096` | default memory (MiB) for `agentbench -s` |
| `SBX_HEALTH_SEC` | `60` (docker: `30`) | how long `create` waits for the agent's `/health` before it gives up |
| `SBX_KEEP_FAILED` | unset | `1` leaves a sandbox whose bring-up failed in place for inspection (no teardown, no retry) |
| `SBX_CREATE_RETRIES` | `1` | how many extra `create` attempts `agentbench -s` makes when a bring-up fails |
| `SBX_EXEC_TIMEOUT` | unset | per-sandbox `exec` budget (seconds) for `agentbench -s`, overriding the `120 + 60*iterations` default; a sandbox killed by it says so instead of just "no timing data" |
| `SBX_NUMA` | `1` | `0`/`off` disables NUMA pinning |
| `SBX_NUMA_NODES` | all online | restrict to nodes, e.g. `0,2` |
| `SBX_<BACKEND>_<KEY>` | — | per-backend option fallback (see `BackendOpt`); e.g. `SBX_FIRECRACKER_CPUSET_CPUS` |

**Put the state on a reflink-capable filesystem.** Each sandbox gets its own copy
of the ~1 GB rootfs template (~120 MB of real data). On XFS with `reflink=1` (or
btrfs) `sbx` copies it with a reflink, so the sandbox costs a few MiB of
copy-on-write blocks instead of a full copy — measured on our test host, three
sandboxes for 10 MiB of disk and 0.05 s of system time in `create`, against
~360 MiB and 0.11 s on ext4. Filesystems without reflink fall back to the plain
sparse copy. Point `SBX_HOME` at such a filesystem, or symlink `~/.sbx` to a
directory on it.

## Layout

```
sandbox/
  go.mod / go.sum               Go module (host-side sbx CLI)
  cmd/sbx/main.go               sbx CLI entry point (Go)
  internal/
    sbx/                        core: Options/Info, ~/.sbx state persistence,
                                NUMA placement (NumaAllocate/ResolvePlacement),
                                backend registry, agent /health polling
    sbxhost/                    host-side plumbing shared by the backends:
                                kernel resolution, cached rootfs/injection,
                                slot/TAP/IP allocation, iptables forwarding,
                                log tailing
    cli/                        command parsing + handlers (create/exec/...)
    agentclient/                HTTP client for the sbx-agent protocol (NDJSON)
    backend/
      docker/                   Docker backend (Docker Engine SDK)
      firecracker/              Firecracker backend (firecracker-go-sdk +
                                netlink TAP + go-iptables + jailer exec + rootfs)
  src/
    agent/main.cpp              sbx-agent — C++ HTTP agent run inside the sandbox
    common/                     http / json / util helpers used by the agent
  Makefile                      builds bin/sbx (Go) + bin/sbx-agent (C++)
  bin/                          built sbx + sbx-agent
```

### Layer boundary

`internal/backend/{docker,firecracker}` are the decision+execution backends: they
pick images, sizes, runtimes, NUMA pinning, kernels and ports, and choose which
cached rootfs template to boot. Execution uses the official SDKs plus a few
structured `exec.Command` calls for tools with no SDK (the `jailer` binary,
`mkfs.ext4`, loop `mount`/`umount`) — every argv is a typed slice, with no
command-line string concatenation.

The shared helpers live in `internal/sbx` (Options/Info, state JSON, the NUMA
ledger under `~/.sbx/numa.lock`, the registry). Backends call
`sbx.ResolvePlacement()` for placement and persist `Info` via `Info.Save()`,
matching the legacy C++ layer's semantics so existing `~/.sbx` state and scripts
keep working.
