/**
 * agentbench — AI Agent workload benchmark suite (LuaJIT + C/C++).
 *
 * CLI entry point.  Dispatches to single-benchmark run, suite run, list, or help.
 */

#include "options.h"
#include "run.h"
#include "suite.h"
#include "lua_bridge.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <dirent.h>
#include <curl/curl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <wordexp.h>
#include <ctype.h>
#include <sys/utsname.h>
#include <time.h>
#include <gnu/libc-version.h>

static void print_benchmark_list(void) {
    printf("Available benchmarks:\n");
    for (int i = 0; i < NUM_BENCHMARKS; i++) {
        printf("  %-20s %s\n",
               ALL_WEIGHTS[i].name, ALL_WEIGHTS[i].desc);
    }
}

static void print_single_result(const char *name, sb_stats_t *s, int fmt) {
    if (fmt == FMT_JSON) {
        printf("{\n");
        printf("  \"benchmark\": \"%s\",\n", name);
        printf("  \"total_ops\": %lu,\n", (unsigned long)s->total_ops);
        printf("  \"elapsed_sec\": %.4f,\n", s->wall_sec);
        printf("  \"latency\": {\n");
        printf("    \"avg_ms\": %.3f,\n", s->latency_avg_ns / 1e6);
        printf("    \"min_ms\": %.3f,\n", s->latency_min_ns / 1e6);
        printf("    \"max_ms\": %.3f,\n", s->latency_max_ns / 1e6);
        printf("    \"p95_ms\": %.3f,\n", s->latency_p95_ns / 1e6);
        printf("    \"p99_ms\": %.3f\n", s->latency_p99_ns / 1e6);
        printf("  }\n");
        printf("}\n");
    } else {
        printf("  %s:\n", name);
        printf("    total ops:       %lu\n", (unsigned long)s->total_ops);
        printf("    elapsed:         %.4f sec\n", s->wall_sec);
        printf("    latency (avg):   %.3f ms\n", s->latency_avg_ns / 1e6);
        printf("    latency (p95):   %.3f ms\n", s->latency_p95_ns / 1e6);
        printf("    latency (p99):   %.3f ms\n", s->latency_p99_ns / 1e6);
    }
}

/* ------------------------------------------------------------------ */
/*  System info banner (shown before a run, like the reference         */
/*  report header)                                                     */
/* ------------------------------------------------------------------ */

static void trim_copy(const char *src, char *dst, size_t dstsz) {
    size_t len = strlen(src);
    size_t b = 0;
    while (b < len && isspace((unsigned char)src[b])) b++;
    size_t e = len;
    while (e > b && isspace((unsigned char)src[e - 1])) e--;
    size_t n = (e - b < dstsz - 1) ? (e - b) : dstsz - 1;
    memcpy(dst, src + b, n);
    dst[n] = '\0';
}

/* First line in `f` starting with `key` (key: value) -> value; -1 if absent */
static int lscpu_field(const char *key) {
    FILE *f = popen("lscpu 2>/dev/null", "r");
    if (!f) return -1;
    char line[256];
    int val = -1;
    size_t kl = strlen(key);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, kl) == 0) {
            const char *colon = strchr(line, ':');
            if (colon) val = atoi(colon + 1);
            break;
        }
    }
    pclose(f);
    return val;
}

/* CPU model name: lscpu "Model name" / "BIOS Model name" (some ARM SoCs only
 * expose the model via the BIOS layer), then /proc/cpuinfo "model name". */
static void read_cpu_model(char *out, size_t outsz) {
    snprintf(out, outsz, "unknown");
    const char *lscpu_keys[] = { "BIOS Model name", "Model name" };
    for (unsigned k = 0; k < sizeof lscpu_keys / sizeof lscpu_keys[0]; k++) {
        FILE *f = popen("lscpu 2>/dev/null", "r");
        if (!f) break;
        char line[512];
        size_t kl = strlen(lscpu_keys[k]);
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, lscpu_keys[k], kl) == 0) {
                const char *colon = strchr(line, ':');
                if (colon && colon[1]) {
                    trim_copy(colon + 1, out, outsz);
                    pclose(f);
                    return;
                }
            }
        }
        pclose(f);
    }
    /* /proc/cpuinfo "model name" (x86) */
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "model name", 10) == 0) {
            const char *colon = strchr(line, ':');
            if (colon) {
                trim_copy(colon + 1, out, outsz);
                fclose(f);
                return;
            }
        }
    }
    fclose(f);
}

/* Distinct "physical id" entries in /proc/cpuinfo (x86); -1 if absent */
static int count_physical_ids(void) {
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return -1;
    char line[256];
    int ids[64];
    int nids = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "physical id", 11) == 0) {
            const char *colon = strchr(line, ':');
            if (!colon) continue;
            int id = atoi(colon + 1);
            int seen = 0;
            for (int i = 0; i < nids; i++) {
                if (ids[i] == id) { seen = 1; break; }
            }
            if (!seen && nids < 64) ids[nids++] = id;
        }
    }
    fclose(f);
    return nids > 0 ? nids : -1;
}

/* NUMA node count from /sys/devices/system/node/node*; -1 if unavailable */
static int count_numa_nodes(void) {
    DIR *d = opendir("/sys/devices/system/node");
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "node", 4) == 0 && isdigit((unsigned char)e->d_name[4]))
            n++;
    }
    closedir(d);
    return n > 0 ? n : -1;
}

static void print_system_info(void) {
    char cpu[256];
    read_cpu_model(cpu, sizeof cpu);

    int sockets = lscpu_field("Socket(s)");
    if (sockets < 1) sockets = count_physical_ids();
    if (sockets < 1) sockets = 1;

    int numa = lscpu_field("NUMA node(s)");
    if (numa < 1) numa = count_numa_nodes();
    if (numa < 1) numa = 1;

    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores < 1) cores = 1;

    struct utsname un;
    char platform[256] = "unknown";
    char arch[64] = "unknown";
    if (uname(&un) == 0) {
        snprintf(platform, sizeof platform, "%.31s-%.64s-%.32s-with-glibc%.16s",
                 un.sysname, un.release, un.machine, gnu_get_libc_version());
        snprintf(arch, sizeof arch, "%.32s", un.machine);
    }

    char tbuf[32];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(tbuf, sizeof tbuf, "%Y-%m-%d %H:%M:%S", &tmv);

    printf("===========================================================================\n");
    printf("  AgentBench %s\n", AGENTBENCH_VERSION);
    printf("  Benchmark suit for AI agent workloads\n");
    printf("===========================================================================\n");
    printf("\n");
    printf("  %-13s%s\n", "CPU:", cpu);
    printf("  %-13s%d\n", "Socket:", sockets);
    printf("  %-13s%d\n", "NUMA:", numa);
    printf("  %-13s%ld\n", "Cores:", cores);
    printf("  %-13s%s\n", "Platform:", platform);
    printf("  %-13s%s\n", "Arch:", arch);
    printf("  %-13s%s\n", "Time:", tbuf);
    printf("\n");
    printf("===========================================================================\n");
    printf("\n");
    fflush(stdout);   /* show the banner before any stderr progress output */
}

/* Parse description from workload script header comment (first line format: "-- <name> — <description>") */
static bool workload_description(const char *path, std::string *out) {
    FILE *f = std::fopen(path, "r");
    if (!f) return false;
    char line[512];
    bool ok = false;
    if (std::fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "--", 2) == 0) {
            p += 2;
            while (*p == ' ' || *p == '\t') p++;
            char *dash = strstr(p, "\xe2\x80\x94");  /* UTF-8 em-dash — */
            if (dash) {
                dash += 3;
                while (*dash == ' ' || *dash == '\t') dash++;
                char *nl = strchr(dash, '\n');
                if (nl) *nl = '\0';
                char *cr = strchr(dash, '\r');
                if (cr) *cr = '\0';
                *out = dash;
                ok = !out->empty();
            }
        }
    }
    std::fclose(f);
    return ok;
}

static void print_workload_list(void) {
    const std::string dir = "lua/workloads";
    DIR *d = opendir(dir.c_str());
    if (!d) {
        printf("No workloads found (expected lua/workloads/*.lua)\n");
        return;
    }
    std::vector<std::string> names;
    struct dirent *entry;
    while ((entry = readdir(d)) != nullptr) {
        const char *name = entry->d_name;
        size_t len = strlen(name);
        if (len > 4 && strcmp(name + len - 4, ".lua") == 0)
            names.push_back(std::string(name, len - 4));
    }
    closedir(d);
    std::sort(names.begin(), names.end());

    printf("Available workloads:\n");
    for (auto &n : names) {
        std::string desc;
        if (workload_description((dir + "/" + n + ".lua").c_str(), &desc))
            printf("  %-20s %s\n", n.c_str(), desc.c_str());
        else
            printf("  %s\n", n.c_str());
    }
}

/* Client mode is being refactored: the command surface (--mode=client, -b,
   --host/--port, --agent-type, ...) is kept, the implementation is empty. */
static void run_single_workload(config_t * /*cfg*/, const std::string & /*workload*/,
                                 const std::string & /*host*/, int /*port*/,
                                 const std::string & /*api_key*/) {
    fprintf(stderr, "client mode: implementation removed (pending refactor)\n");
}

/* Run in sandbox: host-side agentbench -s ... uses the built image to launch
   and execute tests inside the sandbox. Passes through remaining arguments
   to the in-sandbox agentbench (stripping -s/--sandbox). */
static std::string shell_quote(const std::string &s) {
    std::string q = "'";
    for (char ch : s) {
        if (ch == '\'') q += "'\\''";
        else q += ch;
    }
    q += "'";
    return q;
}

static std::string find_sbx() {
    const char *env = getenv("SBX");
    if (env && *env) return env;
    if (access("./sbx", X_OK) == 0) return "./sbx";
    if (access("./sandbox/bin/sbx", X_OK) == 0) return "./sandbox/bin/sbx";
    if (access("sandbox/bin/sbx", X_OK) == 0) return "sandbox/bin/sbx";
    return "sbx";  /* fall back to PATH */
}

/* Names of created sandboxes (used for removal by name). on_term iterates and
 * deletes on Ctrl+C/SIGTERM, ensuring firecracker VM / cgroup / numa
 * accounting is reclaimed even on mid-exit. */
static void run_remove(const std::string &name);
static int run_sandbox_cmd(const std::vector<std::string>& argv, int timeoutSec,
                           const std::string& outFile);
static std::vector<std::string> g_sandbox_names;
/* Process-group leaders forked by run_in_sandbox (create/exec/remove phases).
 * Every phase child owns a separate process group, inherited by its timeout /
 * sbx descendants, so Ctrl+C can kill this run's command trees without
 * touching unrelated firecracker, jailer or sbx processes on the host.
 * A raw array keeps signal-handler access independent of STL allocation. */
static constexpr int kMaxTrackedChildren = 16384;
static pid_t g_child_pids[kMaxTrackedChildren];
static int g_child_n = 0;

static void track_child(pid_t pid) {
    if (pid <= 0) return;
    /* The child also calls setpgid(0, 0); doing it in the parent closes the
     * fork-to-child-setup race as far as the kernel permits. */
    setpgid(pid, pid);
    for (int i = 0; i < g_child_n; i++) {
        if (g_child_pids[i] == 0) {
            g_child_pids[i] = pid;
            return;
        }
    }
    if (g_child_n < kMaxTrackedChildren)
        g_child_pids[g_child_n++] = pid;
}

static void untrack_child(pid_t pid) {
    for (int i = 0; i < g_child_n; i++) {
        if (g_child_pids[i] == pid) {
            g_child_pids[i] = 0;
            return;
        }
    }
}

static void prepare_phase_child(void) {
    setpgid(0, 0);
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
}

static void cleanup_sandboxes(void) {
    /* Cleanup after Ctrl+C is scoped by sandbox name. Each backend kills and
     * reclaims only that sandbox's VM/container PID, cgroup, TAP, slot, NUMA
     * entry and state. Sandboxes are independent with protected shared state
     * (numa.json flock, separate slot files, iptables keyed by sandbox port),
     * so parallelize. The old serial implementation with 10s per sandbox took
     * up to 10+ minutes at large -n (e.g. 128), unacceptable for overcommit. */
    size_t n = g_sandbox_names.size();
    if (n == 0) return;
    std::vector<pid_t> pids;
    pids.reserve(n);
    for (size_t i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            run_remove(g_sandbox_names[i]);
            _exit(0);
        }
        if (pid > 0) pids.push_back(pid);
    }
    for (pid_t p : pids)
        waitpid(p, nullptr, 0);
    g_sandbox_names.clear();
}

static void on_term(int sig) {
    (void)sig;
    /* 1) Kill only this run's create/exec/remove process groups. Each group
     *    includes the phase child and its timeout/sbx descendants. The direct
     *    PID kill is a fallback for the narrow race where setpgid did not
     *    complete before the signal arrived. */
    for (int i = 0; i < g_child_n; i++) {
        pid_t pid = g_child_pids[i];
        if (pid <= 0) continue;
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
    }
    /* 2) Non-blocking cleanup: fork a separate session child to best-effort
     *    reclaim sandboxes in the background; main process no longer waitpid's
     *    on sbx remove — avoids hanging when firecracker is abnormal or sbx
     *    hangs, preventing the main process from getting stuck. */
    pid_t cp = fork();
    if (cp == 0) {
        setsid();
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        /* Reclaim only the sandboxes named by this run. Each backend removes
         * its own VM/container PID, cgroup, TAP, NUMA entry, slot and state. */
        cleanup_sandboxes();
        _exit(0);
    }
    /* 3) Main process exits. Do not kill the main process group: this run's
     *    phase groups were already killed in step 1, while a broad kill could
     *    also hit the invoking shell or the just-forked cleanup child before
     *    it completes setsid(), preventing sandbox reclamation. */
    _exit(130);
}

/* Run a sandbox subcommand as an argv array (no shell), with an optional
 * `timeout` guard and optional stdout/stderr redirection. outFile "" inherits
 * the current stdio; otherwise it is opened (or /dev/null) and dup2'd. Returns
 * the child's exit code. Using argv avoids shell metacharacter/quoting bugs
 * that a hand-built `system()` string would carry. */
static int run_sandbox_cmd(const std::vector<std::string>& argv, int timeoutSec,
                           const std::string& outFile) {
    pid_t pid = fork();
    if (pid < 0) return 1;
    if (pid == 0) {
        if (!outFile.empty()) {
            const char* path = (outFile == "/dev/null") ? "/dev/null" : outFile.c_str();
            int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                close(fd);
            }
        }
        std::vector<std::string> full;
        if (timeoutSec > 0) {
            full.push_back("timeout");
            /* Keep timeout and the sbx command in the phase child's process
             * group. Without --foreground GNU timeout creates a separate
             * group, which would escape this run's Ctrl+C cleanup. */
            full.push_back("--foreground");
            full.push_back(std::to_string(timeoutSec));
        }
        full.insert(full.end(), argv.begin(), argv.end());
        std::vector<char*> cargv;
        for (auto& a : full) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        execvp(cargv[0], cargv.data());
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

/* Create a single sandbox. Silent on success; on failure, prints the sbx error
 * output (redirected to a temp log then read back) for easy diagnosis. Returns
 * the exit code. */
static int run_create(const std::string &name, const std::string &backend,
                      const char *img, const char *kernel, int cpus, int mem_mb,
                      const char *cpuset_cpus, const char *cpuset_mems) {
    const std::string sbx = find_sbx();
    const std::string log = "/tmp/sbx-create-" + name + ".log";
    std::vector<std::string> cmd;
    cmd.push_back(sbx);
    cmd.push_back("create");
    cmd.push_back("--backend"); cmd.push_back(backend);
    cmd.push_back("--image"); cmd.push_back(img);
    cmd.push_back("--name"); cmd.push_back(name);
    cmd.push_back("--cpus"); cmd.push_back(std::to_string(cpus));
    cmd.push_back("--memory"); cmd.push_back(std::to_string(mem_mb));
    if (kernel && *kernel) { cmd.push_back("--kernel"); cmd.push_back(kernel); }
    if (cpuset_cpus && *cpuset_cpus) { cmd.push_back("--cpuset-cpus"); cmd.push_back(cpuset_cpus); }
    if (cpuset_mems && *cpuset_mems) { cmd.push_back("--cpuset-mems"); cmd.push_back(cpuset_mems); }
    /* A create does not build anything: the guest rootfs template is built by
       `make rootfs` (`sbx rootfs build`), so this only has to cover host setup
       and the guest's boot - itself bounded by sbx's /health budget
       (SBX_HEALTH_SEC, 60s). Anything slower than this is a hang, and the log
       printed below says where it hung. */
    int rc = run_sandbox_cmd(cmd, 180, log);
    if (rc != 0) {
        fprintf(stderr, "error: failed to create sandbox '%s' (rc=%d):\n",
                name.c_str(), rc);
        FILE *f = fopen(log.c_str(), "r");
        if (f) {
            char buf[512];
            while (fgets(buf, sizeof(buf), f)) fputs(buf, stderr);
            fclose(f);
        }
    }
    /* Keep the log: the caller parses sbx's `timing:` line from it (to split
       provision from startup), then unlinks it. */
    return rc;
}

/* Execute test inside a single sandbox, output redirected to outPath. Returns exec exit code. */
static int run_exec(const std::string &name, const std::string &inner,
                    const std::string &outPath, int timeoutSec) {
    const std::string sbx = find_sbx();
    int fd = open(outPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) return 1;
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    close(fd);
    std::vector<std::string> cmd;
    cmd.push_back(sbx);
    cmd.push_back("exec");
    cmd.push_back(name);
    cmd.push_back("--");
    cmd.push_back("bash");
    cmd.push_back("-c");
    cmd.push_back(inner);
    return run_sandbox_cmd(cmd, timeoutSec, "");
}

/* Remove a single sandbox (silent). */
static void run_remove(const std::string &name) {
    const std::string sbx = find_sbx();
    std::vector<std::string> cmd;
    cmd.push_back(sbx);
    cmd.push_back("remove");
    cmd.push_back(name);
    /* Each remove step is bounded (tap/iptables child, cgroup retry <=4s, slot/
     * numa flock, chroot deletion), so no outer timeout — a timeout truncation
     * would interrupt remove mid-operation (e.g. numa deleted, state not yet),
     * creating leftovers; and remove now reclaims resources first, then deletes
     * state, so re-running remove on leftovers is idempotent. The cleanup
     * process runs in its own setsid session; main does not wait. */
    run_sandbox_cmd(cmd, 0, "/dev/null");
}

/* --- Power measurement (--power) ------------------------------------------
 * Delegates sampling to scripts/power_test.py (default ipmitool sampler, or a
 * custom sampler via AGENTBENCH_POWER_TOOL / the script's own plugins), running
 * it for the whole benchmark window. The sampler is stopped with SIGINT, which
 * makes it print a per-metric min/max/avg summary. */
static const char *find_power_tool(void) {
    const char *e = getenv("AGENTBENCH_POWER_TOOL");
    if (e && *e) return e;
    if (access("./scripts/power_test.py", R_OK) == 0) return "./scripts/power_test.py";
    if (access("scripts/power_test.py", R_OK) == 0) return "scripts/power_test.py";
    return nullptr;
}

/* argv for the sampler tool, shared by the preflight check and the real run.
 * `output` may be empty: the --check path never writes samples. */
static std::vector<std::string> power_tool_args(const config_t *cfg, const char *tool,
                                                const std::string& output) {
    std::vector<std::string> a = {"python3", tool,
        "--duration", "86400", "--interval", "1", "--quiet"};
    /* Only an explicit BMC address switches the sampler to the network; without
       one it reads the local BMC in band, where credentials are not used. */
    if (cfg->bmc_ip)       a.push_back(std::string("--bmc_ip=") + cfg->bmc_ip);
    if (cfg->bmc_user)     a.push_back(std::string("--bmc_user=") + cfg->bmc_user);
    if (cfg->bmc_password) a.push_back(std::string("--bmc_password=") + cfg->bmc_password);
    if (!output.empty()) {
        a.push_back("--output");
        a.push_back(output);
    }
    /* Extra sampler selection, e.g.
       AGENTBENCH_POWER_ARGS='--sampler redfish --plugin /path/r.py' */
    const char *extra = getenv("AGENTBENCH_POWER_ARGS");
    if (extra && *extra) {
        wordexp_t we;
        if (wordexp(extra, &we, WRDE_NOCMD) == 0) {
            for (size_t i = 0; i < we.we_wordc; i++) a.push_back(we.we_wordv[i]);
            wordfree(&we);
        }
    }
    return a;
}

/* Run the tool synchronously and return its exit status. stdout/stderr are
 * inherited, so the reason a sampler is unusable reaches the user right away. */
static int power_run_tool(const std::vector<std::string>& args) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        std::vector<char*> ca;
        for (auto& s : args) ca.push_back(const_cast<char*>(s.c_str()));
        ca.push_back(nullptr);
        execvp("python3", ca.data());
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

/* Preflight for --power: ask the sampler tool whether the configured sampler
 * can run at all (the ipmi sampler checks for ipmitool and reads one sample
 * there, a vendor sampler checks its own tool). Returns 0 when the benchmark may
 * start. Failing here is deliberate: a power run that cannot measure is a
 * misconfiguration, not something to discover after the benchmark finished. */
static int power_preflight(const config_t *cfg) {
    const char *tool = find_power_tool();
    if (!tool) {
        fprintf(stderr, "error: --power requires scripts/power_test.py "
                        "(set AGENTBENCH_POWER_TOOL)\n");
        return 1;
    }
    if (access(tool, R_OK) != 0) {
        fprintf(stderr, "error: --power: power tool not found: %s\n", tool);
        return 1;
    }
    std::vector<std::string> a = power_tool_args(cfg, tool, "");
    a.push_back("--check");
    int rc = power_run_tool(a);
    if (rc != 0)
        fprintf(stderr, "error: --power: sampler check failed (rc=%d), benchmark "
                        "not started; install the required tool or pick another "
                        "sampler via AGENTBENCH_POWER_ARGS\n", rc);
    return rc == 0 ? 0 : 1;
}

static pid_t start_power_measure(const config_t *cfg, std::string& log_path) {
    const char *tool = find_power_tool();
    if (!tool) {
        fprintf(stderr, "warning: --power set but scripts/power_test.py not found "
                        "(set AGENTBENCH_POWER_TOOL)\n");
        return -1;
    }
    char base[128];
    snprintf(base, sizeof(base), "/tmp/agentbench-power-%ld", (long)getpid());
    log_path = std::string(base) + ".log";
    /* The samples are kept in /tmp, under the name the summary prints. */
    char stamp[64];
    time_t started = time(NULL);
    struct tm tm_started;
    localtime_r(&started, &tm_started);
    strftime(stamp, sizeof(stamp), "/tmp/power-%Y-%m-%d-%H.%M.%S.log", &tm_started);
    std::string data_path = stamp;
    pid_t pid = fork();
    if (pid == 0) {
        /* Never outlive agentbench: get SIGINT (which makes the sampler print
           its summary and exit) if the parent dies unexpectedly. */
        prctl(PR_SET_PDEATHSIG, SIGINT);
        int fd = open(log_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }
        std::vector<std::string> a = power_tool_args(cfg, tool, data_path);
        std::vector<char*> ca;
        for (auto& s : a) ca.push_back(const_cast<char*>(s.c_str()));
        ca.push_back(nullptr);
        execvp("python3", ca.data());
        _exit(127);
    }
    return pid;
}

static void stop_power_measure(pid_t pid, const std::string& log_path) {
    if (pid <= 0) return;
    kill(pid, SIGINT);  /* lets the sampler print its summary before exiting */
    int status = 0, reaped = 0;
    for (int i = 0; i < 100 && !reaped; i++) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid || r < 0) reaped = 1;
        else usleep(100000);
    }
    if (!reaped) { kill(pid, SIGKILL); waitpid(pid, &status, 0); }
    FILE *f = fopen(log_path.c_str(), "r");
    if (f) {
        char buf[4096];
        size_t r;
        while ((r = fread(buf, 1, sizeof(buf), f)) > 0) fwrite(buf, 1, r, stdout);
        fclose(f);
    }
    unlink(log_path.c_str());
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Format a phase duration, mapping "not measured" (-1) to n/a. */
static const char *sec_str(double v, char *buf, size_t n) {
    if (v < 0) snprintf(buf, n, "n/a");
    else snprintf(buf, n, "%.4fs", v);
    return buf;
}

/* Read one `<key>=<value>` field of sbx's timing line. The value is read with
   strtod, not atof: the two agree on today's plain decimals, but a parser that
   cannot tell a malformed field from a real one silently turns a measurement
   into a zero. */
static bool parse_timing_field(const std::string& data, const char *key,
                               double& out) {
    size_t p = data.find(key);
    if (p == std::string::npos) return false;
    out = strtod(data.c_str() + p + strlen(key), nullptr);
    return true;
}

/* Parse the machine-readable line
   `timing: startup=<s> provision=<s> host_setup=<s> boot=<s> userspace=<s> ready=<s>`
   that `sbx create` writes to its log. `startup` is the whole bring-up up to
   the guest's own init and is the number to compare with another sandbox's
   startup; the fields after it are the parts it sums, in the order they happen
   (provision = one-time rootfs template build/cache, host_setup = rootfs copy
   / jailer / VMM API, boot = instance start -> init, the guest's own kernel
   boot), followed by the two segments it does not cover: `userspace` = init ->
   agent healthy, and `ready` = the usable latency (host_setup + boot +
   userspace). A part must never be added to the total it belongs to.
   Returns false when absent (e.g. the backend has no timing, or create failed
   early). Fields reported as -1 mean n/a. */
static bool parse_sbx_timing(const std::string& path, double& startup,
                             double& provision, double& host_setup,
                             double& boot, double& userspace, double& ready) {
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return false;
    std::string data;
    char buf[4096];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, r);
    fclose(f);
    bool found = false;
    found |= parse_timing_field(data, "startup=", startup);
    found |= parse_timing_field(data, "provision=", provision);
    found |= parse_timing_field(data, "host_setup=", host_setup);
    found |= parse_timing_field(data, "boot=", boot);
    found |= parse_timing_field(data, "userspace=", userspace);
    found |= parse_timing_field(data, "ready=", ready);
    return found;
}

static double parse_elapsed(const std::string& path) {
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return -1;
    char buf[65536];
    size_t r = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[r] = '\0';
    const char *p = strstr(buf, "\"total_elapsed");
    if (!p) p = strstr(buf, "Total elapsed");
    if (!p) p = strstr(buf, "total_elapsed");
    if (!p) return -1;
    while (*p && !((*p >= '0' && *p <= '9') || *p == '-' || *p == '.')) p++;
    return atof(p);
}

static void parse_bench_elapsed(const std::string& path,
                                std::vector<std::pair<std::string, double>>& out,
                                std::map<std::string, uint64_t>* opsOut = nullptr) {
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return;
    std::string data;
    {
        char buf[65536];
        size_t r;
        while ((r = fread(buf, 1, sizeof(buf), f)) > 0)
            data.append(buf, r);
    }
    fclose(f);
    if (data.empty()) return;

    /* JSON: {"benchmark":"<name>", "elapsed_sec":X, "ops":N} */
    size_t pos = 0;
    const char *tag = "\"benchmark\":\"";
    while ((pos = data.find(tag, pos)) != std::string::npos) {
        size_t begin = pos + strlen(tag);
        size_t end = data.find('"', begin);
        if (end == std::string::npos) break;
        std::string bname = data.substr(begin, end - begin);
        size_t v = data.find("\"elapsed_sec\"", end);
        if (v == std::string::npos) break;
        v = data.find(':', v);
        while (v < data.size() &&
               !((data[v] >= '0' && data[v] <= '9') || data[v] == '-' || data[v] == '.'))
            v++;
        if (v >= data.size()) break;
        out.emplace_back(bname, atof(data.c_str() + v));
        if (opsOut) {
            size_t ov = data.find("\"ops\"", v);
            if (ov != std::string::npos) {
                ov = data.find(':', ov);
                while (ov < data.size() &&
                       !((data[ov] >= '0' && data[ov] <= '9')))
                    ov++;
                if (ov < data.size())
                    (*opsOut)[bname] = strtoull(data.c_str() + ov, nullptr, 10);
            }
        }
        pos = end;
    }
    if (!out.empty()) return;

    /* Human table: parse only rows inside the results table
       (after the "Benchmark" header + first separator, before
       "Total elapsed" / closing separator). */
    bool inTable = false;
    bool afterHeaderSep = false;
    size_t i = 0;
    while (i < data.size()) {
        size_t nl = data.find('\n', i);
        if (nl == std::string::npos) nl = data.size();
        std::string line = data.substr(i, nl - i);
        i = nl + 1;
        if (line.find("Benchmark") != std::string::npos) { inTable = true; continue; }
        if (line.find("Total el") != std::string::npos) break;
        if (line.find("-----") != std::string::npos) {
            if (inTable && afterHeaderSep) break;
            if (inTable) { afterHeaderSep = true; continue; }
            continue;
        }
        if (!inTable || !afterHeaderSep) continue;
        size_t nb = 0;
        while (nb < line.size() && isspace((unsigned char)line[nb])) nb++;
        size_t ne = nb;
        while (ne < line.size() && !isspace((unsigned char)line[ne])) ne++;
        if (nb >= ne) continue;
        std::string bname = line.substr(nb, ne - nb);
        size_t v = ne;
        while (v < line.size() &&
               !((line[v] >= '0' && line[v] <= '9') || line[v] == '-' || line[v] == '.'))
            v++;
        if (v >= line.size()) continue;
        char* endp = nullptr;
        double val = strtod(line.c_str() + v, &endp);
        if (endp == line.c_str() + v) continue;
        out.emplace_back(bname, val);
        if (opsOut) {
            size_t ov = endp - line.c_str();
            while (ov < line.size() && isspace((unsigned char)line[ov])) ov++;
            if (ov < line.size() && (line[ov] >= '0' && line[ov] <= '9'))
                (*opsOut)[bname] = strtoull(line.c_str() + ov, nullptr, 10);
        }
    }
}

static void print_bench_stats(const std::map<std::string, std::vector<double>>& m,
                              const std::map<std::string, uint64_t>& ops) {
    if (m.empty()) return;
    auto quantile = [](const std::vector<double>& v, double q) -> double {
        if (v.size() == 1) return v[0];
        double pos = q * (v.size() - 1);
        size_t lo = (size_t)pos;
        size_t hi = lo + 1;
        if (hi >= v.size()) hi = v.size() - 1;
        double frac = pos - lo;
        return v[lo] + (v[hi] - v[lo]) * frac;
    };
    fprintf(stdout, "\n===== per-benchmark timing statistics (%zu sandboxes) =====\n",
            m.begin()->second.size());
    fprintf(stdout, "  %-24s %10s %10s %10s %10s %10s %10s %14s\n",
            "Benchmark", "min", "max", "avg", "p50", "p90", "p99", "ops");
    for (const auto& kv : m) {
        std::vector<double> v = kv.second;
        std::sort(v.begin(), v.end());
        double sum = 0;
        for (double x : v) sum += x;
        auto opIt = ops.find(kv.first);
        uint64_t opN = opIt != ops.end() ? opIt->second : 0;
        fprintf(stdout, "  %-24s %10.4f %10.4f %10.4f %10.4f %10.4f %10.4f %14llu\n",
                kv.first.c_str(), v.front(), v.back(), sum / v.size(),
                quantile(v, 0.50), quantile(v, 0.90), quantile(v, 0.99),
                (unsigned long long)opN);
    }
}

/* One row of the sandbox lifecycle timing table (create / exec / remove),
   skipping unmeasured (-1) samples. */
static void print_phase_stats(const char *label, const std::vector<double>& raw) {
    std::vector<double> v;
    for (double x : raw) if (x >= 0) v.push_back(x);
    if (v.empty()) return;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) sum += x;
    auto q = [&](double p) -> double {
        if (v.size() == 1) return v[0];
        double pos = p * (v.size() - 1);
        size_t lo = (size_t)pos;
        size_t hi = lo + 1;
        if (hi >= v.size()) hi = v.size() - 1;
        return v[lo] + (v[hi] - v[lo]) * (pos - lo);
    };
    fprintf(stdout, "  %-12s %9.4f %9.4f %9.4f %9.4f %9.4f %9.4f\n",
            label, v.front(), v.back(), sum / v.size(),
            q(0.50), q(0.90), q(0.99));
}

static int run_in_sandbox(config_t *cfg, int argc, char **argv) {
    /* Ctrl+C / SIGTERM: reclaim all created sandboxes, prevent firecracker VM /
     * cgroup leaks. Sub-processes (fork'd create/exec/remove) inherit the handler
     * with no meaning; each resets to default behavior below. */
    signal(SIGINT, on_term);
    signal(SIGTERM, on_term);
    g_sandbox_names.clear();
    g_child_n = 0;

    const std::string sbx = find_sbx();
    const char *backend = cfg->sandbox_backend ? cfg->sandbox_backend : nullptr;
    if (!backend || !*backend) backend = getenv("SBX_BACKEND");
    if (!backend || !*backend) backend = "firecracker";
    const char *img = getenv("SBX_IMAGE");
    if (!img || !*img) img = "agentbench:latest";
    /* Sandbox memory size (MiB). Priority: --mems=N (CLI) > SBX_MEMORY_MB > 4096. */
    const char *mem = getenv("SBX_MEMORY_MB");
    int mem_mb = (mem && *mem) ? atoi(mem) : 4096;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--mems") == 0 && i + 1 < argc) {
            mem_mb = atoi(argv[++i]);
        } else if (strncmp(arg, "--mems=", 7) == 0) {
            mem_mb = atoi(arg + 7);
        }
    }
    if (mem_mb < 128) mem_mb = 4096;  /* too small → treat as invalid, fall back to default */

    const char *kernel = getenv("SBX_KERNEL");
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if ((strcmp(arg, "--kernel") == 0) && i + 1 < argc) kernel = argv[i + 1];
        else if (strncmp(arg, "--kernel=", 9) == 0) kernel = arg + 9;
    }

    int cpus = 0;
    int cpus_cli = 0;   /* explicitly set by --cpus (overrides -t derived) */
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if ((strcmp(arg, "-t") == 0 || strcmp(arg, "--threads") == 0) && i + 1 < argc) {
            cpus = atoi(argv[i + 1]);
        } else if (strncmp(arg, "--threads=", 10) == 0) {
            cpus = atoi(arg + 10);
        } else if ((strcmp(arg, "--cpus") == 0) && i + 1 < argc) {
            cpus_cli = atoi(argv[i + 1]);
        } else if (strncmp(arg, "--cpus=", 7) == 0) {
            cpus_cli = atoi(arg + 7);
        }
    }
    if (cpus_cli > 0) cpus = cpus_cli;   /* --cpus explicitly overrides sandbox vCPU */
    if (cpus < 1) {
        const char *ce = getenv("SBX_CPUS");
        cpus = (ce && *ce) ? atoi(ce) : 2;
    }

    /* Explicit CPU/memory pinning for the sandbox (passed to sbx create):
     * --cpuset-cpus / --cpuset-mems (dash, underscore or dot spelling), or the
     * SBX_CPUSET_CPUS / SBX_CPUSET_MEMS env vars. */
    const char *cpuset_cpus = getenv("SBX_CPUSET_CPUS");
    const char *cpuset_mems = getenv("SBX_CPUSET_MEMS");
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *v = nullptr;
        if (strncmp(arg, "--cpuset.cpus=", 14) == 0) v = arg + 14;
        else if (strncmp(arg, "--cpuset-cpus=", 14) == 0) v = arg + 14;
        else if (strncmp(arg, "--cpuset_cpus=", 14) == 0) v = arg + 14;
        else if (strcmp(arg, "--cpuset.cpus") == 0 || strcmp(arg, "--cpuset-cpus") == 0 ||
                 strcmp(arg, "--cpuset_cpus") == 0) {
            if (i + 1 < argc) v = argv[++i];
        }
        if (v) cpuset_cpus = v;

        v = nullptr;
        if (strncmp(arg, "--cpuset.mems=", 14) == 0) v = arg + 14;
        else if (strncmp(arg, "--cpuset-mems=", 14) == 0) v = arg + 14;
        else if (strncmp(arg, "--cpuset_mems=", 14) == 0) v = arg + 14;
        else if (strcmp(arg, "--cpuset.mems") == 0 || strcmp(arg, "--cpuset-mems") == 0 ||
                 strcmp(arg, "--cpuset_mems") == 0) {
            if (i + 1 < argc) v = argv[++i];
        }
        if (v) cpuset_mems = v;
    }

    char name[80];
    snprintf(name, sizeof(name), "agentbench-%ld", (long)getpid());

    std::string inner = "cd /opt/agentbench && exec agentbench";
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-s" || arg == "--sandbox") continue;
        if (arg == "-B" || arg == "-n" || arg == "--sandbox-backend" || arg == "--sandboxes") { i++; continue; }
        if (arg == "--kernel") { i++; continue; }
        /* --mems is host-side sandbox memory size, not passed into guest */
        if (arg == "--mems") { i++; continue; }
        if (arg.compare(0, 7, "--mems=") == 0) continue;
        if (arg.compare(0, 20, "--sandbox-backend=") == 0) continue;
        if (arg.compare(0, 11, "--sandboxes=") == 0) continue;
        if (arg.compare(0, 9, "--kernel=") == 0) continue;
        /* --cpus / --cpus=N is host-side sandbox vCPU spec, not passed into guest */
        if (arg == "--cpus") { i++; continue; }
        if (arg.compare(0, 7, "--cpus=") == 0) continue;
        /* host-side placement args are for sbx create, not the in-guest run */
        if (arg.compare(0, 9, "--cpuset-") == 0 || arg.compare(0, 9, "--cpuset.") == 0) {
            if (arg.find('=') == std::string::npos) i++;
            continue;
        }
        inner += " " + shell_quote(argv[i]);
    }

    const int n = cfg->sandboxes > 0 ? cfg->sandboxes : 1;
    std::vector<std::string> snames(n);
    std::vector<std::string> outs(n);
    for (int idx = 0; idx < n; idx++) {
        char sname[96];
        snprintf(sname, sizeof(sname), "%s-%d", name, idx);
        snames[idx] = sname;
        outs[idx] = std::string("/tmp/sbx-run-") + name + "-" + std::to_string(idx) + ".log";
    }
    g_sandbox_names = snames;

    /* Per-sandbox phase durations. startup and ready are the two totals, and
       both are shown with the parts they sum, in the order they happen:
         startup = provision + host_setup + boot   (up to the guest's init)
         ready   = host_setup + boot + userspace   (up to a usable sandbox)
       A part must never be added to the total it already belongs to.
       startup/provision/host_setup/boot/userspace/ready come from sbx's own
       timing line; exec (the benchmark run) and remove (teardown) are timed
       here. -1 = not measured. */
    std::vector<double> startupSec(n, -1.0), provisionSec(n, -1.0),
                        hostSetupSec(n, -1.0), bootSec(n, -1.0),
                        userspaceSec(n, -1.0), readySec(n, -1.0),
                        execSec(n, -1.0), removeSec(n, -1.0);

    /* Phase 1: concurrently create all sandboxes, wait until all are ready before phase 2. */
    std::vector<pid_t> cpid(n);
    /* Per-sandbox create outcome. A sandbox that never came up has no exec or
     * remove time to report and must not fail the run as an exec failure, so
     * both its phase samples and its exit code are tracked separately. */
    std::vector<bool> created(n, true);
    std::vector<int> createRc(n, 0);
    double t_create0 = now_sec();
    /* The sandbox path drives an external `sbx` CLI. `make clean` removes it
       (together with the harness and sbx-agent), so after a partial rebuild the
       run would otherwise fail one sandbox at a time with an opaque rc=1: say
       it once, up front. */
    {
        std::string sbxbin = find_sbx();
        bool found = false;
        if (sbxbin.find('/') != std::string::npos) {
            found = access(sbxbin.c_str(), X_OK) == 0;
        } else {
            const char *pathEnv = getenv("PATH");
            std::string list = pathEnv ? pathEnv : "";
            size_t pos = 0;
            while (pos <= list.size()) {
                size_t colon = list.find(':', pos);
                std::string dir = list.substr(pos, colon == std::string::npos ? std::string::npos
                                                                              : colon - pos);
                if (!dir.empty() && access((dir + "/" + sbxbin).c_str(), X_OK) == 0) { found = true; break; }
                if (colon == std::string::npos) break;
                pos = colon + 1;
            }
        }
        if (!found) {
            fprintf(stderr,
                    "error: `%s` not found (looked in ./, ./sandbox/bin and $PATH)\n"
                    "       build the sandbox CLI with `make sbx` (or `make all`) and the guest agent\n"
                    "       with `make agent` - `make clean` removes both\n",
                    sbxbin.c_str());
            return 1;
        }
    }
    int create_left = 0;
    for (int idx = 0; idx < n; idx++) {
        pid_t pid = fork();
        if (pid == 0) {
            prepare_phase_child();
            /* A bring-up can fail for a reason that does not repeat - host
               contention, a guest that did not get scheduled - and that costs
               the whole run: retry it (SBX_CREATE_RETRIES, default 1) before
               giving up. SBX_KEEP_FAILED asks for the failed sandbox instead
               (see sbx), so it disables the retry. */
            int tries = 1;
            if (!getenv("SBX_KEEP_FAILED")) {
                if (const char *e = getenv("SBX_CREATE_RETRIES")) {
                    int v = atoi(e);
                    if (v > 0) tries = v + 1;
                } else {
                    tries = 2;
                }
            }
            int rc = 1;
            for (int attempt = 0; attempt < tries; attempt++) {
                rc = run_create(snames[idx], backend, img, kernel, cpus, mem_mb,
                                cpuset_cpus, cpuset_mems);
                if (rc == 0) break;
                /* rc=124 is the create's own timeout: whatever it is waiting
                   for is still running, and retrying would add a second one
                   rather than help. Report it and move on. */
                if (rc == 124) {
                    fprintf(stderr, "warning: create of %s timed out (rc=124); not retrying\n",
                            snames[idx].c_str());
                    break;
                }
                /* rc=3 is sbx's "this environment is not prepared": the guest
                   rootfs template has not been built for the current image, so
                   the same create fails the same way every time. The fix is in
                   the message (`make rootfs`), not in another attempt - retrying
                   it under -n N is what turned one missing template into a storm
                   of builds. */
                if (rc == 3) {
                    fprintf(stderr, "warning: create of %s cannot run yet (rc=3); not retrying\n",
                            snames[idx].c_str());
                    break;
                }
                if (attempt + 1 < tries)
                    fprintf(stderr, "warning: create of %s failed (rc=%d), retrying\n",
                            snames[idx].c_str(), rc);
            }
            _exit(rc);
        }
        cpid[idx] = pid;
        if (pid > 0) {
            create_left++;
            track_child(pid);
        } else {
            created[idx] = false;
        }
    }
    /* Reap whichever sandbox finishes first so each gets its own create time
       (waiting in index order would charge later sandboxes earlier ones' time). */
    while (create_left > 0) {
        int status = 0;
        pid_t p = waitpid(-1, &status, 0);
        if (p <= 0) break;
        untrack_child(p);
        for (int idx = 0; idx < n; idx++) {
            if (cpid[idx] != p) continue;
            int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            if (rc != 0) {
                created[idx] = false;
                createRc[idx] = rc;
            }
            /* Prefer sbx's own startup time (the whole bring-up up to the
               guest's init, provisioning and host setup included); fall back
               to wall time only for a completed create whose backend reports
               none. */
            double startup = -1, provision = -1, host_setup = -1, boot = -1;
            double userspace = -1, ready = -1;
            std::string clog = "/tmp/sbx-create-" + snames[idx] + ".log";
            if (parse_sbx_timing(clog, startup, provision, host_setup, boot,
                                 userspace, ready)) {
                startupSec[idx] = startup;
                provisionSec[idx] = provision;
                hostSetupSec[idx] = host_setup;
                bootSec[idx] = boot;
                userspaceSec[idx] = userspace;
                readySec[idx] = ready;
            } else if (rc == 0) {
                startupSec[idx] = now_sec() - t_create0;
                readySec[idx] = startupSec[idx];
            }
            ::unlink(clog.c_str());
            cpid[idx] = -1;
            create_left--;
            break;
        }
    }

    /* Phase 2: once all sandboxes are ready, dispatch exec to all concurrently. */
    /* Timeout must cover full suite duration: it scales linearly with
     * iterations. The full 13-benchmark suite measures ~30s/iteration in a
     * 2-vCPU guest (and more when several sandboxes run concurrently on one
     * host), so budget 60s/iteration (~2x single-sandbox cost) on top of a
     * 120s base that covers sandbox/agent startup. A fixed timeout that is too
     * tight (e.g. the old 20s/iteration factor) kills legitimate long runs with
     * `timeout` → the guest never prints its summary → "no timing data".
     *
     * `-n` is not a capacity check: it only sets each sandbox's cpu.max /
     * memory.max, so a large -n on a small host oversubscribes CPU and can ask
     * for more RAM than the host has, and the suite then needs far more than
     * this formula allows. SBX_EXEC_TIMEOUT=<seconds> replaces it (the budget is
     * per sandbox, so it has to cover one guest's suite, not the whole run). */
    int exec_timeout = 120 + cfg->iterations * 60;
    if (const char *e = getenv("SBX_EXEC_TIMEOUT")) {
        int v = atoi(e);
        if (v > 0) exec_timeout = v;
    }
    std::vector<pid_t> epid(n);
    std::vector<int> execRc(n, 0);
    double t_exec0 = now_sec();
    int exec_left = 0;
    for (int idx = 0; idx < n; idx++) {
        /* Nothing to exec into: the sandbox never came up. Exec'ing it anyway
         * only buys a fast rc=1 and a bogus ~0.03s sample in the exec row of
         * the lifecycle table, which is a benchmark's own distribution. */
        if (!created[idx]) {
            epid[idx] = -1;
            continue;
        }
        pid_t pid = fork();
        if (pid == 0) {
            prepare_phase_child();
            _exit(run_exec(snames[idx], inner, outs[idx], exec_timeout));
        }
        epid[idx] = pid;
        if (pid > 0) {
            exec_left++;
            track_child(pid);
        }
    }
    while (exec_left > 0) {
        int status = 0;
        pid_t p = waitpid(-1, &status, 0);
        if (p <= 0) break;
        untrack_child(p);
        for (int idx = 0; idx < n; idx++) {
            if (epid[idx] != p) continue;
            execSec[idx] = now_sec() - t_exec0;
            execRc[idx] = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            epid[idx] = -1;
            exec_left--;
            break;
        }
    }

    /* Phase 3: concurrently destroy all sandboxes. */
    std::vector<pid_t> rpid(n);
    double t_remove0 = now_sec();
    int remove_left = 0;
    for (int idx = 0; idx < n; idx++) {
        pid_t pid = fork();
        if (pid == 0) {
            prepare_phase_child();
            run_remove(snames[idx]);
            _exit(0);
        }
        rpid[idx] = pid;
        if (pid > 0) {
            remove_left++;
            track_child(pid);
        }
    }
    while (remove_left > 0) {
        int status = 0;
        pid_t p = waitpid(-1, &status, 0);
        if (p <= 0) break;
        untrack_child(p);
        for (int idx = 0; idx < n; idx++) {
            if (rpid[idx] != p) continue;
            /* The remove still runs for every sandbox - it is the safety net
               for SBX_KEEP_FAILED and for a create killed outside its own
               teardown - but one that never came up contributes no remove
               time, only the no-op cost of a remove that finds nothing. */
            removeSec[idx] = created[idx] ? now_sec() - t_remove0 : -1.0;
            rpid[idx] = -1;
            remove_left--;
            break;
        }
    }

    int result = 0;
    std::vector<double> elapsed;
    std::vector<double> allElapsed;
    std::map<std::string, std::vector<double>> benchElapsed;
    std::map<std::string, uint64_t> benchOps;
    for (int idx = 0; idx < n; idx++) {
        /* A sandbox that never came up still fails the run - a runner must not
           read a lost sandbox as a fast one - but it is reported as a create
           failure, not passed off as one more exec failure. */
        if (!created[idx] && result == 0) result = createRc[idx] > 0 ? createRc[idx] : 1;
        else if (execRc[idx] != 0 && result == 0) result = execRc[idx];
        double t = parse_elapsed(outs[idx]);
        allElapsed.push_back(t);
        if (t >= 0) elapsed.push_back(t);
        std::vector<std::pair<std::string, double>> be;
        parse_bench_elapsed(outs[idx], be, &benchOps);
        for (const auto& kv : be) benchElapsed[kv.first].push_back(kv.second);
    }
    if (cfg->verbose) {
        for (int idx = 0; idx < n; idx++) {
            char hdr[160];
            snprintf(hdr, sizeof(hdr), "\n===== sandbox %s-%d =====", name, idx);
            fprintf(stdout, "%s\n", hdr);
            FILE *f = fopen(outs[idx].c_str(), "r");
            if (f) {
                char buf[4096];
                size_t r;
                while ((r = fread(buf, 1, sizeof(buf), f)) > 0)
                    fwrite(buf, 1, r, stdout);
                fclose(f);
            }
            unlink(outs[idx].c_str());
        }
    } else {
        for (int idx = 0; idx < n; idx++) {
            /* The lifecycle table's own six fields, in its order and always all
               six (measured, or "n/a"): a field that is only sometimes printed
               cannot be lined up against that table, which is the point of
               printing the breakdown here at all. */
            char sb[16], bb[16], ub[16], db[16], eb[16], rb[16];
            sec_str(startupSec[idx], sb, sizeof sb);
            sec_str(bootSec[idx], bb, sizeof bb);
            sec_str(userspaceSec[idx], ub, sizeof ub);
            sec_str(readySec[idx], db, sizeof db);
            sec_str(execSec[idx], eb, sizeof eb);
            sec_str(removeSec[idx], rb, sizeof rb);
            /* Bare "no timing data" hides two very different causes: the guest
               was killed by the exec budget (timeout -> 124), or it ran and
               printed no summary (its own failure). A killed run is a sizing
               problem and must not be mistaken for a benchmark result. */
            std::string why;
            if (allElapsed[idx] < 0) {
                why = "  (no timing data";
                if (!created[idx])
                    why += ": create failed, rc=" + std::to_string(createRc[idx]);
                else if (execRc[idx] == 124)
                    why += ": exec timed out after " + std::to_string(exec_timeout) +
                           "s: raise SBX_EXEC_TIMEOUT, or use fewer/smaller sandboxes";
                else if (execRc[idx] != 0)
                    why += ": exec failed, rc=" + std::to_string(execRc[idx]);
                else
                    why += ": the guest printed no summary";
                why += ")";
            }
            fprintf(stdout,
                    "sandbox %s-%d: startup %s  boot %s  userspace %s  ready %s  "
                    "exec %s  remove %s%s\n",
                    name, idx, sb, bb, ub, db, eb, rb, why.c_str());
        }
        for (int idx = 0; idx < n; idx++) unlink(outs[idx].c_str());
    }
    int timed_out = 0;
    for (int idx = 0; idx < n; idx++)
        if (execRc[idx] == 124) timed_out++;
    if (timed_out > 0) {
        /* stdout is block-buffered when redirected to a file while stderr is
           not, so a warning printed here would land in the middle of a sandbox
           line. Flush first so it starts on its own. */
        fflush(stdout);
        fprintf(stderr,
                "warning: %d of %d sandboxes were killed by the %ds exec budget "
                "(SBX_EXEC_TIMEOUT=<seconds> raises it for the whole run; -n only sets "
                "each sandbox's cpu/memory limits and does not check the host has that "
                "much, so fewer or smaller sandboxes may be the real fix)\n",
                timed_out, n, exec_timeout);
    }

    if (!elapsed.empty()) {
        std::sort(elapsed.begin(), elapsed.end());
        double sum = 0;
        for (double v : elapsed) sum += v;
        auto quantile = [&](double q) -> double {
            if (elapsed.size() == 1) return elapsed[0];
            double pos = q * (elapsed.size() - 1);
            size_t lo = (size_t)pos;
            size_t hi = lo + 1;
            if (hi >= elapsed.size()) hi = elapsed.size() - 1;
            double frac = pos - lo;
            return elapsed[lo] + (elapsed[hi] - elapsed[lo]) * frac;
        };
        fprintf(stdout,
                "\n===== sandbox timing statistics (%zu sandboxes) =====\n"
                "  min:    %.4f s\n"
                "  max:    %.4f s\n"
                "  avg:    %.4f s\n"
                "  p50:    %.4f s\n"
                "  p90:    %.4f s\n"
                "  p99:    %.4f s\n",
                elapsed.size(), elapsed.front(), elapsed.back(),
                sum / elapsed.size(), quantile(0.50), quantile(0.90),
                quantile(0.99));
    }
    bool have_lifecycle = false;
    int measured = 0;
    for (int idx = 0; idx < n; idx++) {
        if (created[idx]) measured++;
        if (startupSec[idx] >= 0 || provisionSec[idx] >= 0 ||
            hostSetupSec[idx] >= 0 || bootSec[idx] >= 0 ||
            userspaceSec[idx] >= 0 || readySec[idx] >= 0 ||
            execSec[idx] >= 0 || removeSec[idx] >= 0)
            have_lifecycle = true;
    }
    if (have_lifecycle) {
        /* The count is the sandboxes the table can describe, not the ones
           requested: a create failure gets its own line above and must not
           inflate the row count the phases are read against. The rows are the
           same fields as the per-sandbox lines above, in the same order. */
        fprintf(stdout,
                "\n===== sandbox lifecycle timing statistics (%d sandboxes) =====\n"
                "  %-12s %9s %9s %9s %9s %9s %9s\n",
                measured, "phase", "min", "max", "avg", "p50", "p90", "p99");
        print_phase_stats("startup", startupSec);
        /* The guest's own boot (Firecracker's Guest-boot-time where the
           backend reports one), so the bare kernel boot is visible next to
           the startup total that also carries the host-side setup. */
        print_phase_stats("boot", bootSec);
        print_phase_stats("userspace", userspaceSec);
        print_phase_stats("ready", readySec);
        print_phase_stats("exec", execSec);
        print_phase_stats("remove", removeSec);
    }
    print_bench_stats(benchElapsed, benchOps);
    return result;
}

/* Client mode is being refactored: the command surface is kept, the
   implementation is empty. */
static void run_workload_cli(config_t *cfg) {
    std::string workload = cfg->benchmark_name ? cfg->benchmark_name :
                          (cfg->workload_names ? cfg->workload_names : "");
    run_single_workload(cfg, workload,
                        cfg->host ? cfg->host : "127.0.0.1",
                        cfg->port > 0 ? cfg->port : 18789,
                        cfg->api_key ? cfg->api_key : "");
}

int main(int argc, char **argv) {
    config_t cfg;
    parse_options(argc, argv, &cfg);

    /* Start power sampling (if requested) before the run and stop it after, so
       the summary covers the whole benchmark window. */
    pid_t power_pid = -1;
    std::string power_log;
    if (cfg.power && cfg.cmd == CMD_RUN) {
        /* Check the sampler before the benchmark starts (e.g. "is ipmitool
           installed?") so a misconfiguration fails fast instead of after the
           run. */
        if (power_preflight(&cfg) != 0) {
            config_free(&cfg);
            return 1;
        }
        power_pid = start_power_measure(&cfg, power_log);
    }

    if (cfg.sandbox) {
        /* Guard against nesting: agentbench running inside a sandbox with -s
           would try to create another sandbox. Detect via SBX_AGENT_TOKEN
           (always present in the agent environment) and reject. */
        const char *inSb = getenv("SBX_AGENT_TOKEN");
        if (inSb && *inSb) {
            fprintf(stderr, "error: -s/--sandbox is not allowed inside a sandbox (nested sandbox)\n");
            stop_power_measure(power_pid, power_log);
            config_free(&cfg);
            return 1;
        }
        int rc = run_in_sandbox(&cfg, argc, argv);
        stop_power_measure(power_pid, power_log);
        config_free(&cfg);
        curl_global_cleanup();
        return rc;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    switch (cfg.cmd) {
    case CMD_LIST_SIMULATION:
        print_benchmark_list();
        break;

    case CMD_LIST_CLIENT:
        print_workload_list();
        break;

    case CMD_HELP:
        print_usage();
        break;

    case CMD_RUN:
        if (cfg.mode == MODE_SIMULATION) {
            if (cfg.benchmark_name) {
                /* Validate name before running */
                int found = 0;
                for (int j = 0; j < NUM_BENCHMARKS; j++) {
                    if (strcmp(cfg.benchmark_name, ALL_WEIGHTS[j].name) == 0) {
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    fprintf(stderr, "error: unknown benchmark '%s'\n", cfg.benchmark_name);
                    break;
                }
                sb_stats_t stats = run_benchmark(&cfg, cfg.benchmark_name, NULL);
                if (cfg.output_format != FMT_JSON)
                    print_system_info();
                print_single_result(cfg.benchmark_name, &stats, cfg.output_format);
            } else {
                suite_result_t sr = run_suite(&cfg);
                if (cfg.output_format != FMT_JSON)
                    print_system_info();
                suite_result_print(&sr, cfg.output_format);
                suite_result_free(&sr);
            }
        } else {
            run_workload_cli(&cfg);
        }
        break;

    default:
        print_usage();
        return 1;
    }

    stop_power_measure(power_pid, power_log);
    config_free(&cfg);
    curl_global_cleanup();
    return 0;
}
