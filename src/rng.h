#ifndef RNG_H
#define RNG_H

#include <stdint.h>

/* splitmix64: small, fast, and good enough to seed and shuffle. */
static inline uint64_t rng_next(uint64_t *state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Uniform in [0, n). The modulo bias is negligible for the small n used here. */
static inline uint64_t rng_below(uint64_t *state, uint64_t n) {
    return rng_next(state) % n;
}

#endif
