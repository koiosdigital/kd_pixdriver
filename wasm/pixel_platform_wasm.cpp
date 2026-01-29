#include "pixel_platform.h"

// Simple xorshift32 PRNG for WASM
// Fast, deterministic, good enough for visual effects

namespace {
    uint32_t prng_state = 12345;  // Default seed
}

void pixel_set_random_seed(uint32_t seed) {
    prng_state = seed ? seed : 1;  // Avoid 0 state
}

uint32_t pixel_random() {
    // xorshift32 algorithm
    uint32_t x = prng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    prng_state = x;
    return x;
}

uint8_t pixel_random_byte() {
    return static_cast<uint8_t>(pixel_random() & 0xFF);
}
