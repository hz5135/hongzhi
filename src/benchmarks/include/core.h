#ifndef BENCH_CORE_H
#define BENCH_CORE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Checked realloc: an allocation failure must abort loudly instead of writing
 * through a NULL pointer somewhere downstream. */
static inline void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) {
        fprintf(stderr, "error: out of memory (realloc %zu bytes)\n", n);
        abort();
    }
    return q;
}

/* All core benchmark kernel functions.
 * Each takes an `int threads` parameter as the last argument:
 *   threads = 1 → serial execution (original behavior)
 *   threads > 1 → kernel internally creates N threads, splits work evenly
 *
 * Compiled with default visibility so LuaJIT ffi.C can resolve them.
 *
 * HOW TO ADD A NEW ATOMIC BENCHMARK (must touch all five places):
 *   1. src/benchmarks/<name>.c      — implement run_<name>(..., int threads)
 *   2. this header                  — declare it with default visibility
 *   3. lua/benchmarks/<name>.lua    — declare ffi.cdef + event() calling it
 *   4. src/suite.cpp ALL_WEIGHTS    — register name + weight
 *   5. Makefile BENCH_SRCS          — add the kernel to the build
 * Run `make test` to verify wiring (missing script/kernel fails loudly). */

__attribute__((visibility("default")))
long long run_tree_search(int total_sets, int mcts_simulations, int mcts_max_depth,
                    int beam_width, int beam_steps, int vocab_size, int threads);

__attribute__((visibility("default")))
long long run_json_processing(int total_sets, int num_schemas, int num_conversations,
                        int messages_per_conversation, int num_round_trips,
                        int threads);

__attribute__((visibility("default")))
long long run_text_processing(int total_sets, int corpus_size, int threads);

__attribute__((visibility("default")))
long long run_diff_patch(int total_sets, int num_file_pairs, int file_lines,
                   int patch_apply_rounds, int multi_file_diffs, int files_per_multi,
                   int threads);

__attribute__((visibility("default")))
long long run_html_parsing(int total_sets, int num_pages, int num_selector_queries,
                     int num_markdown_conversions, int threads);

#endif /* BENCH_CORE_H */