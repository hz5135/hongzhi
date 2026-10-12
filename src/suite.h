#ifndef SUITE_H
#define SUITE_H

#include "options.h"
#include "stats.h"

/* Weight entry for one benchmark */
typedef struct {
    const char *name;
    double      weight;
    const char *desc;   /* one-line description shown in `list simulation` */
} bench_weight_t;

/* All 13 benchmark weights (declared in suite.c, usable from main.c) */
extern const bench_weight_t ALL_WEIGHTS[];
extern const int NUM_BENCHMARKS;

/* Result of a suite run */
typedef struct {
    sb_stats_t *results;       /* array, one per benchmark */
    char      **bench_names;   /* array of benchmark names */
    double      total_elapsed_sec;  /* sum of all benchmark run times */
    int         num_benchmarks;
    int         thread_count;
} suite_result_t;

/* Run the full benchmark suite (all or selected benchmarks).
 * Returns the aggregated suite result. */
suite_result_t run_suite(config_t *cfg);

/* Free a suite result. */
void suite_result_free(suite_result_t *sr);

/* Print suite results to stdout. */
void suite_result_print(suite_result_t *sr, int output_format);

#endif /* SUITE_H */