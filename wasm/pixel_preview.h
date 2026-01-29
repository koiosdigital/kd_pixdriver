#pragma once

#include "pixel_core.h"
#include <cstdint>
#include <vector>
#include <string>
#include <functional>

// Lightweight LED effect preview for WASM
// Does not depend on PixelChannel or PixelDriver
class PixelPreview {
public:
    // Initialize with LED count and format
    PixelPreview(uint16_t led_count, bool is_rgbw, uint32_t update_rate_hz = 60);

    // Configure effect
    void setEffect(const std::string& effect_id);
    void setColor(uint8_t r, uint8_t g, uint8_t b, uint8_t w = 0);
    void setBrightness(uint8_t brightness);
    void setSpeed(uint8_t speed);  // 1-10

    // Advance simulation by one tick
    void tick();

    // Reset to tick 0 and clear state
    void reset();

    // Set random seed for reproducible previews
    void setRandomSeed(uint32_t seed);

    // Get current frame data as RGBA array (4 bytes per LED)
    const uint8_t* getFrameData() const;
    size_t getFrameSize() const;  // led_count * 4

    // Get LED count
    uint16_t getLedCount() const { return led_count_; }

    // Get available effect names
    static std::vector<std::string> getEffectList();

private:
    // Effect implementations (portable versions)
    void applySolid();
    void applyBlink();
    void applyBreathe();
    void applyCyclic();
    void applyRainbow();
    void applyColorWipe();
    void applyTheaterChase();
    void applySparkle();
    void applyComet();
    void applyFire();
    void applyWave();
    void applyTwinkle();
    void applyGradient();
    void applyPulse();
    void applyMeteor();
    void applyRunningLights();

    // Get update interval based on speed
    uint32_t getEffectInterval(uint8_t speed) const;

    // Configuration
    uint16_t led_count_;
    bool is_rgbw_;
    uint32_t update_rate_hz_;

    // Effect parameters
    std::string current_effect_;
    PixelColor color_;
    uint8_t brightness_;
    uint8_t speed_;

    // Tick counter
    uint32_t tick_;

    // Effect state
    EffectState state_;

    // Fire effect needs dynamic heat map for >64 LEDs
    std::vector<uint8_t> heat_map_;

    // LED buffer
    std::vector<PixelColor> buffer_;

    // Output buffer (RGBA format for JavaScript)
    mutable std::vector<uint8_t> output_rgba_;
};
