/**
 * pyrand.h — faithful C port of Python 3.11's `random.Random` (MT19937).
 *
 * Used by benchmarks that must reproduce the reference Python implementation
 * op-for-op. Implements the exact PRNG interface the reference consumes:
 *
 *   pr_seed        — CPython random_seed(int) -> init_by_array([int])
 *   pr_random      — random(): 53-bit double (genrand_res53)
 *   pr_randint     — randint(a, b) inclusive
 *   pr_choices     — choices(seq, k): index = floor(random() * n)  (no weights)
 *   pr_choice      — choice(seq): seq[randbelow(len(seq))]
 *   pr_gauss       — gauss(mu, sigma): Box-Muller, cos returned / sin cached
 *
 * `pr_choices` / `pr_choice` are here because the reference uses both
 * `random()` (for choices) and `randbelow` (for choice/randint) streams and
 * they must interleave exactly.
 *
 * All functions are static inline so each translation unit gets its own copy
 * (the generator is stateful, not thread-safe by design).
 */

#ifndef PYRAND_H
#define PYRAND_H

#include <assert.h>
#include <stdint.h>
#include <math.h>

#define PYRAND_PI 3.14159265358979323846

typedef struct {
    uint32_t mt[624];
    int index;
    int gauss_set;
    double gauss_next;
} pyrand_t;

static inline void pr_seed(pyrand_t *r, uint32_t seedval) {
    /* CPython random_seed(int) -> init_by_array([int]) */
    r->mt[0] = 19650218U;
    for (int i = 1; i < 624; i++) {
        uint32_t p = r->mt[i - 1];
        r->mt[i] = (uint32_t)((1812433253U * (p ^ (p >> 30)) + (uint32_t)i) & 0xffffffffU);
    }
    uint32_t key[1] = { seedval };
    int i = 1, j = 0;
    for (int k = 624; k; k--) {
        uint32_t p = r->mt[i - 1];
        r->mt[i] = ((r->mt[i] ^ ((p ^ (p >> 30)) * 1664525U)) + key[j] + (uint32_t)j)
                   & 0xffffffffU;
        i++;
        j++;
        if (i >= 624) { r->mt[0] = r->mt[623]; i = 1; }
        if (j >= 1) j = 0;
    }
    for (int k = 623; k; k--) {
        uint32_t p = r->mt[i - 1];
        r->mt[i] = ((r->mt[i] ^ ((p ^ (p >> 30)) * 1566083941U)) - (uint32_t)i)
                   & 0xffffffffU;
        i++;
        if (i >= 624) { r->mt[0] = r->mt[623]; i = 1; }
    }
    r->mt[0] = 0x80000000U;
    r->index = 624;
    r->gauss_set = 0;
    r->gauss_next = 0.0;
}

static inline uint32_t pr_next32(pyrand_t *r) {
    if (r->index >= 624) {
        for (int kk = 0; kk < 227; kk++) {
            uint32_t y = (r->mt[kk] & 0x80000000U) | (r->mt[kk + 1] & 0x7fffffffU);
            r->mt[kk] = r->mt[kk + 397] ^ (y >> 1) ^ (y & 1 ? 0x9908b0dfU : 0U);
        }
        for (int kk = 227; kk < 623; kk++) {
            uint32_t y = (r->mt[kk] & 0x80000000U) | (r->mt[kk + 1] & 0x7fffffffU);
            r->mt[kk] = r->mt[kk - 227] ^ (y >> 1) ^ (y & 1 ? 0x9908b0dfU : 0U);
        }
        uint32_t y = (r->mt[623] & 0x80000000U) | (r->mt[0] & 0x7fffffffU);
        r->mt[623] = r->mt[396] ^ (y >> 1) ^ (y & 1 ? 0x9908b0dfU : 0U);
        r->index = 0;
    }
    uint32_t y = r->mt[r->index++];
    y ^= y >> 11;
    y ^= (y << 7) & 0x9d2c5680U;
    y ^= (y << 15) & 0xefc60000U;
    y ^= y >> 18;
    return y;
}

/* getrandbits(k): top k bits of a 32-bit word (k <= 32, matches CPython) */
static inline uint32_t pr_getrandbits(pyrand_t *r, int k) {
    uint32_t w = pr_next32(r);
    if (k < 32) w >>= (32 - k);
    return w;
}

/* randbelow(n): rejection sampling, matches CPython _randbelow */
static inline uint32_t pr_randbelow(pyrand_t *r, uint32_t n) {
    int k = 0;
    for (uint32_t t = n; t; t >>= 1) k++;
    for (;;) {
        uint32_t v = pr_getrandbits(r, k);
        if (v < n) return v;
    }
}

/* randint(a, b) inclusive = a + randbelow(b - a + 1) */
static inline int32_t pr_randint(pyrand_t *r, int32_t a, int32_t b) {
    /* CPython randint(a, b) raises ValueError when b < a; assert instead of
     * letting (b - a + 1) wrap to a huge unsigned width and return garbage. */
    assert(b >= a);
    uint32_t width = (uint32_t)(b - a + 1);
    return a + (int32_t)pr_randbelow(r, width);
}

/* random(): 53-bit double, matches CPython genrand_res53 */
static inline double pr_random(pyrand_t *r) {
    uint32_t a = pr_next32(r) >> 5;
    uint32_t b = pr_next32(r) >> 6;
    return (a * 67108864.0 + b) / 9007199254740992.0;
}

/* choice(seq): seq[randbelow(len(seq))] */
static inline int32_t pr_choice(pyrand_t *r, int32_t n) {
    return (int32_t)pr_randbelow(r, (uint32_t)n);
}

/* choices(seq, k) with no weights: index = floor(random() * n) per draw */
static inline int32_t pr_choices(pyrand_t *r, int32_t n) {
    return (int32_t)(pr_random(r) * (double)n);
}

/* gauss(mu, sigma): Box-Muller with one cached value, matches CPython 3.11
 * random.py gauss(): returns cos(x2pi)*g2rad, caches sin(x2pi)*g2rad. */
static inline double pr_gauss(pyrand_t *r, double mu, double sigma) {
    if (r->gauss_set) {
        r->gauss_set = 0;
        return mu + r->gauss_next * sigma;
    }
    double x2pi = pr_random(r) * 2.0 * PYRAND_PI;
    double g2rad = sqrt(-2.0 * log(1.0 - pr_random(r)));
    r->gauss_next = sin(x2pi) * g2rad;
    r->gauss_set = 1;
    return mu + (cos(x2pi) * g2rad) * sigma;
}

#endif /* PYRAND_H */
