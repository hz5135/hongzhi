#!/usr/bin/env bash
# Build the simulation-mode agentbench sources into a Docker image.
# Output: agentbench:latest (kept in the local daemon, not pushed).
#
# By default the image is compiled for a conservative, portable ISA so it runs
# in every sandbox backend out of the box — including Firecracker microVMs,
# whose vCPUs do not expose every host extension (e.g. SVE/SVE2 on aarch64).
# No user choice is required.
#
# If you need the maximum possible host performance for Docker-only runs, you
# can opt in explicitly:
#   MARCH=native ./scripts/build-image.sh
#
# The base-image source (remote mirror registry) is configurable via the
# MIRROR_REGISTRY env var; the base image becomes
# "<MIRROR_REGISTRY>/ubuntu:22.04", or plain "ubuntu:22.04" when empty:
#   MIRROR_REGISTRY= ./scripts/build-image.sh   (Docker Hub: ubuntu:22.04, default)
#   MIRROR_REGISTRY=registry.example.com/mirror ./scripts/build-image.sh
#
# Set it when Docker Hub is not reachable from the build host. Do not point it
# at a registry whose copy of the base image is a single foreign architecture:
# an x86_64 host building against an arm64 base fails inside the first RUN with
# "exec /bin/sh: exec format error".
set -euo pipefail
cd "$(dirname "$0")/.."

detect_default_march() {
    case "$(uname -m)" in
        aarch64|arm64) echo "armv8-a" ;;
        x86_64|amd64)  echo "x86-64" ;;
        *)             echo "native" ;;
    esac
}

MARCH="${MARCH:-$(detect_default_march)}"
MIRROR_REGISTRY="${MIRROR_REGISTRY:-}"

exec docker build \
    --build-arg MARCH="$MARCH" \
    --build-arg MIRROR_REGISTRY="$MIRROR_REGISTRY" \
    --build-arg IMAGE="${IMAGE:-ubuntu:22.04}" \
    -t agentbench:latest -f docker/Dockerfile .
