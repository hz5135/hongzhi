#include "export.h"
#include "stats.h"   /* for sb_tls_t, sb_histogram_t, hist_record */

#include <stdio.h>
#include <time.h>

/* Thread-local storage for export functions.
 * The runner sets this up before each worker thread starts.
 *
 * This must stay in sync with the worker count used by the runner.
 * Currently the simulation runner uses exactly 1 Lua worker thread
 * (thread_id 0); the C kernels spawn their own internal threads which
 * do not call these functions.  Keep MAX_WORKERS generous to allow
 * multi-worker extensions without silent OOB writes. */
#define MAX_WORKERS 64

static export_ctx_t g_ctx[MAX_WORKERS];

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void export_set_ctx(int thread_id, export_ctx_t *ctx) {
    if (thread_id < 0 || thread_id >= MAX_WORKERS) {
        fprintf(stderr, "error: worker thread id %d out of range (max %d)\n",
                thread_id, MAX_WORKERS - 1);
        return;
    }
    g_ctx[thread_id] = *ctx;
}

void export_setup(int thread_id) {
    if (thread_id < 0 || thread_id >= MAX_WORKERS) return;
    if (g_ctx[thread_id].tls) {
        sb_tls_t *tls = (sb_tls_t *)g_ctx[thread_id].tls;
        tls->events = 0;
        tls->ops = 0;
        tls->latency_sum = 0;
        tls->latency_min = 1e18;
        tls->latency_max = 0;
    }
}

int sb_more_events(int thread_id) {
    if (thread_id < 0 || thread_id >= MAX_WORKERS) return 0;
    export_ctx_t *ctx = &g_ctx[thread_id];

    if (ctx->keep_running && !*ctx->keep_running) return 0;

    if (ctx->max_events > 0 && ctx->tls) {
        sb_tls_t *tls = (sb_tls_t *)ctx->tls;
        if (tls->events >= ctx->max_events) return 0;
    }
    return 1;
}

void sb_event_start(int thread_id) {
    if (thread_id < 0 || thread_id >= MAX_WORKERS) return;
    g_ctx[thread_id].last_start_ns = now_ns();
}

void sb_event_stop(int thread_id) {
    if (thread_id < 0 || thread_id >= MAX_WORKERS) return;
    export_ctx_t *ctx = &g_ctx[thread_id];
    if (!ctx->tls) return;

    uint64_t end = now_ns();
    double elapsed = (double)(end - ctx->last_start_ns);

    sb_tls_t *tls = (sb_tls_t *)ctx->tls;
    tls->events++;
    tls->latency_sum += elapsed;
    if (elapsed < tls->latency_min) tls->latency_min = elapsed;
    if (elapsed > tls->latency_max) tls->latency_max = elapsed;

    /* Record in per-thread histogram (no locking needed) */
    if (ctx->hist) {
        hist_record((sb_histogram_t *)ctx->hist, elapsed);
    }
}

void sb_add_ops(int thread_id, uint64_t n) {
    if (thread_id >= 0 && thread_id < MAX_WORKERS && g_ctx[thread_id].tls) {
        ((sb_tls_t *)g_ctx[thread_id].tls)->ops += n;
    }
}