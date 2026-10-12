#include "workers.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int work_threads(int total, int threads) {
    if (threads < 1) return 1;
    return threads > total ? total : threads;
}

int work_slice(int total, int threads, int i) {
    return total / threads + (i < total % threads ? 1 : 0);
}

void *work_ops(long long ops) {
    return (void *)(intptr_t)ops;
}

long long work_run(void *workers, size_t stride, int threads,
                   void *(*body)(void *), const char *who) {
    pthread_t *ids = (pthread_t *)malloc((size_t)threads * sizeof(pthread_t));
    if (!ids) {
        fprintf(stderr, "error: %s: out of memory for %d threads\n", who, threads);
        return 0;
    }

    int started = 0;
    for (int i = 0; i < threads; i++) {
        if (pthread_create(&ids[i], NULL, body,
                           (char *)workers + (size_t)i * stride) == 0) {
            started++;
            continue;
        }
        fprintf(stderr, "error: %s: pthread_create failed for worker %d/%d\n",
                who, i, threads);
        break;
    }

    long long total = 0;
    for (int i = 0; i < started; i++) {
        void *ret = NULL;
        pthread_join(ids[i], &ret);
        total += (long long)(intptr_t)ret;
    }
    free(ids);
    return total;
}
