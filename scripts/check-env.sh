#!/usr/bin/env bash
# Environment checks run before any real compilation (invoked from the
# top-level Makefile). Verifies:
#
#   1. A working Docker environment — docker CLI present and the daemon
#      reachable. Required by the sbx tool, the docker backend and `make image`.
#   2. A static libstdc++ is installed so -static-libstdc++/-static-libgcc
#      linking works, with distro-specific install hints for mainstream
#      distributions (CentOS/RHEL/Fedora, Debian/Ubuntu, openSUSE, Arch,
#      Alpine, ...).
#
# Exits 0 only when every check passes. Bypass with: SKIP_ENV_CHECK=1 make
#
# The docker check is only required for docker-dependent artifacts (image,
# sbx); pure compilation targets (agentbench, sbx-agent) skip it so builds
# inside the Docker container (which has no docker CLI) still work.
#   CHECK_DOCKER=0 make  → skip the docker check only
#   SKIP_ENV_CHECK=1 make → skip all checks
set -u

CXX="${CXX:-g++}"
CHECK_DOCKER="${CHECK_DOCKER:-1}"
FAILURES=0

fail() {
    echo "ERROR: $*" >&2
    FAILURES=$((FAILURES + 1))
}

detect_distro() {
    if [ -r /etc/os-release ]; then
        # shellcheck disable=SC1091
        . /etc/os-release
        DISTRO_ID="${ID:-unknown}"
        DISTRO_LIKE="${ID_LIKE:-}"
        DISTRO_NAME="${NAME:-$DISTRO_ID}"
    elif [ -r /etc/redhat-release ]; then
        DISTRO_ID="rhel"
        DISTRO_LIKE="fedora"
        DISTRO_NAME="Red Hat / CentOS family"
    else
        DISTRO_ID="unknown"
        DISTRO_LIKE=""
        DISTRO_NAME="unknown"
    fi
}

check_docker() {
    if ! command -v docker >/dev/null 2>&1; then
        fail "docker CLI not found. Install Docker Engine first, e.g.:"
        case "$DISTRO_ID" in
            centos|rhel|almalinux|rocky|ol|amzn)
                echo "      sudo dnf install -y docker-ce docker-ce-cli" >&2 ;;
            debian|ubuntu|linuxmint|pop|kali)
                echo "      sudo apt-get install -y docker.io" >&2 ;;
            fedora)
                echo "      sudo dnf install -y docker-ce docker-ce-cli" >&2 ;;
            arch|manjaro)
                echo "      sudo pacman -S docker" >&2 ;;
            alpine)
                echo "      sudo apk add docker" >&2 ;;
            *)
                echo "      see https://docs.docker.com/engine/install/" >&2 ;;
        esac
        return
    fi
    if ! docker info >/dev/null 2>&1; then
        fail "docker daemon is not reachable. Start it, e.g.:"
        echo "      sudo systemctl start docker (and enable it: sudo systemctl enable docker)" >&2
    else
        echo "ok: docker CLI and daemon are available ($(docker version --format '{{.Server.Version}}' 2>/dev/null || echo 'version unknown'))"
    fi
}

check_static_libstdcxx() {
    if ! command -v "$CXX" >/dev/null 2>&1; then
        fail "C++ compiler '$CXX' not found. Install a toolchain (e.g. g++)."
        return
    fi
    local lib
    lib=$("$CXX" -print-file-name=libstdc++.a)
    if [ "$lib" = "libstdc++.a" ] || [ ! -f "$lib" ]; then
        fail "static libstdc++ (libstdc++.a) not found; '-static-libstdc++' linking will fail. Install it:"
        case "$DISTRO_ID" in
            centos|rhel|almalinux|rocky|ol|amzn|fedora)
                echo "      sudo dnf install -y libstdc++-static" >&2
                echo "      (older releases: sudo yum install -y libstdc++-static)" >&2 ;;
            debian|ubuntu|linuxmint|pop|kali)
                echo "      sudo apt-get install -y g++" >&2
                echo "      (the static libstdc++.a ships with the libstdc++-<ver>-dev / g++ packages)" >&2 ;;
            opensuse-leap|opensuse-tumbleweed|suse)
                echo "      sudo zypper install -y libstdc++-devel-static" >&2 ;;
            arch|manjaro)
                echo "      sudo pacman -S gcc" >&2 ;;
            alpine)
                echo "      sudo apk add libstdc++-static" >&2 ;;
            *)
                echo "      static libstdc++ is missing on $DISTRO_NAME; install the distro's equivalent of libstdc++-static" >&2 ;;
        esac
    else
        echo "ok: static libstdc++ found at $lib"
    fi
}

detect_distro
echo "=== environment check (docker + static libstdc++) ==="
echo "distribution: $DISTRO_NAME ($DISTRO_ID)"
if [ "$CHECK_DOCKER" = "1" ]; then
    check_docker
else
    echo "skipped: docker check (not required for this target)"
fi
check_static_libstdcxx

if [ "$FAILURES" -ne 0 ]; then
    echo ""
    echo "environment check FAILED ($FAILURES problem(s)); fix the issues above and re-run make." >&2
    exit 1
fi
echo "environment check passed."
