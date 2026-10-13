# sandbox — agentbench sandbox layer.
#
#   sbx        host-side CLI, written in Go (cmd/sbx + internal/); backends
#              use the Docker SDK, the firecracker-go-sdk, netlink and
#              go-iptables instead of shelling out to CLIs.
#   sbx-agent  in-guest HTTP agent, still C++ (src/agent + src/common).
#
# Deps are fetched at build time via GOPROXY (default proxy.golang.org).

GO          ?= go
GOPROXY     ?= https://proxy.golang.org,direct
CXX         ?= g++
CXXFLAGS    ?= -O3 -Wall -Wextra -std=gnu++17
# Statically link libstdc++/libgcc so the binaries run without relying on the
# host's libstdc++ version.
CXXFLAGS    += -static-libstdc++ -static-libgcc
CPPFLAGS    += -Isrc
LDLIBS      += -lpthread -lutil

BUILD_DIR := build
BIN_DIR   := bin

all: sbx sbx-agent

sbx: $(BIN_DIR)/sbx
sbx-agent: $(BIN_DIR)/sbx-agent

$(BIN_DIR)/sbx: FORCE
	@mkdir -p $(BIN_DIR)
	GOPROXY=$(GOPROXY) $(GO) build -o $@ ./cmd/sbx

COMMON_SRCS := \
	src/common/util.cpp \
	src/common/json.cpp \
	src/common/http.cpp
COMMON_OBJS := $(COMMON_SRCS:src/%.cpp=$(BUILD_DIR)/%.o)
AGENT_SRCS := src/agent/main.cpp

$(BUILD_DIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<

$(BIN_DIR)/sbx-agent: $(COMMON_OBJS) $(AGENT_SRCS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(COMMON_OBJS) $(AGENT_SRCS) $(LDLIBS)

.PHONY: FORCE
FORCE:

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

.PHONY: all sbx sbx-agent clean
