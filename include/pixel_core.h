#pragma once

#include <cstdint>
#include <array>
#include "pixel_version.h"

// Portable core types for LED pixel effects
// Used by both ESP32 and WASM builds

// Version API
inline const char* getPixelDriverVersion() {
    return PIXDRIVER_GIT_COMMIT;
}

inline const char* getPixelDriverVersionFull() {
    return PIXDRIVER_GIT_COMMIT_FULL;
}

inline const char* getPixelDriverBuildTime() {
    return PIXDRIVER_BUILD_TIMESTAMP;
}

enum class PixelFormat : uint8_t {
    RGB = 3,
    RGBW = 4
};

struct PixelColor {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t w = 0;

    constexpr PixelColor() = default;
    constexpr PixelColor(uint8_t red, uint8_t green, uint8_t blue, uint8_t white = 0) noexcept
        : r(red), g(green), b(blue), w(white) {}

    // Create from 32-bit RGB/RGBW value
    static constexpr PixelColor fromRGB(uint32_t rgb) noexcept {
        return PixelColor(
            static_cast<uint8_t>((rgb >> 16) & 0xFF),
            static_cast<uint8_t>((rgb >> 8) & 0xFF),
            static_cast<uint8_t>(rgb & 0xFF)
        );
    }

    // HSV to RGB conversion (inline for portability)
    static PixelColor fromHSV(uint8_t hue, uint8_t saturation, uint8_t value) noexcept {
        if (saturation == 0) {
            return PixelColor(value, value, value);
        }

        const uint8_t region = hue / 43;
        const uint8_t remainder = (hue - (region * 43)) * 6;

        const uint8_t p = (value * (255 - saturation)) >> 8;
        const uint8_t q = (value * (255 - ((saturation * remainder) >> 8))) >> 8;
        const uint8_t t = (value * (255 - ((saturation * (255 - remainder)) >> 8))) >> 8;

        switch (region) {
            case 0:  return PixelColor(value, t, p);
            case 1:  return PixelColor(q, value, p);
            case 2:  return PixelColor(p, value, t);
            case 3:  return PixelColor(p, q, value);
            case 4:  return PixelColor(t, p, value);
            default: return PixelColor(value, p, q);
        }
    }

    // Scale color by brightness (0-255)
    [[nodiscard]] constexpr PixelColor scale(uint8_t brightness) const noexcept {
        if (brightness == 255) return *this;
        return PixelColor(
            static_cast<uint8_t>((r * brightness) / 255),
            static_cast<uint8_t>((g * brightness) / 255),
            static_cast<uint8_t>((b * brightness) / 255),
            static_cast<uint8_t>((w * brightness) / 255)
        );
    }

    // Blend two colors
    [[nodiscard]] constexpr PixelColor blend(const PixelColor& other, uint8_t amount) const noexcept {
        const uint8_t inv = 255 - amount;
        return PixelColor(
            static_cast<uint8_t>((r * inv + other.r * amount) / 255),
            static_cast<uint8_t>((g * inv + other.g * amount) / 255),
            static_cast<uint8_t>((b * inv + other.b * amount) / 255),
            static_cast<uint8_t>((w * inv + other.w * amount) / 255)
        );
    }

    // Common colors
    static constexpr PixelColor Black() noexcept { return PixelColor(0, 0, 0); }
    static constexpr PixelColor White() noexcept { return PixelColor(255, 255, 255); }
    static constexpr PixelColor Red() noexcept { return PixelColor(255, 0, 0); }
    static constexpr PixelColor Green() noexcept { return PixelColor(0, 255, 0); }
    static constexpr PixelColor Blue() noexcept { return PixelColor(0, 0, 255); }
    static constexpr PixelColor Yellow() noexcept { return PixelColor(255, 255, 0); }
    static constexpr PixelColor Cyan() noexcept { return PixelColor(0, 255, 255); }
    static constexpr PixelColor Magenta() noexcept { return PixelColor(255, 0, 255); }

    constexpr bool operator==(const PixelColor& other) const noexcept {
        return r == other.r && g == other.g && b == other.b && w == other.w;
    }
    constexpr bool operator!=(const PixelColor& other) const noexcept {
        return !(*this == other);
    }
};

// Gamma correction table for more natural-looking brightness
inline constexpr std::array<uint8_t, 256> GAMMA_TABLE = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   1,   1,   1,   1,
    1,   1,   1,   1,   1,   1,   1,   1,   1,   2,   2,   2,   2,   2,   2,   2,
    2,   3,   3,   3,   3,   3,   3,   3,   4,   4,   4,   4,   4,   5,   5,   5,
    5,   6,   6,   6,   6,   7,   7,   7,   7,   8,   8,   8,   9,   9,   9,  10,
   10,  10,  11,  11,  11,  12,  12,  13,  13,  13,  14,  14,  15,  15,  16,  16,
   17,  17,  18,  18,  19,  19,  20,  20,  21,  21,  22,  22,  23,  24,  24,  25,
   25,  26,  27,  27,  28,  29,  29,  30,  31,  32,  32,  33,  34,  35,  35,  36,
   37,  38,  39,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  50,
   51,  52,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  66,  67,  68,
   69,  70,  72,  73,  74,  75,  77,  78,  79,  81,  82,  83,  85,  86,  87,  89,
   90,  92,  93,  95,  96,  98,  99, 101, 102, 104, 105, 107, 109, 110, 112, 114,
  115, 117, 119, 120, 122, 124, 126, 127, 129, 131, 133, 135, 137, 138, 140, 142,
  144, 146, 148, 150, 152, 154, 156, 158, 160, 162, 164, 167, 169, 171, 173, 175,
  177, 180, 182, 184, 186, 189, 191, 193, 196, 198, 200, 203, 205, 208, 210, 213,
  215, 218, 220, 223, 225, 228, 231, 233, 236, 239, 241, 244, 247, 249, 252, 255
};

// Apply gamma correction
[[nodiscard]] inline constexpr uint8_t gammaCorrect(uint8_t value) noexcept {
    return GAMMA_TABLE[value];
}

// Sin wave lookup table generation for smooth animations (256 entries, 0-255 output)
inline constexpr std::array<uint8_t, 256> generateSinTable() {
    std::array<uint8_t, 256> table{};
    for (int i = 0; i < 256; ++i) {
        int angle = i;
        int result;
        if (angle < 128) {
            result = (angle < 64) ? angle * 4 : (128 - angle) * 4;
        } else {
            angle -= 128;
            result = (angle < 64) ? -(angle * 4) : -((128 - angle) * 4);
        }
        table[i] = static_cast<uint8_t>((result + 256) / 2);
    }
    return table;
}

inline constexpr std::array<uint8_t, 256> SIN_TABLE = generateSinTable();

// Effect state - shared between ESP32 and WASM
struct EffectState {
    uint32_t last_update_tick = 0;
    uint32_t phase = 0;
    uint8_t counter = 0;
    bool direction = false;

    // Union for effect-specific state to save memory
    union {
        struct { uint8_t brightness; bool increasing; } breathe;
        struct { uint16_t pixel; bool clearing; } wipe;
        struct { uint8_t offset; } chase;
        struct { uint8_t offset; } rainbow;
        struct { uint8_t offset; } cyclic;
        struct { int16_t head; uint8_t tail_length; } comet;
        struct { uint8_t position; } wave;
        struct { uint8_t heat[64]; } fire;  // Heat map for fire effect
    };

    EffectState() : breathe{128, true} {}
};
