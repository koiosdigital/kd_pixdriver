#include "pixel_effects.h"
#include "esp_log.h"
#include "esp_random.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <utility>
#include <unordered_map>
#include <functional>
#include <cctype>

PixelEffectEngine::PixelEffectEngine(uint32_t update_rate_hz)
    : update_rate_hz_(update_rate_hz) {
    // Reserve space for a reasonable number of channels
    channel_states_.reserve(8);

    // Register all built-in effects
    registerEffect("SOLID", "Solid", [](PixelEffectEngine* engine, PixelChannel* channel, uint32_t tick) {
        engine->applySolid(channel);
        });
    registerEffect("BLINK", "Blink", [](PixelEffectEngine* engine, PixelChannel* channel, uint32_t tick) {
        engine->applyBlink(channel, tick);
        });
    registerEffect("BREATHE", "Breathe", [](PixelEffectEngine* engine, PixelChannel* channel, uint32_t tick) {
        engine->applyBreathe(channel, tick);
        });
    registerEffect("CYCLIC", "Cyclic", [](PixelEffectEngine* engine, PixelChannel* channel, uint32_t tick) {
        engine->applyCyclic(channel, tick);
        });
    registerEffect("RAINBOW", "Rainbow", [](PixelEffectEngine* engine, PixelChannel* channel, uint32_t tick) {
        engine->applyRainbow(channel, tick);
        });
    registerEffect("COLOR_WIPE", "Color Wipe", [](PixelEffectEngine* engine, PixelChannel* channel, uint32_t tick) {
        engine->applyColorWipe(channel, tick);
        });
    registerEffect("THEATER_CHASE", "Theater Chase", [](PixelEffectEngine* engine, PixelChannel* channel, uint32_t tick) {
        engine->applyTheaterChase(channel, tick);
        });
    registerEffect("SPARKLE", "Sparkle", [](PixelEffectEngine* engine, PixelChannel* channel, uint32_t tick) {
        engine->applySparkle(channel, tick);
        });
}

void PixelEffectEngine::updateEffect(PixelChannel* channel, uint32_t tick) {
    if (!channel) return;

    const auto& config = channel->getEffectConfig();
    ensureChannelState(channel->getId());

    const std::string& effect_name = config.effect;

    auto to_upper = [](const std::string& value) {
        std::string upper = value;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
            });
        return upper;
        };

    std::string normalized = to_upper(effect_name);

    // Raw mode indicates the firmware is managing the pixel buffer directly.
    if (normalized == "RAW") {
        return;
    }

    // Look up effect in registry (case-insensitive).
    auto it = effect_registry_.find(effect_name);
    if (it == effect_registry_.end()) {
        it = effect_registry_.find(normalized);
    }

    if (it != effect_registry_.end()) {
        it->second.fn(this, channel, tick);
    }
    else {
        // Fallback to solid
        applySolid(channel);
    }
}

void PixelEffectEngine::applySolid(PixelChannel* channel) {
    const auto& config = channel->getEffectConfig();
    auto& buffer = channel->getPixelBuffer();

    // Use unscaled color - brightness scaling happens later
    std::fill(buffer.begin(), buffer.end(), config.color);
}

void PixelEffectEngine::applyBlink(PixelChannel* channel, uint32_t tick) {
    const auto& config = channel->getEffectConfig();
    auto& buffer = channel->getPixelBuffer();
    auto& state = channel_states_[channel->getId()];

    uint32_t interval = getEffectInterval(config.speed);

    if (tick - state.last_update_tick >= interval) {
        state.direction = !state.direction;  // Toggle blink state
        state.last_update_tick = tick;
    }

    if (state.direction) {
        PixelColor scaled_color = config.color;
        std::fill(buffer.begin(), buffer.end(), scaled_color);
    }
    else {
        std::fill(buffer.begin(), buffer.end(), PixelColor(0, 0, 0, 0));
    }
}

void PixelEffectEngine::applyBreathe(PixelChannel* channel, uint32_t tick) {
    const auto& config = channel->getEffectConfig();
    auto& buffer = channel->getPixelBuffer();
    auto& state = channel_states_[channel->getId()];

    uint32_t interval = getEffectInterval(config.speed) / 4;  // Smoother breathing

    if (tick - state.last_update_tick >= interval) {
        if (state.breathe.increasing) {
            state.breathe.current_brightness += 5;
            if (state.breathe.current_brightness >= 255) {
                state.breathe.current_brightness = 255;
                state.breathe.increasing = false;
            }
        }
        else {
            state.breathe.current_brightness -= 5;
            if (state.breathe.current_brightness <= 0) {
                state.breathe.current_brightness = 0;
                state.breathe.increasing = true;
            }
        }
        state.last_update_tick = tick;
    }

    // Apply breathe animation (0-255) to the base color
    PixelColor breathe_color = config.color.scale(state.breathe.current_brightness);
    std::fill(buffer.begin(), buffer.end(), breathe_color);
}

void PixelEffectEngine::applyCyclic(PixelChannel* channel, uint32_t tick) {
    const auto& config = channel->getEffectConfig();
    auto& buffer = channel->getPixelBuffer();
    auto& state = channel_states_[channel->getId()];

    uint32_t interval = getEffectInterval(config.speed);

    if (tick - state.last_update_tick >= interval) {
        state.cyclic.trail_offset = (state.cyclic.trail_offset + 1) % buffer.size();
        state.last_update_tick = tick;
    }

    // Clear buffer
    std::fill(buffer.begin(), buffer.end(), PixelColor(0, 0, 0, 0));

    // Draw trail
    const int trail_length = std::min(5, static_cast<int>(buffer.size()));
    PixelColor base_color = config.color;

    for (int i = 0; i < trail_length; ++i) {
        int pixel_idx = (state.cyclic.trail_offset + i) % buffer.size();
        uint8_t fade = 255 - (i * 255 / trail_length);
        buffer[pixel_idx] = base_color.scale(fade);
    }
}

void PixelEffectEngine::applyRainbow(PixelChannel* channel, uint32_t tick) {
    const auto& config = channel->getEffectConfig();
    auto& buffer = channel->getPixelBuffer();
    auto& state = channel_states_[channel->getId()];

    uint32_t interval = getEffectInterval(config.speed);

    if (tick - state.last_update_tick >= interval) {
        state.rainbow.rainbow_offset = (state.rainbow.rainbow_offset + 1) % 256;
        state.last_update_tick = tick;
    }

    for (size_t i = 0; i < buffer.size(); ++i) {
        uint8_t hue = static_cast<uint8_t>((i * 256 / buffer.size()) + state.rainbow.rainbow_offset) % 256;
        buffer[i] = PixelColor::fromHSV(hue, 255, config.brightness);
    }
}

void PixelEffectEngine::applyColorWipe(PixelChannel* channel, uint32_t tick) {
    const auto& config = channel->getEffectConfig();
    auto& buffer = channel->getPixelBuffer();
    auto& state = channel_states_[channel->getId()];

    uint32_t interval = getEffectInterval(config.speed);

    if (tick - state.last_update_tick >= interval) {
        if (!state.color_wipe.clearing) {
            if (state.color_wipe.current_pixel < buffer.size()) {
                state.color_wipe.current_pixel++;
            }
            else {
                state.color_wipe.clearing = true;
                state.color_wipe.current_pixel = 0;
            }
        }
        else {
            if (state.color_wipe.current_pixel < buffer.size()) {
                state.color_wipe.current_pixel++;
            }
            else {
                state.color_wipe.clearing = false;
                state.color_wipe.current_pixel = 0;
            }
        }
        state.last_update_tick = tick;
    }

    PixelColor fill_color = state.color_wipe.clearing ?
        PixelColor(0, 0, 0, 0) : config.color;

    // Fill pixels up to current position
    for (size_t i = 0; i < state.color_wipe.current_pixel && i < buffer.size(); ++i) {
        buffer[i] = fill_color;
    }

    // Clear remaining pixels if not clearing phase
    if (!state.color_wipe.clearing) {
        for (size_t i = state.color_wipe.current_pixel; i < buffer.size(); ++i) {
            buffer[i] = PixelColor(0, 0, 0, 0);
        }
    }
}

void PixelEffectEngine::applyTheaterChase(PixelChannel* channel, uint32_t tick) {
    const auto& config = channel->getEffectConfig();
    auto& buffer = channel->getPixelBuffer();
    auto& state = channel_states_[channel->getId()];

    uint32_t interval = getEffectInterval(config.speed);

    if (tick - state.last_update_tick >= interval) {
        state.theater_chase.offset = (state.theater_chase.offset + 1) % 3;
        state.last_update_tick = tick;
    }

    PixelColor on_color = config.color;

    for (size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = ((i + state.theater_chase.offset) % 3 == 0) ?
            on_color : PixelColor(0, 0, 0, 0);
    }
}

void PixelEffectEngine::applySparkle(PixelChannel* channel, uint32_t tick) {
    const auto& config = channel->getEffectConfig();
    auto& buffer = channel->getPixelBuffer();
    auto& state = channel_states_[channel->getId()];

    uint32_t interval = getEffectInterval(config.speed) / 2;  // Faster sparkle

    if (tick - state.last_update_tick >= interval) {
        // Clear all pixels first
        std::fill(buffer.begin(), buffer.end(), PixelColor(0, 0, 0, 0));

        // Randomly light some pixels
        for (size_t i = 0; i < buffer.size(); ++i) {
            if ((esp_random() % 20) == 0) {  // 5% chance per pixel
                buffer[i] = config.color;
            }
        }

        state.last_update_tick = tick;
    }
}

void PixelEffectEngine::applyCustom(PixelChannel* channel, uint32_t tick) {
    const auto& config = channel->getEffectConfig();

    if (config.custom_effect) {
        auto& buffer = channel->getPixelBuffer();
        config.custom_effect(buffer, tick);
    }
    else {
        // Fallback to solid if no custom effect is defined
        applySolid(channel);
    }
}

uint32_t PixelEffectEngine::getEffectInterval(uint8_t speed) const {
    // Convert speed (1-10) to update interval in ticks
    // Higher speed = lower interval = faster updates
    const uint32_t base_interval = update_rate_hz_ / 10;  // 100ms at 60Hz
    return base_interval * (11 - std::clamp(speed, uint8_t(1), uint8_t(10)));
}

void PixelEffectEngine::ensureChannelState(int32_t channel_id) {
    if (channel_id >= static_cast<int32_t>(channel_states_.size())) {
        channel_states_.resize(channel_id + 1);

        // Initialize new state
        auto& state = channel_states_[channel_id];
        state.last_update_tick = 0;
        state.phase = 0;
        state.direction = false;

        // Initialize effect-specific state
        state.breathe.current_brightness = 0;
        state.breathe.increasing = true;
        state.color_wipe.current_pixel = 0;
        state.color_wipe.clearing = false;
        state.theater_chase.offset = 0;
        state.rainbow.rainbow_offset = 0;
        state.cyclic.trail_offset = 0;
    }
}

void PixelEffectEngine::registerEffect(const std::string& name, const std::string& display_name, EffectFn fn) {
    effect_registry_[name] = EffectEntry{ fn, display_name };
}

void PixelEffectEngine::registerEffect(const std::string& name, EffectFn fn) {
    // For backward compatibility, use name as display name
    registerEffect(name, name, fn);
}

std::vector<PixelEffectEngine::EffectInfo> PixelEffectEngine::getAllEffects() const {
    std::vector<EffectInfo> effects;
    for (const auto& [id, entry] : effect_registry_) {
        effects.push_back({ id, entry.display_name });
    }
    return effects;
}

void PixelEffectEngine::unregisterEffect(const std::string& name) {
    effect_registry_.erase(name);
}
