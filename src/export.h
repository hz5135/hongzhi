#ifndef EXPORT_H
#define EXPORT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Functions exported to LuaJIT via ffi.C (linked with -Wl,--export-dynamic).
 *
 * These are called from Lua's thread_run loop to interact with the C
 * framework: timing, event counting, RNG, etc. */

/* Check whether the current run should continue producing events. */
__attribute__((visibility("default")))
int sb_more_events(int thread_id);

/* Start/stop per-event timer. */
__attribute__((visibility("default")))
void sb_event_start(int thread_id);

__attribute__((visibility("default")))
void sb_event_stop(int thread_id);

/* Increment the ops counter for this thread. */
__attribute__((visibility("default")))
void sb_add_ops(int thread_id, uint64_t n);

/* Thread-local storage for export functions — set by the runner. */
typedef struct {
    volatile int   *keep_running;   /* points to runner->keep_running */
    void           *tls;            /* points to sb_tls_t for this thread */
    void           *hist;           /* points to per-thread sb_histogram_t */
    uint64_t        max_events;     /* 0 = unlimited */
    uint64_t        last_start_ns;  /* last event start timestamp */
} export_ctx_t;

void export_set_ctx(int thread_id, export_ctx_t *ctx);

/* Initialize per-thread counters (called by worker start). */
void export_setup(int thread_id);

#ifdef __cplusplus
}
#endif

#endif /* EXPORT_H */