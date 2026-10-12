#ifndef OPTIONS_H
#define OPTIONS_H

/* Output format */
#define FMT_TEXT 0
#define FMT_JSON 1

/* Mode */
#define MODE_SIMULATION 0
#define MODE_CLIENT     1

/* Commands */
#define CMD_NONE         0
#define CMD_LIST_SIMULATION 1
#define CMD_LIST_CLIENT  2
#define CMD_RUN          3
#define CMD_HELP         4

/* Max CLI option key-value pairs */
#define MAX_LUA_OPTS 64

/* Version */
#define AGENTBENCH_VERSION "v0.2"

typedef struct {
    /* General */
    int    mode;               /* MODE_SIMULATION or MODE_CLIENT */
    int    threads;            /* simulation: worker threads */
    int    iterations;         /* simulation: suite iterations */
    int    output_format;      /* FMT_TEXT or FMT_JSON */
    int    verbose;

    /* Simulation mode */
    char  *benchmark_names;    /* comma-separated, NULL = all */
    int    num_benchmarks;

    /* Client mode */
    int    concurrency;        /* concurrent HTTP connections */
    int    requests;           /* total requests to complete (0 = unlimited) */
    int    duration;           /* benchmark duration in seconds */
    int    timeout_ms;         /* per-request timeout in ms */
    int    sessions;
    int    turns;
    char  *agent_type;
    char  *agent_ids;          /* comma-separated agent identities (x-openclaw-agent-id) */
    char  *host;
    int    port;
    char  *api_key;
    char  *workload_names;     /* comma-separated, NULL = all */

    /* Command */
    int    cmd;
    char  *benchmark_name;     /* single benchmark/workload name (positional) */

    /* Sandbox */
    int    sandbox;            /* run inside a sandbox built from the image */
    char  *sandbox_backend;    /* backend: docker | firecracker (default) */
    int    sandboxes;          /* number of sandboxes to run in parallel (default: 1) */

    /* Power measurement (BMC / IPMI) */
    int    power;              /* measure CPU and server power during the run */
    char  *bmc_ip;             /* BMC address; NULL = local BMC, in band */
    char  *bmc_user;           /* BMC admin username (out-of-band only) */
    char  *bmc_password;       /* BMC admin password (out-of-band only) */

    /* Lua script options (name=value) */
    char  *lua_opts[MAX_LUA_OPTS];
    int    num_lua_opts;
} config_t;

void parse_options(int argc, char **argv, config_t *cfg);
void config_free(config_t *cfg);
void print_usage(void);
void print_version(void);

#endif /* OPTIONS_H */