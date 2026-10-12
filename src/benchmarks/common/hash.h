#ifndef BENCH_HASH_H
#define BENCH_HASH_H

/**
 * hash.h — FNV-1a, 64 bit.
 *
 * Four kernels need a cheap content digest: for a result field whose value is
 * only folded into lengths that are then discarded, and as the stand-in for the
 * reference implementation's sha256/md5 (a documented deviation — the digest
 * never reaches an op count).
 */

#include <stddef.h>
#include <stdint.h>

uint64_t hash_fnv1a64(const char *s, size_t len);

#endif /* BENCH_HASH_H */
