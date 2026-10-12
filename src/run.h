#ifndef RUN_H
#define RUN_H

#include "options.h"
#include "stats.h"
#include "export.h"

#ifdef __cplusplus
extern "C" {
#endif
#include <lua.h>
#ifdef __cplusplus
}
#endif
#include <pthread.h>
#include <stdint.h>

/* Per-worker context (set by worker thread before starting Lua) */
typedef struct {
    int         thread_id;
    void       *runner;       /* points to sb_runner_t */
    lua_State  *L;
    export_ctx_t export_ctx;
} sb_worker_t;

/* Runner state for one benchmark run */
typedef struct sb_runner {
    config_t   *cfg;
    const char *benchmark_name;
    const char *script_path;

    int         num_workers;    /* number of Lua worker threads (1) */
    int         threads;        /* kernel thread count (cfg->threads) */
    int         max_events;     /* events per worker (1 = single run) */

    /* Synchronization */
    pthread_barrier_t barrier;
    volatile int       keep_running;

    /* Per-thread counters */
    sb_tls_t       *tls;           /* array [num_threads] */
    sb_histogram_t *hist;          /* per-thread histograms [num_threads] */
    pthread_mutex_t hist_lock;     /* unused, kept for compat */

    /* Wall clock */
    uint64_t    wall_start_ns;

    /* Workers */
    sb_worker_t *workers;
    pthread_t   *worker_threads;  /* one per Lua worker (== num_workers) */
} sb_runner_t;

/* Run a benchmark and return aggregated stats.
 * cfg: CLI configuration (used for threads, time, events, etc.)
 * benchmark_name: name of the benchmark
 * script_path: path to the Lua script, or NULL to search lua/benchmarks/
 * Returns the aggregated stats. */
sb_stats_t run_benchmark(config_t *cfg, const char *benchmark_name,
                         const char *script_path);

#endif /* RUN_H */