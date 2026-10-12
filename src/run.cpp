#include "run.h"
#include "lua_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Worker thread entry point                                          */
/* ------------------------------------------------------------------ */

static void *worker_thread(void *arg) {
    sb_worker_t *w = (sb_worker_t *)arg;
    sb_runner_t *r = (sb_runner_t *)w->runner;
    int tid = w->thread_id;

    /* Initialize Lua state */
    w->L = lua_bridge_thread_init(NULL, r->script_path);
    if (!w->L) {
        fprintf(stderr, "error: worker %d failed to init Lua state\n", tid);
        return (void *)(intptr_t)-1;
    }

    /* Set up export context */
    export_ctx_t *ctx = &w->export_ctx;
    ctx->keep_running = &r->keep_running;
    ctx->tls = &r->tls[tid];
    ctx->hist = &r->hist[tid];
    ctx->max_events = r->max_events > 0 ? (uint64_t)r->max_events : 0;
    export_set_ctx(tid, ctx);
    export_setup(tid);

    /* Pass the kernel thread count to the Lua script (sysbench.opt.threads) */
    char tbuf[32];
    snprintf(tbuf, sizeof(tbuf), "%d", r->threads);
    lua_bridge_set_opt(w->L, "threads", tbuf);

    /* Apply CLI-provided Lua options */
    if (r->cfg && r->cfg->num_lua_opts > 0) {
        lua_bridge_apply_opts(w->L, r->cfg->lua_opts, r->cfg->num_lua_opts);
    }

    /* Call thread_init */
    if (lua_bridge_call_thread_init(w->L, tid) != 0) {
        lua_bridge_thread_close(w->L);
        w->L = NULL;
        return (void *)(intptr_t)-1;
    }

    /* Wait for all workers to be ready */
    pthread_barrier_wait(&r->barrier);

    /* Run the event loop */
    int rc = lua_bridge_thread_run(w->L, tid);

    /* Call thread_done */
    lua_bridge_call_thread_done(w->L, tid);

    /* Close Lua state */
    lua_bridge_thread_close(w->L);
    w->L = NULL;

    return (void *)(intptr_t)rc;
}

/* ------------------------------------------------------------------ */
/*  Find benchmark Lua script                                           */
/* ------------------------------------------------------------------ */

/* Reentrant variant: caller provides the output buffer. */
static int find_script_r(const char *benchmark_name, char *out, size_t outsz) {
    const char *dirs[] = {
        "lua/benchmarks",
        "/usr/share/agentbench/lua/benchmarks",
        NULL
    };
    for (int i = 0; dirs[i]; i++) {
        snprintf(out, outsz, "%s/%s.lua", dirs[i], benchmark_name);
        FILE *f = fopen(out, "r");
        if (f) { fclose(f); return 1; }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

/* Per-thread counters are declared cache-line aligned (stats.h), so they
   cannot come from calloc: the compiler is then entitled to access them at an
   address it assumes is 64-byte aligned, and the padding that keeps two
   counters off one cache line would not hold either.  posix_memalign supplies
   the alignment; the zeroing calloc used to give is kept, because the
   aggregation reads a zero latency_min as "this thread recorded nothing". */
static sb_tls_t *tls_alloc(int n) {
    void *mem = NULL;
    if (posix_memalign(&mem, CACHE_LINE, (size_t)n * sizeof(sb_tls_t)) != 0)
        return NULL;
    memset(mem, 0, (size_t)n * sizeof(sb_tls_t));
    return (sb_tls_t *)mem;
}

sb_stats_t run_benchmark(config_t *cfg, const char *benchmark_name,
                          const char *script_path) {
    sb_stats_t empty;
    memset(&empty, 0, sizeof(empty));

    /* Find script */
    char resolved[512];
    const char *script_path_local = NULL;
    if (!script_path) {
        if (find_script_r(benchmark_name, resolved, sizeof(resolved))) {
            script_path_local = resolved;
        } else {
            fprintf(stderr, "error: benchmark script not found for '%s'\n",
                    benchmark_name);
            return empty;
        }
        script_path = script_path_local;
    }

    /* Set up runner */
    sb_runner_t runner;
    memset(&runner, 0, sizeof(runner));
    runner.cfg = cfg;
    runner.benchmark_name = benchmark_name;
    runner.script_path = script_path;
    runner.threads = cfg->threads > 0 ? cfg->threads : (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (runner.threads < 1) runner.threads = 1;
    runner.num_workers = 1;       /* 1 Lua worker; kernel internally uses runner.threads */
    runner.max_events = 1;        /* run 1 event per worker, measure elapsed time */
    runner.keep_running = 1;

    if (cfg->verbose) {
        printf("  [%s] threads=%d\n", benchmark_name, runner.threads);
    }

    /* Init barrier */
    if (pthread_barrier_init(&runner.barrier, NULL, runner.num_workers + 1) != 0) {
        fprintf(stderr, "error: barrier init failed for '%s'\n", benchmark_name);
        return empty;
    }

    /* Init hist lock */
    if (pthread_mutex_init(&runner.hist_lock, NULL) != 0) {
        pthread_barrier_destroy(&runner.barrier);
        fprintf(stderr, "error: mutex init failed for '%s'\n", benchmark_name);
        return empty;
    }

    /* Allocate per-thread counters and histograms */
    runner.tls = tls_alloc(runner.num_workers);
    runner.hist = (sb_histogram_t *)calloc(runner.num_workers, sizeof(sb_histogram_t));
    runner.workers = (sb_worker_t *)calloc(runner.num_workers, sizeof(sb_worker_t));
    runner.worker_threads = (pthread_t *)calloc(runner.num_workers, sizeof(pthread_t));
    if (!runner.tls || !runner.hist || !runner.workers || !runner.worker_threads) {
        fprintf(stderr, "error: out of memory for '%s'\n", benchmark_name);
        free(runner.tls);
        free(runner.hist);
        free(runner.workers);
        free(runner.worker_threads);
        pthread_barrier_destroy(&runner.barrier);
        pthread_mutex_destroy(&runner.hist_lock);
        return empty;
    }
    for (int i = 0; i < runner.num_workers; i++) {
        hist_init(&runner.hist[i]);
    }

    /* Create workers */
    for (int i = 0; i < runner.num_workers; i++) {
        runner.workers[i].thread_id = i;
        runner.workers[i].runner = &runner;
    }

    /* Start worker thread */
    int worker_error = 0;
    for (int i = 0; i < runner.num_workers; i++) {
        if (pthread_create(&runner.worker_threads[i], NULL, worker_thread,
                           &runner.workers[i]) != 0) {
            fprintf(stderr, "error: failed to create worker %d for '%s'\n",
                    i, benchmark_name);
            worker_error = 1;
            /* Mark this worker as failed; the barrier wait below would
             * hang, so skip barrier participation for unstarted workers. */
            runner.worker_threads[i] = 0;
            continue;
        }
    }
    if (worker_error) {
        /* Worker creation failed — the barrier can never be satisfied.
         * Cancel started workers and bail out. */
        fprintf(stderr, "error: aborting benchmark '%s'\n", benchmark_name);
        /* Detach started workers so they don't block; they will hit the
         * barrier and hang, so cancel them instead. */
        for (int i = 0; i < runner.num_workers; i++) {
            if (runner.worker_threads[i]) {
                pthread_cancel(runner.worker_threads[i]);
                pthread_join(runner.worker_threads[i], NULL);
            }
        }
        pthread_barrier_destroy(&runner.barrier);
        pthread_mutex_destroy(&runner.hist_lock);
        free(runner.tls);
        free(runner.hist);
        free(runner.workers);
        free(runner.worker_threads);
        return empty;
    }

    /* Wait on barrier with workers — all start together, timing excludes startup */
    pthread_barrier_wait(&runner.barrier);

    /* Record wall start — after barrier, all workers are ready to run */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    runner.wall_start_ns = (uint64_t)ts.tv_sec * 1000000000ULL
                         + (uint64_t)ts.tv_nsec;
    for (int i = 0; i < runner.num_workers; i++) {
        void *ret;
        pthread_join(runner.worker_threads[i], &ret);
        if ((intptr_t)ret != 0) worker_error = 1;
    }
    if (worker_error) {
        /* Always warn — a silent 0-elapsed result would pollute the suite */
        fprintf(stderr, "warning: worker(s) returned errors for '%s'\n",
                benchmark_name);
    }

    /* Record wall time — actual execution time */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t end_ns = (uint64_t)ts.tv_sec * 1000000000ULL
                    + (uint64_t)ts.tv_nsec;
    double wall_sec = (double)(end_ns - runner.wall_start_ns) / 1e9;

    /* Aggregate stats */
    sb_histogram_t merged_hist;
    hist_init(&merged_hist);
    for (int i = 0; i < runner.num_workers; i++) {
        for (int bucket = 0; bucket < HIST_BUCKETS; bucket++) {
            merged_hist.buckets[bucket] += runner.hist[i].buckets[bucket];
        }
        merged_hist.count += runner.hist[i].count;
        if (runner.hist[i].min_ns < merged_hist.min_ns)
            merged_hist.min_ns = runner.hist[i].min_ns;
        if (runner.hist[i].max_ns > merged_hist.max_ns)
            merged_hist.max_ns = runner.hist[i].max_ns;
    }
    sb_stats_t stats = stats_aggregate(runner.tls, &merged_hist,
                                       runner.num_workers, wall_sec);

    /* Cleanup */
    pthread_barrier_destroy(&runner.barrier);
    pthread_mutex_destroy(&runner.hist_lock);
    free(runner.tls);
    free(runner.hist);
    free(runner.workers);
    free(runner.worker_threads);

    return stats;
}