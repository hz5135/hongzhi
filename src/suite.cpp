#include "suite.h"
#include "run.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The benchmark registry: name, weight, and the one-line description
 * `list simulation` prints. One entry per benchmark kernel, added by the PR
 * that introduces the kernel. The weight is the scenario's observed frequency
 * under business scenarios (docs/designs/simulation mode.md); nothing reads it
 * yet, because no score is computed. */
const bench_weight_t ALL_WEIGHTS[] = {
    {"json_processing",    0.10, "JSON serialization + deserialization"},
    {"text_processing",    0.08, "Chunking, regex-like extraction, log parsing"},
};
const int NUM_BENCHMARKS = sizeof(ALL_WEIGHTS) / sizeof(ALL_WEIGHTS[0]);

/* The three multi-agent (concurrent) scenarios. A single-agent run (-t 1)
 * executes every scenario except these; they only mean something with several
 * agents. None of the benchmarks in this build is one of them, so the set is
 * empty today and the selection in run_suite() picks the same benchmarks for
 * -t 1 and -t N. The mechanism stays in place for the scenarios that need it. */
static int is_multi_agent_benchmark(const char *name) {
    return strcmp(name, "context_switching") == 0 ||
           strcmp(name, "concurrent_dispatch") == 0 ||
           strcmp(name, "subprocess_spawning") == 0;
}

static int parse_benchmark_list(const char *list, char ***out) {
    if (!list || !*list) return 0;
    /* Count commas */
    int n = 1;
    for (const char *p = list; *p; p++)
        if (*p == ',') n++;
    *out = (char **)calloc(n, sizeof(char *));
    char *copy = strdup(list);
    int idx = 0;
    char *tok = strtok(copy, ",");
    while (tok && idx < n) {
        (*out)[idx++] = strdup(tok);
        tok = strtok(NULL, ",");
    }
    free(copy);
    return idx;
}

suite_result_t run_suite(config_t *cfg) {
    suite_result_t result;
    memset(&result, 0, sizeof(result));

    /* Determine which benchmarks to run */
    char **bench_names = NULL;
    int n_bench = 0;

    if (cfg->benchmark_names) {
        n_bench = parse_benchmark_list(cfg->benchmark_names, &bench_names);
    }

    /* Validate user-provided names against known benchmarks */
    if (n_bench > 0) {
        for (int i = 0; i < n_bench; i++) {
            int found = 0;
            for (int j = 0; j < NUM_BENCHMARKS; j++) {
                if (strcmp(bench_names[i], ALL_WEIGHTS[j].name) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                fprintf(stderr, "error: unknown benchmark '%s'\n", bench_names[i]);
                for (int k = 0; k < n_bench; k++) free(bench_names[k]);
                free(bench_names);
                result.num_benchmarks = 0;
                result.results = NULL;
                result.bench_names = NULL;
                return result;
            }
        }
    }

    if (n_bench == 0) {
        /* Run all benchmarks: -t 1 (single agent) runs the 10 single-threaded
         * scenarios; -t N >= 2 (multiple agents, the default CPU-count) runs
         * all 13. An explicit -b list always runs exactly what was requested. */
        int eff_threads = cfg->threads > 0 ? cfg->threads
                                           : (int)sysconf(_SC_NPROCESSORS_ONLN);
        int multi_agent = eff_threads > 1;
        n_bench = NUM_BENCHMARKS;
        bench_names = (char **)calloc(n_bench, sizeof(char *));
        int idx = 0;
        for (int i = 0; i < NUM_BENCHMARKS; i++) {
            if (!multi_agent && is_multi_agent_benchmark(ALL_WEIGHTS[i].name))
                continue;
            bench_names[idx++] = strdup(ALL_WEIGHTS[i].name);
        }
        n_bench = idx;
    }

    result.results = (sb_stats_t *)calloc(n_bench, sizeof(sb_stats_t));
    result.bench_names = (char **)calloc(n_bench, sizeof(char *));
    for (int i = 0; i < n_bench; i++)
        result.bench_names[i] = strdup(bench_names[i]);
    result.num_benchmarks = n_bench;
    result.thread_count = cfg->threads > 0 ? cfg->threads : (int)sysconf(_SC_NPROCESSORS_ONLN);

    /* Run multiple iterations */
    int iterations = cfg->iterations > 0 ? cfg->iterations : 1;
    double *elapsed_sum = (double *)calloc(n_bench, sizeof(double));

    for (int iter = 0; iter < iterations; iter++) {
        if (cfg->verbose) {
            printf("  Iteration %d / %d\n", iter + 1, iterations);
        } else {
            /* Non-verbose: update iteration progress in-place with \r, don't repeat the Running line */
            fprintf(stderr, "\r  Iteration %d / %d", iter + 1, iterations);
            fflush(stderr);
        }

        for (int i = 0; i < n_bench; i++) {
            const char *name = bench_names[i];
            if (cfg->verbose) {
                fprintf(stderr, "  Running %s ...\n", name);
                fflush(stderr);
            }
            sb_stats_t stats = run_benchmark(cfg, name, NULL);
            elapsed_sum[i] += stats.wall_sec;

            if (iter == 0) {
                result.results[i] = stats;
            } else {
                result.results[i].total_ops += stats.total_ops;
                result.results[i].wall_sec += stats.wall_sec;
            }
        }
    }

    if (!cfg->verbose && iterations > 0) {
        fprintf(stderr, "\r  Iteration %d / %d done\n", iterations, iterations);
    }

    /* wall_sec accumulates total execution time across N iterations (ops keeps
     * single-round event counts, so ops/sec = single-round ops / avg single-
     * round time = total ops / total time, throughput is consistent) */
    double total_elapsed = 0.0;
    for (int i = 0; i < n_bench; i++) {
        result.results[i].wall_sec = elapsed_sum[i];
        total_elapsed += result.results[i].wall_sec;
    }
    result.total_elapsed_sec = total_elapsed;

    /* Cleanup */
    free(elapsed_sum);
    for (int i = 0; i < n_bench; i++)
        free(bench_names[i]);
    free(bench_names);

    return result;
}

void suite_result_free(suite_result_t *sr) {
    if (sr->results) free(sr->results);
    if (sr->bench_names) {
        for (int i = 0; i < sr->num_benchmarks; i++)
            free(sr->bench_names[i]);
        free(sr->bench_names);
    }
    sr->results = NULL;
    sr->bench_names = NULL;
    sr->num_benchmarks = 0;
}

void suite_result_print(suite_result_t *sr, int output_format) {
    if (output_format == FMT_JSON) {
        printf("{\n");
        printf("  \"results\": [\n");
        for (int i = 0; i < sr->num_benchmarks; i++) {
            sb_stats_t *s = &sr->results[i];
            printf("    {\"benchmark\":\"%s\", \"elapsed_sec\":%.4f, \"ops\":%llu}",
                   sr->bench_names[i], s->wall_sec,
                   (unsigned long long)s->total_ops);
            if (i < sr->num_benchmarks - 1) printf(",");
            printf("\n");
        }
        printf("  ],\n");
        printf("  \"total_elapsed_sec\": %.4f,\n", sr->total_elapsed_sec);
        printf("  \"thread_count\": %d\n", sr->thread_count);
        printf("}\n");
    } else {
        printf("\n  %-22s %14s %18s\n", "Benchmark", "elapsed (sec)", "ops");
        printf("  %s\n", "------------------------------------------------------------");
        for (int i = 0; i < sr->num_benchmarks; i++) {
            printf("  %-22s %14.4f %18llu\n", sr->bench_names[i],
                   sr->results[i].wall_sec,
                   (unsigned long long)sr->results[i].total_ops);
        }
        printf("  %s\n", "------------------------------------------------------------");
        printf("  %-22s %14.4f\n", "Total elapsed", sr->total_elapsed_sec);
        printf("  Thread count:          %d\n", sr->thread_count);
    }
}