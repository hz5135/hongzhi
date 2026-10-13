#!/usr/bin/env bash
# Prepare the guest rootfs template that the microVM backends
# (firecracker, cloud-hypervisor) boot, so that a benchmark run never has to
# build one: `sbx create` only copies the finished template.
#
# Output: ~/.sbx/registry/rootfs/<image>/<tag>/{rootfs.ext4,injected.ext4}
#
# This runs the derived image build (systemd + the guest tools on top of the
# image built by build-image.sh), exports it to ext4 and bakes the sandbox agent
# in - ~40s, cached and invalidated when the image id or the agent binary
# changes. `make rootfs` runs this after `make image` (and again whenever
# sbx-agent is rebuilt); run it on its own with
#   ./scripts/build-rootfs.sh
#   SBX_IMAGE=agentbench:latest ./scripts/build-rootfs.sh
#
# SBX_IMAGE selects the image (default agentbench:latest), SBX the sbx binary
# (default ./sbx).
set -euo pipefail
cd "$(dirname "$0")/.."

IMAGE="${SBX_IMAGE:-agentbench:latest}"
SBX="${SBX:-./sbx}"

if [ ! -x "$SBX" ]; then
    echo "build-rootfs.sh: $SBX not found - build the sandbox CLI first: make sbx" >&2
    exit 1
fi
# An sbx older than this source tree has no `rootfs` command, and the error it
# prints for that ("unknown command 'rootfs'") does not say why: name the fix.
if ! "$SBX" help 2>/dev/null | grep -q '^  rootfs'; then
    echo "build-rootfs.sh: $SBX does not support \`rootfs\` - it is older than" >&2
    echo "                this source tree. Rebuild/install the CLI first: make sbx" >&2
    exit 1
fi

# The template is built for the guest, which does not run the host's libc/etc.;
# sbx reports what it built (`guest rootfs for <image>: ...`), including whether
# it was already current.
exec "$SBX" rootfs build --image "$IMAGE"
