# syntax=docker/dockerfile:1

# AgentBench — LuaJIT + C/C++ benchmark suite container image.
#
# Multi-stage build:
#   stage 1 (builder)  compiles the sandbox agent (the only thing still built
#                      in-container; the benchmark binary comes from the host).
#   stage 2 (runtime)  ships the host-built binary + the lua scripts + the agent.
#
# Build:
#   docker build -t agentbench:latest -f docker/Dockerfile .
#   (prefer `./scripts/build-image.sh` / `make image`, which default to a
#    conservative portable ISA so the image runs in every sandbox backend)
#
# MIRROR_REGISTRY selects the base-image source (remote registry prefix):
#   - registry.example.com/mirror       → registry.example.com/mirror/ubuntu:22.04
#   - (empty, default)                  → ubuntu:22.04 (Docker Hub)
# Set it to a Docker Hub proxy when the default registry is not reachable from
# the build host. Do not point it at a registry whose copy of the base image is
# a single foreign architecture: an x86_64 host building against an arm64 base
# fails inside the first RUN with "exec /bin/sh: exec format error".
#   Pass it with `docker build --build-arg MIRROR_REGISTRY=...`, or export the
#   MIRROR_REGISTRY env var and build via `./scripts/build-image.sh` / `make image`.
# UBUNTU_IMAGE additionally overrides the base image name/tag if needed.
#
# MARCH controls the ISA the binary is compiled for.
#   Default when built via scripts/build-image.sh:
#     aarch64 -> armv8-a, x86_64 -> x86-64  (portable, sandbox-safe)
#   If you build with plain `docker build` and want host-native performance
#   (Docker backend only), pass `--build-arg MARCH=native`.

ARG MIRROR_REGISTRY=
ARG UBUNTU_IMAGE=ubuntu:22.04
ARG BASE_IMAGE=${MIRROR_REGISTRY:+${MIRROR_REGISTRY}/}${UBUNTU_IMAGE}
ARG MARCH=native

# ---------------------------------------------------------------------------
# Stage 1 — build
# ---------------------------------------------------------------------------
FROM ${BASE_IMAGE} AS builder
ARG MARCH=native

ENV DEBIAN_FRONTEND=noninteractive
ENV CC=gcc CXX=g++
# No CFLAGS/CXXFLAGS here any more: the only thing this stage still compiles is
# the guest agent, and the make line below passes its flags explicitly.

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Builds the sandbox agent (sandbox/bin/sbx-agent) used by the `sbx` CLI inside
# sandboxes.  The benchmark itself is NOT built here any more: the runtime stage
# takes the binary the host built (`make agentbench`, portable ISA by default),
# so the file a guest runs is the file bare metal runs — which is what makes a
# `-s` comparison a comparison of environments rather than of two builds.
# Stale in-container artifacts are removed first so make cannot reuse them.
RUN rm -rf sandbox/build sandbox/bin \
 && make -C sandbox CFLAGS="-O3 -march=${MARCH}" \
              CXXFLAGS="-O3 -march=${MARCH} -std=gnu++17" sbx-agent

# ---------------------------------------------------------------------------
# Stage 2 — runtime
# ---------------------------------------------------------------------------
FROM ${BASE_IMAGE} AS runtime

WORKDIR /opt/agentbench

# From the build context (the host build), not from the builder stage — see the
# note above.  A missing file fails the build loudly, which is the point: the
# image is no longer self-contained.
COPY agentbench /usr/local/bin/agentbench
COPY lua ./lua
# NOTE: installed as `agentbench-agent`, NOT `sbx-agent`. On some self-built
# kernels the overlay2 driver serves the old `usr/local/bin/sbx-agent` inode as
# EPERM inside containers (stat/open fail with "Operation not permitted"
# although the layer tar and the physical diff file are fine); a different path
# sidesteps the broken inode. Revisit if such a kernel is fixed. The guest
# rootfs injection (internal/sbxhost/rootfs.go) installs the agent under its own
# name, so the two paths are independent.
COPY --from=builder /src/sandbox/bin/sbx-agent /usr/local/bin/agentbench-agent

# The sandbox agent is the container entrypoint: it starts the HTTP agent
# inside the container so exec/upload/download requests can be served.
RUN chmod +x /usr/local/bin/agentbench-agent
ENTRYPOINT ["/usr/local/bin/agentbench-agent"]
ENV AGENTBENCH_TMPDIR=/tmp