/* I2S Pixel Protocol for WS2812/WS2812B LED strips
 * This file defines the protocol constants and lookup tables for
 * driving WS2812/WS2812B RGB and RGBW LED strips using I2S peripheral
 */
#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

    // WS2812B timing constants
#define WS2812B_ZERO 0b100  // Encoding for bit 0
#define WS2812B_ONE  0b110  // Encoding for bit 1

#define WS2812B_BYTES_PER_COLOR  3   // 3 bytes per color channel (24 bits encoded as 3 bytes)
#define WS2812B_COLORS_PER_RGB   3   // RGB = 3 colors
#define WS2812B_COLORS_PER_RGBW  4   // RGBW = 4 colors
#define WS2812B_BYTES_PER_RGB    (WS2812B_BYTES_PER_COLOR * WS2812B_COLORS_PER_RGB)    // 9 bytes
#define WS2812B_BYTES_PER_RGBW   (WS2812B_BYTES_PER_COLOR * WS2812B_COLORS_PER_RGBW)  // 12 bytes

#define WS2812B_BITRATE      2600000UL   // 2.6 Mbps = 385 ns/bit
#define WS2812B_RESET_BITS   ((50 * WS2812B_BITRATE / 1000000UL) + 1) // 50 uS of zero bits
#define WS2812B_RESET_BYTES  ((WS2812B_RESET_BITS + 7) / 8) // Zero bytes for reset

// Lookup table type for color encoding
    typedef uint8_t ws2812b_color_encoding[WS2812B_BYTES_PER_COLOR];

    // Color lookup table - converts a single color value (0-255) to its I2S bitstream
    extern const ws2812b_color_encoding ws2812b_color_lookup[256];

#ifdef __cplusplus
}
#endif
