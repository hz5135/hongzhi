#ifndef STATS_H
#define STATS_H

#include <features.h>
#include <stdint.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/*  Per-thread counters (cache-line aligned)                           */
/* ------------------------------------------------------------------ */

#define CACHE_LINE 64

typedef struct {
    uint64_t events;
    uint64_t ops;
    double   latency_sum;       /* sum of event latencies (ns) */
    double   latency_min;
    double   latency_max;
    char     _pad[CACHE_LINE - 5 * 8];  /* 40 bytes used, pad to cache line */
} __attribute__((aligned(CACHE_LINE))) sb_tls_t;

/* ------------------------------------------------------------------ */
/*  Simple latency histogram (log-linear buckets)                      */
/* ------------------------------------------------------------------ */

#define HIST_BUCKETS 256

typedef struct {
    uint64_t buckets[HIST_BUCKETS];
    uint64_t count;
    double   min_ns;
    double   max_ns;
} sb_histogram_t;

void  hist_init(sb_histogram_t *h);
void  hist_record(sb_histogram_t *h, double ns);
double hist_percentile(sb_histogram_t *h, int pct);

/* ------------------------------------------------------------------ */
/*  Runner stats (aggregated)                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t total_events;
    uint64_t total_ops;
    double   wall_sec;
    double   latency_avg_ns;
    double   latency_min_ns;
    double   latency_max_ns;
    double   latency_p95_ns;
    double   latency_p99_ns;
} sb_stats_t;

/* Aggregate per-thread counters + histogram into stats. */
sb_stats_t stats_aggregate(sb_tls_t *tls, sb_histogram_t *hist,
                           int nthreads, double wall_sec);

#endif /* STATS_H */