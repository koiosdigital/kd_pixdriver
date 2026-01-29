#pragma once

#include <cstdint>

// Platform abstraction for random number generation
// ESP32 uses hardware RNG, WASM uses seeded PRNG

#ifdef __EMSCRIPTEN__
    // WASM implementation - uses seeded xorshift32 PRNG
    void pixel_set_random_seed(uint32_t seed);
    uint32_t pixel_random();
    uint8_t pixel_random_byte();
#else
    // ESP32 implementation - uses hardware RNG
    #include "esp_random.h"
    inline uint32_t pixel_random() { return esp_random(); }
    inline uint8_t pixel_random_byte() { return static_cast<uint8_t>(esp_random() & 0xFF); }
    inline void pixel_set_random_seed(uint32_t) {} // No-op on ESP32
#endif
