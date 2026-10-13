#!/usr/bin/env bash
# Run the full simulation-mode benchmark suite inside a Firecracker microVM
# managed by sbx. The guest rootfs it boots is built by `make rootfs`
# (cached under ~/.sbx/registry/rootfs/agentbench/latest/); a run never builds
# one, so a missing or stale template fails fast with the command that fixes it.
#
# Env overrides: SBX VCPUS MEMORY THREADS ITERATIONS KEEP_SANDBOX
#   SBX           path to the sbx CLI   (default: sandbox/bin/sbx)
#   VCPUS         microVM vCPU count    (default: 2)
#   MEMORY        microVM memory in MiB (default: 4096)
#   THREADS       agentbench threads    (default: 2)
#   ITERATIONS                         (default: 1)
#   KEEP_SANDBOX=1 to leave the VM running after the run (default: cleanup)
#
# The image is built with a conservative portable ISA by default
# (`./scripts/build-image.sh`), so it runs in Firecracker vCPUs without SVE.
# Do NOT build it with MARCH=native for Firecracker runs (the guest would
# crash with "Illegal instruction", exit 132).
set -euo pipefail
cd "$(dirname "$0")/.."

SBX="${SBX:-./sandbox/bin/sbx}"
NAME="${SANDBOX_NAME:-bench-firecracker}"
VCPUS="${VCPUS:-2}"
MEMORY="${MEMORY:-4096}"
THREADS="${THREADS:-2}"
ITERATIONS="${ITERATIONS:-1}"
KEEP_SANDBOX="${KEEP_SANDBOX:-0}"

# Host environment preflight: verifies cgroup v2 with cpu/memory controllers,
# the firecracker/jailer binaries, the guest OS kernel and the sandbox agent.
# Fail fast with a clear error before creating the VM.
"$SBX" doctor --backend firecracker

"$SBX" create --backend firecracker --image agentbench:latest --name "$NAME" --cpus "$VCPUS" --memory "$MEMORY" >/dev/null

cleanup() {
    "$SBX" stop "$NAME" >/dev/null 2>&1 || true
    "$SBX" remove "$NAME" >/dev/null 2>&1 || true
}
if [ "$KEEP_SANDBOX" = "0" ]; then
    trap cleanup EXIT
fi

"$SBX" exec "$NAME" --timeout 600000 -- \
    bash -c "cd /opt/agentbench && agentbench --mode=simulation -t $THREADS -i $ITERATIONS -j"