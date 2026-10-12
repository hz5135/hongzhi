#include "stats.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Histogram                                                           */
/* ------------------------------------------------------------------ */

void hist_init(sb_histogram_t *h) {
    memset(h, 0, sizeof(*h));
    h->min_ns = 1e18;
    h->max_ns = 0;
}

void hist_record(sb_histogram_t *h, double ns) {
    if (ns < h->min_ns) h->min_ns = ns;
    if (ns > h->max_ns) h->max_ns = ns;

    /* Log-linear bucket: use log2 to map nanosecond range */
    /* Bucket 0 = 0-1ns, up to 2^255 ns (~10^68 years) */
    double l2 = log2(ns);
    int idx = l2 < 0 ? 0 : (l2 >= HIST_BUCKETS ? HIST_BUCKETS - 1 : (int)l2);
    h->buckets[idx]++;
    h->count++;
}

double hist_percentile(sb_histogram_t *h, int pct) {
    if (h->count == 0) return 0.0;
    uint64_t target = (uint64_t)(h->count * pct / 100.0);
    if (target == 0) target = 1;
    uint64_t cum = 0;
    for (int i = 0; i < HIST_BUCKETS; i++) {
        cum += h->buckets[i];
        if (cum >= target) {
            /* Return the midpoint of this bucket */
            return (pow(2.0, i) + pow(2.0, i + 1)) / 2.0;
        }
    }
    return h->max_ns;
}

/* ------------------------------------------------------------------ */
/*  Aggregation                                                         */
/* ------------------------------------------------------------------ */

sb_stats_t stats_aggregate(sb_tls_t *tls, sb_histogram_t *hist,
                           int nthreads, double wall_sec) {
    sb_stats_t s;
    memset(&s, 0, sizeof(s));

    s.wall_sec = wall_sec;

    double latency_sum = 0;
    double latency_min = 1e18;
    double latency_max = 0;

    for (int i = 0; i < nthreads; i++) {
        s.total_events += tls[i].events;
        s.total_ops    += tls[i].ops;
        latency_sum    += tls[i].latency_sum;
        if (tls[i].latency_min > 0 && tls[i].latency_min < latency_min)
            latency_min = tls[i].latency_min;
        if (tls[i].latency_max > latency_max)
            latency_max = tls[i].latency_max;
    }

    s.latency_avg_ns = s.total_events > 0 ? latency_sum / s.total_events : 0;
    s.latency_min_ns = latency_min;
    s.latency_max_ns = latency_max;
    s.latency_p95_ns = hist_percentile(hist, 95);
    s.latency_p99_ns = hist_percentile(hist, 99);

    return s;
}