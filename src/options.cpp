#include "options.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_THREADS        0
#define DEFAULT_ITERATIONS     5
#define DEFAULT_OUTPUT_FORMAT  FMT_TEXT
#define DEFAULT_PORT           18789
#define DEFAULT_CONCURRENCY    32
#define DEFAULT_REQUESTS       0
#define DEFAULT_DURATION       60
#define DEFAULT_TIMEOUT        120000
#define DEFAULT_SESSIONS       1
#define DEFAULT_TURNS          1

void print_version(void) {
    printf("agentbench %s\n", AGENTBENCH_VERSION);
}

void print_usage(void) {
    printf(
        "Usage:\n"
        "  agentbench --mode=simulation [options]\n"
        "  agentbench --mode=client --host=HOST --port=PORT [options]\n"
        "  agentbench list simulation\n"
        "  agentbench list client\n"
        "\n"
        "Benchmark suite for AI agent workloads\n"
        "\n"
        "General options:\n"
        "  --mode=MODE          run mode: simulation | client\n"
        "  --version            show version and exit\n"
        "  --help               show this help message and exit\n"
        "\n"
        "Simulation mode options:\n"
        "  -b, --benchmarks=LIST  run only the named benchmarks (default: all)\n"
        "  -i, --iterations=N     suite iterations (default: 5); reported elapsed is the\n"
        "                         accumulated total across the N iterations\n"
        "  -t, --threads=N        max threads for concurrent benchmarks (default: CPU count)\n"
        "  -j, --json             output results as JSON\n"
        "  -v, --verbose          show detailed per-iteration results\n"
        "  -s, --sandbox          run the benchmark inside a sandbox (default\n"
        "                         backend: firecracker microVM)\n"
        "  -B, --sandbox-backend  sandbox backend (default firecracker; e.g. docker,\n"
        "                         gvisor, firecracker, or any backend registered in sbx;\n"
        "                         override via SBX_BACKEND)\n"
        "  -n, --sandboxes=N      number of sandboxes to run in parallel (default: 1)\n"
        "      --mems=N           sandbox memory in MiB (default 4096; override via\n"
        "                         SBX_MEMORY_MB; e.g. --mems=2048)\n"
        "      --cpus=N           sandbox vCPUs (default: follows -t/--threads, then\n"
        "                         SBX_CPUS, then 2; e.g. --cpus=4)\n"
        "      --cpuset-cpus=LIST pin the sandbox to a host CPU set (e.g. 0-127\n"
        "                         --cpuset-cpus=0-127; also --cpuset.cpus / _cpus)\n"
        "      --cpuset-mems=LIST pin the sandbox to NUMA memory nodes (e.g. 0)\n"
        "                         --cpuset-mems=0; also --cpuset.mems / _mems)\n"
        "\n"
        "Power measurement options:\n"
        "      --power            measure the power of CPU and server\n"
        "      --bmc_ip=BMC_IP    BMC address; without it ipmitool reads the local\n"
        "                         BMC in band, which needs no credentials\n"
        "      --bmc_user=USER_NAME  BMC account, used with --bmc_ip (default: admin)\n"
        "      --bmc_password=PASSWORD  BMC password, used with --bmc_ip (default: admin)\n"
        "\n"
        "Client mode options:\n"
        "  -h, --host=HOST        Gateway host (required)\n"
        "  -p, --port=PORT        Gateway service port (default: 18789)\n"
        "  -w, --workloads=LIST  run only the named workloads (default: all)\n"
        "  -c, --clients=N        concurrent HTTP connections (default: 32)\n"
        "  -a, --agent-type=TYPE  agent type: openclaw | hermes (default: openclaw)\n"
        "  -A, --agent-ids=LIST comma-separated agent identities (default: main)\n"
        "  -S, --sessions=N       sessions per agent (default: 1)\n"
        "  -t, --turns=N          max conversation turns per session (default: 1)\n"
        "  -d, --duration=N     benchmark duration in seconds (default: 60)\n"
        "  -r, --requests=N     total requests to complete (0 = unlimited, default: 0)\n"
        "  -O, --timeout=N      per-request timeout in ms (default: 120000)\n"
        "  -k, --api-key=KEY      Bearer token for gateway auth\n"
        "  -j, --json             output results as JSON\n"
        "  -v, --verbose          show detailed results\n"
    );
}

static void parse_opt_value(const char *arg, config_t *cfg) {
    if (cfg->num_lua_opts >= MAX_LUA_OPTS) {
        fprintf(stderr, "error: too many Lua options (max %d)\n", MAX_LUA_OPTS);
        exit(1);
    }
    size_t len = strlen(arg) + 1;
    cfg->lua_opts[cfg->num_lua_opts] = (char *)malloc(len);
    memcpy(cfg->lua_opts[cfg->num_lua_opts], arg, len);
    cfg->num_lua_opts++;
}

/* Shared handling for the power/BMC long options (codes 210-213). Returns 1
 * when `opt` was one of them. */
static int handle_power_opt(int opt, char *arg, config_t *cfg) {
    switch (opt) {
    case 210: cfg->power = 1;        return 1;
    case 211: cfg->bmc_ip = arg;     return 1;
    case 212: cfg->bmc_user = arg;   return 1;
    case 213: cfg->bmc_password = arg; return 1;
    default:  return 0;
    }
}

static int parse_int_arg(const char *name, const char *val) {
    if (!val || !*val) {
        fprintf(stderr, "error: --%s requires a numeric argument\n", name);
        exit(1);
    }
    for (const char *p = val; *p; p++) {
        if (*p < '0' || *p > '9') {
            fprintf(stderr, "error: --%s expects a number, got '%s'\n", name, val);
            exit(1);
        }
    }
    return atoi(val);
}

/* Long options for pass 1 (only --mode and --help matter) */
static const struct option pass1_long_opts[] = {
    {"mode",    required_argument, 0, 'm'},
    {"help",    no_argument,       0,  1 },
    {"version", no_argument,       0,  2 },
    {"sandbox",    no_argument,       0, 's'},
    {"sandbox-backend", required_argument, 0, 'B'},
    {"sandboxes",  required_argument, 0, 'n'},
    {"power",      no_argument,       0, 210},
    {"bmc_ip",     required_argument, 0, 211},
    {"bmc_user",   required_argument, 0, 212},
    {"bmc_password", required_argument, 0, 213},
    {0, 0, 0, 0}
};

/* Long options for simulation mode */
static const struct option sim_long_opts[] = {
    {"mode",       required_argument, 0, 'm'},
    {"help",       no_argument,       0,  1 },
    {"version",    no_argument,       0,  2 },
    {"benchmarks", required_argument, 0, 'b'},
    {"iterations", required_argument, 0, 'i'},
    {"threads",    required_argument, 0, 't'},
    {"json",       no_argument,       0, 'j'},
    {"verbose",    no_argument,       0, 'v'},
    {"sandbox",    no_argument,       0, 's'},
    {"sandbox-backend", required_argument, 0, 'B'},
    {"sandboxes",  required_argument, 0, 'n'},
    {"mems",       required_argument, 0, 200},
    {"cpus",       required_argument, 0, 201},
    {"cpuset-cpus", required_argument, 0, 202},
    {"cpuset.mems", required_argument, 0, 203},
    {"cpuset.cpus", required_argument, 0, 202},
    {"cpuset-mems", required_argument, 0, 203},
    {"cpuset_cpus", required_argument, 0, 202},
    {"cpuset_mems", required_argument, 0, 203},
    {"power",      no_argument,       0, 210},
    {"bmc_ip",     required_argument, 0, 211},
    {"bmc_user",   required_argument, 0, 212},
    {"bmc_password", required_argument, 0, 213},
    {0, 0, 0, 0}
};

/* Long options for client mode */
static const struct option client_long_opts[] = {
    {"mode",       required_argument, 0, 'm'},
    {"help",       no_argument,       0,  1 },
    {"version",    no_argument,       0,  2 },
    {"workloads",  required_argument, 0, 'w'},
    {"agent-ids",  required_argument, 0, 'A'},
    {"host",       required_argument, 0, 'h'},
    {"port",       required_argument, 0, 'p'},
    {"duration",   required_argument, 0, 'd'},
    {"timeout",    required_argument, 0, 'O'},
    {"clients",    required_argument, 0, 'c'},
    {"agent-type", required_argument, 0, 'a'},
    {"sessions",   required_argument, 0, 'S'},
    {"turns",      required_argument, 0, 't'},
    {"requests",   required_argument, 0, 'r'},
    {"api-key",    required_argument, 0, 'k'},
    {"json",       no_argument,       0, 'j'},
    {"verbose",    no_argument,       0, 'v'},
    {"sandbox",    no_argument,       0, 's'},
    {"sandbox-backend", required_argument, 0, 'B'},
    {"sandboxes",  required_argument, 0, 'n'},
    {"power",      no_argument,       0, 210},
    {"bmc_ip",     required_argument, 0, 211},
    {"bmc_user",   required_argument, 0, 212},
    {"bmc_password", required_argument, 0, 213},
    {0, 0, 0, 0}
};

void parse_options(int argc, char **argv, config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->threads         = DEFAULT_THREADS;
    cfg->iterations      = DEFAULT_ITERATIONS;
    cfg->output_format   = DEFAULT_OUTPUT_FORMAT;
    cfg->port            = DEFAULT_PORT;
    cfg->concurrency     = DEFAULT_CONCURRENCY;
    cfg->requests        = DEFAULT_REQUESTS;
    cfg->duration        = DEFAULT_DURATION;
    cfg->timeout_ms      = DEFAULT_TIMEOUT;
    cfg->sessions        = DEFAULT_SESSIONS;
    cfg->turns           = DEFAULT_TURNS;
    cfg->sandboxes       = 1;
    cfg->mode            = -1;

    optind = 1;
    opterr = 0;

    /* Pass 1: find --mode and --help, and positional commands */
    int mode_idx = 0;
    while (1) {
        int opti = 0;
        int opt = getopt_long(argc, argv, "+m:bhptSsB:n:", pass1_long_opts, &opti);
        if (opt == -1) break;
        if (opt == 'm') {
            if (strcmp(optarg, "simulation") == 0) cfg->mode = MODE_SIMULATION;
            else if (strcmp(optarg, "client") == 0) cfg->mode = MODE_CLIENT;
            else {
                fprintf(stderr, "error: unknown mode '%s' (use: simulation|client)\n", optarg);
                exit(1);
            }
        }
        if (opt == 's') cfg->sandbox = 1;
        if (opt == 'B') cfg->sandbox_backend = optarg;
        if (opt == 'n') cfg->sandboxes = parse_int_arg("sandboxes", optarg);
        handle_power_opt(opt, optarg, cfg);
        if (opt == 1) { print_usage(); exit(0); }
        if (opt == 2) { print_version(); exit(0); }
    }
    mode_idx = optind;

    /* Check for "list" command */
    if (mode_idx < argc) {
        if (strcmp(argv[mode_idx], "list") == 0) {
            if (mode_idx + 1 < argc) {
                if (strcmp(argv[mode_idx + 1], "client") == 0) {
                    cfg->cmd = CMD_LIST_CLIENT; return;
                }
                if (strcmp(argv[mode_idx + 1], "simulation") == 0) {
                    cfg->cmd = CMD_LIST_SIMULATION; return;
                }
            }
            fprintf(stderr, "error: 'list' requires a type: 'list simulation' or 'list client'\n");
            exit(1);
        }
    }

    if (cfg->mode < 0) {
        fprintf(stderr, "error: --mode is required (simulation|client)\n");
        exit(1);
    }

    /* Pass 2: parse mode-specific options */
    optind = 1;
    opterr = 0;

    if (cfg->mode == MODE_SIMULATION) {
        /* Unrecognized `--key=value` args are passed through to the benchmark
         * Lua scripts (agentbench.opt.<key>). getopt_long returns '?' with
         * optarg=NULL for unknown long options, so they are collected here by
         * scanning argv against the known long-option names. */
        for (int i = 1; i < argc; i++) {
            const char *a = argv[i];
            if (strncmp(a, "--", 2) != 0) continue;
            const char *eq = strchr(a + 2, '=');
            if (!eq || eq == a + 2) continue;
            int known = 0;
            for (int k = 0; sim_long_opts[k].name; k++)
                if (strlen(sim_long_opts[k].name) == (size_t)(eq - (a + 2)) &&
                    strncmp(sim_long_opts[k].name, a + 2, eq - (a + 2)) == 0)
                { known = 1; break; }
            if (!known) parse_opt_value(a + 2, cfg);
        }
        while (1) {
            int opti = 0;
            int opt = getopt_long(argc, argv, "b:i:t:jvsB:n:", sim_long_opts, &opti);
            if (opt == -1) break;
            switch (opt) {
            case 'b': cfg->benchmark_names = optarg; break;
            case 'i': cfg->iterations = parse_int_arg("iterations", optarg); break;
            case 't': cfg->threads = parse_int_arg("threads", optarg); break;
            case 'j': cfg->output_format = FMT_JSON; break;
            case 'v': cfg->verbose = 1; break;
            case 's': cfg->sandbox = 1; break;
            case 'B': cfg->sandbox_backend = optarg; break;
            case 'n': cfg->sandboxes = parse_int_arg("sandboxes", optarg); break;
            case 210: case 211: case 212: case 213:
                handle_power_opt(opt, optarg, cfg); break;
            default: break;
            }
        }
        if (cfg->threads <= 0)
            cfg->threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
        if (cfg->threads < 1) cfg->threads = 1;
        /* Cap at 512 to avoid exhausting system resources on bad input */
        if (cfg->threads > 512) {
            fprintf(stderr, "error: --threads too large (%d), max 512\n", cfg->threads);
            exit(1);
        }
        cfg->cmd = CMD_RUN;

    } else {  /* MODE_CLIENT */
        while (1) {
            int opti = 0;
            int opt = getopt_long(argc, argv, "h:p:w:d:O:A:c:a:S:t:r:k:jvsB:n:", client_long_opts, &opti);
            if (opt == -1) break;
            switch (opt) {
            case 'h': cfg->host = optarg; break;
            case 'p': cfg->port = parse_int_arg("port", optarg); break;
            case 'w': cfg->workload_names = optarg; break;
            case 'd': cfg->duration = parse_int_arg("duration", optarg); break;
            case 'O': cfg->timeout_ms = parse_int_arg("timeout", optarg); break;
            case 'A': cfg->agent_ids = optarg; break;
            case 'c': cfg->concurrency = parse_int_arg("clients", optarg); break;
            case 'a': cfg->agent_type = optarg; break;
            case 'S': cfg->sessions = parse_int_arg("sessions", optarg); break;
            case 't': cfg->turns = parse_int_arg("turns", optarg); break;
            case 'r': cfg->requests = parse_int_arg("requests", optarg); break;
            case 'k': cfg->api_key = optarg; break;
            case 'j': cfg->output_format = FMT_JSON; break;
            case 'v': cfg->verbose = 1; break;
            case 's': cfg->sandbox = 1; break;
            case 'B': cfg->sandbox_backend = optarg; break;
            case 'n': cfg->sandboxes = parse_int_arg("sandboxes", optarg); break;
            case 210: case 211: case 212: case 213:
                handle_power_opt(opt, optarg, cfg); break;
            default: break;
            }
        }
        /* Same pass-through as simulation mode: unrecognized `--key=value`
         * args go to the workload Lua scripts (agentbench.opt.<key>). */
        for (int i = 1; i < argc; i++) {
            const char *a = argv[i];
            if (strncmp(a, "--", 2) != 0) continue;
            const char *eq = strchr(a + 2, '=');
            if (!eq || eq == a + 2) continue;
            int known = 0;
            for (int k = 0; client_long_opts[k].name; k++)
                if (strlen(client_long_opts[k].name) == (size_t)(eq - (a + 2)) &&
                    strncmp(client_long_opts[k].name, a + 2, eq - (a + 2)) == 0)
                { known = 1; break; }
            if (!known) parse_opt_value(a + 2, cfg);
        }
        if (!cfg->host) {
            fprintf(stderr, "error: --host is required in client mode\n");
            exit(1);
        }
        cfg->cmd = CMD_RUN;
    }

    /* --power needs no credentials: by default ipmitool reads the local BMC in
     * band. --bmc_ip switches to out-of-band access, and credentials only make
     * sense together with it. */
    if (cfg->power && !cfg->bmc_ip && (cfg->bmc_user || cfg->bmc_password)) {
        fprintf(stderr, "error: --bmc_user/--bmc_password need --bmc_ip "
                        "(without it ipmitool reads the local BMC in band)\n");
        exit(1);
    }

    /* Remaining positional arg = single benchmark/workload name */
    if (optind < argc) {
        cfg->benchmark_name = strdup(argv[optind]);
    }
}

void config_free(config_t *cfg) {
    if (cfg->benchmark_name) free(cfg->benchmark_name);
    /* benchmark_names / workload_names point into argv, do not free */
    for (int i = 0; i < cfg->num_lua_opts; i++) {
        if (cfg->lua_opts[i]) free(cfg->lua_opts[i]);
    }
}