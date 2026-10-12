#ifndef BENCH_WORKERS_H
#define BENCH_WORKERS_H

/**
 * workers.h — run a fixed workload across N threads.
 *
 * Every kernel wraps its own work in the same mechanics: spread the workload
 * over the workers, start one thread per worker, join them, and add up the ops
 * each one counted.  This module owns those mechanics, so a kernel keeps only
 * its work struct and its worker body:
 *
 *     typedef struct { int sets; const char *bin; } ss_work_t;
 *
 *     static void *ss_worker(void *arg) {
 *         ss_work_t *w = (ss_work_t *)arg;
 *         long long ops = 0;
 *         for (int it = 0; it < w->sets; it++) ops += run_once(w->bin);
 *         return work_ops(ops);
 *     }
 *
 *     ...
 *     threads = work_threads(total_sets, threads);
 *     ss_work_t *work = calloc((size_t)threads, sizeof *work);
 *     for (int i = 0; i < threads; i++)
 *         work[i].sets = work_slice(total_sets, threads, i);
 *     long long total = work_run(work, sizeof *work, threads, ss_worker,
 *                                "subprocess_spawning");
 *     free(work);
 */

#include <stddef.h>

/* A thread count that is useful for `total` units of work: at least one, and
 * never more workers than there is work to hand out. */
int work_threads(int total, int threads);

/* How many of `total` units go to worker `i` when they are spread over
 * `threads` workers: an even share, with the remainder handed to the first
 * workers, so the shares add up to `total` exactly. */
int work_slice(int total, int threads, int i);

/* The op count a worker returns from its thread body. */
void *work_ops(long long ops);

/* Start `threads` workers, each handed `(char *)workers + i * stride`, then
 * join them and return the sum of the op counts they returned.  `who` names the
 * caller in error messages.  The caller owns the `workers` array; the thread
 * ids are this function's own business.
 *
 * A worker that cannot be started is reported on stderr while the ones that did
 * start are still joined and counted, so a run that only partly started reports
 * what it measured instead of reporting nothing. */
long long work_run(void *workers, size_t stride, int threads,
                   void *(*body)(void *), const char *who);

#endif /* BENCH_WORKERS_H */
