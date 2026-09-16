#pragma once

#include "kd_pixdriver.h"
#include "pixel_core.h"
#include <cstdint>
#include <vector>
#include <string>
#include <string_view>
#include <functional>

class PixelEffectEngine {
public:
    explicit PixelEffectEngine(uint32_t update_rate_hz);

    /// Render one frame of `channel`'s effect into its pixel buffer at FULL
    /// scale - brightness and current limiting are applied afterwards by the
    /// driver. Called by the driver task under PixelDriver::Lock. Empty
    /// buffers are skipped.
    void updateEffect(PixelChannel* channel, uint32_t tick);

    // Effect registration
    struct EffectInfo {
        std::string id;
        std::string display_name;
    };

    using EffectFn = std::function<void(PixelEffectEngine*, PixelChannel*, uint32_t)>;

    /// Register (or replace, case-insensitively by id) an effect. Do not call
    /// from inside an effect callback.
    void registerEffect(std::string_view name, std::string_view display_name, EffectFn fn);
    void unregisterEffect(std::string_view name);
    [[nodiscard]] std::vector<EffectInfo> getAllEffects() const;

    // Index-based dispatch. A channel resolves its effect id once (when the
    // effect is set) and caches the index; the registry generation lets it
    // notice (un)registrations or an engine rebuild and re-resolve, so no
    // string hashing happens per frame.
    static constexpr int kEffectUnknown = -1;  // unknown id: falls back to SOLID
    static constexpr int kEffectRaw = -2;      // "RAW": firmware writes the buffer directly
    [[nodiscard]] int resolveEffect(std::string_view id) const;
    [[nodiscard]] uint32_t registryGeneration() const noexcept { return generation_; }

    /// Forget a channel's animation state (call when a channel id is reused).
    void resetChannelState(int32_t channel_id);

private:
    uint32_t update_rate_hz_;

    struct EffectEntry {
        std::string id;
        std::string display_name;
        EffectFn fn;
        bool active;  // unregister deactivates in place so indices stay stable
    };
    std::vector<EffectEntry> effects_;
    uint32_t generation_;

    [[nodiscard]] int findEntry(std::string_view id) const;

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

    // Per-channel animation state, indexed by channel id (EffectState and
    // SIN_TABLE come from pixel_core.h, shared with the WASM preview).
    std::vector<EffectState> channel_states_;

    // Helper functions
    [[nodiscard]] uint32_t getEffectInterval(uint8_t speed) const noexcept;
    void ensureChannelState(int32_t channel_id);

    // Utility for gamma correction
    [[nodiscard]] static uint8_t gammaCorrect(uint8_t value) noexcept;
};
