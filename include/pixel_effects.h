#pragma once

#include "kd_pixdriver.h"
#include <cstdint>
#include <vector>
#include <string>
#include <string_view>
#include <unordered_map>
#include <functional>
#include <array>

class PixelEffectEngine {
public:
    explicit PixelEffectEngine(uint32_t update_rate_hz);

    void updateEffect(PixelChannel* channel, uint32_t tick);

    // Effect registration
    struct EffectInfo {
        std::string id;
        std::string display_name;
    };

    using EffectFn = std::function<void(PixelEffectEngine*, PixelChannel*, uint32_t)>;

    void registerEffect(std::string_view name, std::string_view display_name, EffectFn fn);
    void unregisterEffect(std::string_view name);
    [[nodiscard]] std::vector<EffectInfo> getAllEffects() const;

private:
    uint32_t update_rate_hz_;

    struct EffectEntry {
        EffectFn fn;
        std::string display_name;
    };
    std::unordered_map<std::string, EffectEntry> effect_registry_;

    // Built-in effect implementations
    void applySolid(PixelChannel* channel);
    void applyBlink(PixelChannel* channel, uint32_t tick);
    void applyBreathe(PixelChannel* channel, uint32_t tick);
    void applyCyclic(PixelChannel* channel, uint32_t tick);
    void applyRainbow(PixelChannel* channel, uint32_t tick);
    void applyColorWipe(PixelChannel* channel, uint32_t tick);
    void applyTheaterChase(PixelChannel* channel, uint32_t tick);
    void applySparkle(PixelChannel* channel, uint32_t tick);

    // New effects
    void applyComet(PixelChannel* channel, uint32_t tick);
    void applyFire(PixelChannel* channel, uint32_t tick);
    void applyWave(PixelChannel* channel, uint32_t tick);
    void applyTwinkle(PixelChannel* channel, uint32_t tick);
    void applyGradient(PixelChannel* channel, uint32_t tick);
    void applyPulse(PixelChannel* channel, uint32_t tick);
    void applyMeteor(PixelChannel* channel, uint32_t tick);
    void applyRunningLights(PixelChannel* channel, uint32_t tick);

    // Effect state - using a more memory-efficient approach
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
    };

    std::vector<EffectState> channel_states_;

    // Helper functions
    [[nodiscard]] uint32_t getEffectInterval(uint8_t speed) const noexcept;
    void ensureChannelState(int32_t channel_id);

    // Utility for gamma correction
    [[nodiscard]] static uint8_t gammaCorrect(uint8_t value) noexcept;

    // Sin wave lookup for smooth animations (256 entries, 0-255 output)
    static constexpr std::array<uint8_t, 256> generateSinTable() {
        std::array<uint8_t, 256> table{};
        for (int i = 0; i < 256; ++i) {
            // Using integer approximation of sin
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

    static const std::array<uint8_t, 256> sin_table_;
};
