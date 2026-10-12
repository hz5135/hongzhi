# AgentBench — LuaJIT + C/C++ benchmark suite
#
# Framework: C++ (g++)
# Benchmark kernels: C (gcc, extern "C" for LuaJIT FFI)
# LuaJIT and libcurl vendored under third_party/, built from source.

# CFLAGS/CXXFLAGS use plain assignment (not ?=) so a CFLAGS exported in the
# shell environment (e.g. gcc-toolset's `scl enable` sets -gdwarf-5) cannot
# leak into the build and break older compilers. To override, pass them on the
# make command line: `make CFLAGS=...`.
# The ISA the benchmark is built for. Portable by default so the binary runs on
# a CPU that does not expose every extension of the build host; a
# -march=native build of this tree SIGILLs on such a machine. Opt into the
# build host's own ISA with `make MARCH=native`. (Written with $(if)/$(filter)
# rather than a shell `case`: the `)` of a case pattern would close $(shell).)
UNAME_M := $(shell uname -m)
MARCH   ?= $(if $(filter aarch64 arm64,$(UNAME_M)),armv8-a,$(if $(filter x86_64 amd64,$(UNAME_M)),x86-64,native))
CC      ?= gcc
CXX     ?= g++
CFLAGS  := -O3 -march=$(MARCH) -g -Wall -Wextra -MMD -MP
CXXFLAGS:= -O3 -march=$(MARCH) -g -Wall -Wextra -std=gnu++17 -MMD -MP
# Statically link libstdc++/libgcc so the binary runs on hosts whose system
# libstdc++ is older than the compiler (a GCC 13/15 build needs GLIBCXX_3.4.32+,
# which stock RHEL/older systems lack) — no LD_LIBRARY_PATH required.
CXXFLAGS += -static-libstdc++ -static-libgcc
LDFLAGS += -Wl,--export-dynamic

BUILD_DIR := build
CC_VERSION := $(shell $(CC) -dumpfullversion -dumpversion 2>/dev/null)
CXX_VERSION := $(shell $(CXX) -dumpfullversion -dumpversion 2>/dev/null)
# The stamp carries the ISA too: objects built for one -march must not be
# reused for another, or switching MARCH would silently keep the old code.
TOOLCHAIN_STAMP := $(BUILD_DIR)/.toolchain-$(CC_VERSION)-$(CXX_VERSION)-$(MARCH).stamp

# --- Vendored third-party libraries ---
LUAJIT_DIR := third_party/luajit
LUAJIT_VENDORED_LIB := $(LUAJIT_DIR)/src/libluajit.a

# LuaJIT backend: default to the vendored source tree (third_party/luajit,
# upstream LuaJIT 2.1 from https://github.com/luajit/luajit).
# Set `make LUAJIT=system` to build against the distro package instead
# (e.g. Debian/Ubuntu libluajit-5.1-dev, resolved via pkg-config).
ifeq ($(LUAJIT),system)
  LUAJIT_CPPFLAGS := $(shell pkg-config --cflags luajit)
  LUAJIT_LINK     := $(shell pkg-config --libs luajit)
  LUAJIT_LIB_DEP  :=
else
  LUAJIT_CPPFLAGS := -I$(LUAJIT_DIR)/src
  LUAJIT_LINK     := $(LUAJIT_VENDORED_LIB)
  LUAJIT_LIB_DEP  := $(LUAJIT_VENDORED_LIB)
endif

CURL_VERSION := 8.11.0
CURL_DIR     := third_party/curl-$(CURL_VERSION)
CURL_LIB     := $(CURL_DIR)/lib/.libs/libcurl.a

CPPFLAGS += -Isrc -Isrc/benchmarks/include -Isrc/benchmarks/common $(LUAJIT_CPPFLAGS) -I$(CURL_DIR)/include
CXXFLAGS += -I/usr/include/
LDLIBS   += -lm -lpthread -ldl

# --- Framework sources (C++) ---
FRAMEWORK_SRCS := \
	src/main.cpp \
	src/run.cpp \
	src/suite.cpp \
	src/stats.cpp \
	src/options.cpp \
	src/lua_bridge.cpp \
	src/export.cpp

FRAMEWORK_OBJS := $(FRAMEWORK_SRCS:src/%.cpp=$(BUILD_DIR)/%.o)

# --- Benchmark kernel sources (C, extern "C") ---
# One entry per benchmark kernel; added by the PR that introduces the kernel.
BENCH_SRCS := \
	src/benchmarks/json_processing.c

# --- Helpers the kernels share (src/benchmarks/common/, header beside each implementation) ---
# workers is the thread fan-out every kernel uses; the rest are pulled in by the
# kernel that needs them.
BENCH_SRCS += \
	src/benchmarks/common/hash.c \
	src/benchmarks/common/json_dom.c \
	src/benchmarks/common/json_escape.c \
	src/benchmarks/common/strbuf.c \
	src/benchmarks/common/strlist.c \
	src/benchmarks/common/text.c \
	src/benchmarks/common/workers.c

BENCH_OBJS := $(BENCH_SRCS:src/%.c=$(BUILD_DIR)/%.o)

# Header dependencies (from -MMD): the pattern rules below list only the
# .c/.cpp file, so without this a change to a shared header (core.h /
# pyrand.h) would leave stale objects that silently disagree with the
# sources. `make clean` removes them together with $(BUILD_DIR).
DEPS := $(FRAMEWORK_OBJS:.o=.d) $(BENCH_OBJS:.o=.d)
-include $(DEPS)

# --- Default goal: build the binary ---
# Stated explicitly: the -include of the .d files above happens before the first
# target, and GNU make takes the default goal from the first rule it reads, which
# would otherwise be build/main.o.
.DEFAULT_GOAL := agentbench

# --- Environment check: static libstdc++ ---
# Required because the link uses -static-libstdc++/-static-libgcc above. Bypass
# the check with: make SKIP_ENV_CHECK=1
.PHONY: envcheck-static
envcheck-static:
	@if [ -n "$(SKIP_ENV_CHECK)" ]; then \
	  echo "envcheck: skipped (SKIP_ENV_CHECK set)"; \
	else \
	  lib="$$($(CXX) -print-file-name=libstdc++.a)"; \
	  if [ "$$lib" = "libstdc++.a" ] || [ ! -f "$$lib" ]; then \
	    echo "ERROR: static libstdc++ (libstdc++.a) not found; the '-static-libstdc++' link will fail." >&2; \
	    echo "       install it, e.g. 'dnf install -y libstdc++-static' (RHEL family) or the" >&2; \
	    echo "       equivalent for your distribution." >&2; \
	    exit 1; \
	  fi; \
	  echo "ok: static libstdc++ found at $$lib"; \
	fi

$(TOOLCHAIN_STAMP):
	@mkdir -p $(dir $@)
	@printf '%s\n' "CC=$(CC)" "CC_VERSION=$(CC_VERSION)" "CXX=$(CXX)" "CXX_VERSION=$(CXX_VERSION)" > "$@"

# --- Build vendored LuaJIT ---
$(LUAJIT_VENDORED_LIB):
	$(MAKE) -C $(LUAJIT_DIR)/src \
	    BUILDMODE=static \
	    CFLAGS="-O3 -fPIC" \
	    CC=$(CC)

# --- Build vendored libcurl from source (third_party/curl-8.11.0/) ---
# CFLAGS is set explicitly so an ambient CFLAGS in the environment (e.g. one
# carrying a toolchain-specific -gdwarf-5) cannot leak into the curl build and
# break older compilers.
# curl-config-check reconfigures when a stale curl/Makefile from an older
# checkout (configured for a toolchain whose AR path no longer exists) is
# present, instead of reusing it. No-op when the existing config is valid.
.PHONY: curl-config-check
curl-config-check:
	@if test -f $(CURL_DIR)/Makefile && test -f $(CURL_DIR)/config.status && \
	      ! grep -q "$(shell command -v ar || echo __no_ar__)" $(CURL_DIR)/lib/Makefile 2>/dev/null; then \
	  echo "curl: stale toolchain config detected, reconfiguring"; \
	  cd $(CURL_DIR) && make distclean >/dev/null 2>&1; \
	fi

$(CURL_DIR)/Makefile: curl-config-check
	cd $(CURL_DIR) && test -f Makefile || CFLAGS="-O3" ./configure --disable-shared --enable-static --with-pic \
	    --without-ssl --without-zlib --without-brotli --without-zstd \
	    --without-libpsl --disable-ftp --disable-file --disable-ldap \
	    --disable-rtsp --disable-proxy --disable-dict --disable-telnet \
	    --disable-tftp --disable-pop3 --disable-imap --disable-smtp \
	    --disable-gopher --disable-mqtt --disable-ntlm --disable-tls-srp \
	    --disable-cookies --disable-doh --disable-sspi --disable-http-auth \
	    --disable-unix-sockets --disable-hsts

$(CURL_LIB): $(CURL_DIR)/Makefile
	$(MAKE) -C $(CURL_DIR)/lib

# --- Compile framework (C++) ---
$(BUILD_DIR)/%.o: src/%.cpp $(TOOLCHAIN_STAMP)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<

# --- Compile benchmark kernels (C) ---
$(BUILD_DIR)/%.o: src/%.c $(TOOLCHAIN_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

agentbench: envcheck-static $(FRAMEWORK_OBJS) $(BENCH_OBJS) $(LUAJIT_LIB_DEP) $(CURL_LIB)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(FRAMEWORK_OBJS) $(BENCH_OBJS) $(LUAJIT_LINK) $(CURL_LIB) $(LDLIBS)

# --- Smoke test ---
test: agentbench
	@echo "=== version ==="
	./agentbench --version
	@echo ""
	@echo "=== list simulation ==="
	./agentbench list simulation
	@echo ""
	@echo "=== list client ==="
	./agentbench list client
	@echo ""
	@echo "=== simulation mode (single benchmark, json) ==="
	./agentbench --mode=simulation -t 1 -i 1 -b json_processing -j
	@echo ""
	@echo "ALL SMOKE TESTS PASSED"

# --- Clean ---
# Removes the build outputs, and the two vendored libraries that are only valid
# for the toolchain that produced them (a stale libcurl configured for a
# different AR/CC pair is what curl-config-check exists to catch).
clean:
	rm -rf $(BUILD_DIR) agentbench
	@if [ "$(LUAJIT)" != "system" ]; then $(MAKE) -C $(LUAJIT_DIR)/src clean; fi
	-test -f $(CURL_DIR)/Makefile && $(MAKE) -C $(CURL_DIR)/lib distclean 2>/dev/null || true
	rm -f $(CURL_LIB)
	rm -rf $(CURL_DIR)/lib/.deps $(CURL_DIR)/lib/.libs $(CURL_DIR)/lib/*.lo $(CURL_DIR)/lib/*.la $(CURL_DIR)/lib/*.o
	find $(CURL_DIR)/lib -type d -name .deps -prune -exec rm -rf {} +
	find $(CURL_DIR) -type d -name .deps -prune -exec rm -rf {} +
	find $(CURL_DIR) -name '*.o' -o -name '*.lo' -o -name '*.la' -o -name '*.Po' -o -name '.dirstamp' | xargs -r rm -f
	rm -f $(CURL_DIR)/Makefile $(CURL_DIR)/config.status $(CURL_DIR)/config.log \
	      $(CURL_DIR)/libtool $(CURL_DIR)/lib/curl_config.h
	find $(CURL_DIR) -name Makefile -not -name 'Makefile.*' -delete

.PHONY: agentbench test clean envcheck-static curl-config-check
