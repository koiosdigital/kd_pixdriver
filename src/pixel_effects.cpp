#include "pixel_effects.h"
#include "pixel_platform.h"
#include "pixel_core.h"
#include <algorithm>
#include <cmath>
#include <cctype>

#ifndef __EMSCRIPTEN__
#include "esp_log.h"
#endif

namespace {
#ifndef __EMSCRIPTEN__
constexpr const char* TAG = "pixel_effects";
#endif

// Fast pseudo-random for effects (not cryptographic)
inline uint32_t fastRandom() {
    return pixel_random();
}

inline uint8_t fastRandomByte() {
    return pixel_random_byte();
}

// Case-insensitive string comparison
inline bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(a[i])) !=
            std::toupper(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

} // anonymous namespace

// Static member initialization
const std::array<uint8_t, 256> PixelEffectEngine::sin_table_ = PixelEffectEngine::generateSinTable();

PixelEffectEngine::PixelEffectEngine(uint32_t update_rate_hz)
    : update_rate_hz_(update_rate_hz) {
    channel_states_.reserve(4);

    // Register all built-in effects
    auto reg = [this](std::string_view id, std::string_view name, auto fn) {
        registerEffect(id, name, [fn](PixelEffectEngine* e, PixelChannel* c, uint32_t t) {
            (e->*fn)(c, t);
        });
    };

    // Original effects
    registerEffect("SOLID", "Solid", [](PixelEffectEngine* e, PixelChannel* c, uint32_t) {
        e->applySolid(c);
    });
    reg("BLINK", "Blink", &PixelEffectEngine::applyBlink);
    reg("BREATHE", "Breathe", &PixelEffectEngine::applyBreathe);
    reg("CYCLIC", "Cyclic", &PixelEffectEngine::applyCyclic);
    reg("RAINBOW", "Rainbow", &PixelEffectEngine::applyRainbow);
    reg("COLOR_WIPE", "Color Wipe", &PixelEffectEngine::applyColorWipe);
    reg("THEATER_CHASE", "Theater Chase", &PixelEffectEngine::applyTheaterChase);
    reg("SPARKLE", "Sparkle", &PixelEffectEngine::applySparkle);

    // New effects
    reg("COMET", "Comet", &PixelEffectEngine::applyComet);
    reg("FIRE", "Fire", &PixelEffectEngine::applyFire);
    reg("WAVE", "Wave", &PixelEffectEngine::applyWave);
    reg("TWINKLE", "Twinkle", &PixelEffectEngine::applyTwinkle);
    reg("GRADIENT", "Gradient", &PixelEffectEngine::applyGradient);
    reg("PULSE", "Pulse", &PixelEffectEngine::applyPulse);
    reg("METEOR", "Meteor", &PixelEffectEngine::applyMeteor);
    reg("RUNNING_LIGHTS", "Running Lights", &PixelEffectEngine::applyRunningLights);
}

void PixelEffectEngine::updateEffect(PixelChannel* channel, uint32_t tick) {
    if (!channel) return;

    const auto& config = channel->getEffectConfig();
    ensureChannelState(channel->getId());

    const std::string& effect_name = config.effect;

    // Raw mode - firmware manages buffer directly
    if (equalsIgnoreCase(effect_name, "RAW")) {
        return;
    }

    // Try exact match first (common case)
    if (auto it = effect_registry_.find(effect_name); it != effect_registry_.end()) {
        it->second.fn(this, channel, tick);
        return;
    }

    // Try case-insensitive search
    for (const auto& [key, entry] : effect_registry_) {
        if (equalsIgnoreCase(key, effect_name)) {
            entry.fn(this, channel, tick);
            return;
        }
    }

    // Fallback to solid
    applySolid(channel);
}

void PixelEffectEngine::applySolid(PixelChannel* channel) {
    const auto& config = channel->getEffectConfig();
    auto& buffer = channel->getPixelBuffer();
    std::fill(buffer.begin(), buffer.end(), config.color);
}

void PixelEffectEngine::applyBlink(PixelChannel* channel, uint32_t tick) {
    const auto& config = channel->getEffectConfig();
    auto& buffer = channel->getPixelBuffer();
    auto& state = channel_states_[channel->getId()];

    const uint32_t interval = getEffectInterval(config.speed);

    if (tick - state.last_update_tick >= interval) {
        state.direction = !state.direction;
        state.last_update_tick = tick;
    }

    const PixelColor color = state.direction ? config.color : PixelColor::Black();
    std::fill(buffer.begin(), buffer.end(), color);
}

void PixelEffectEngine::applyBreathe(PixelChannel* channel, uint32_t tick) {
    const auto& config = channel->getEffectConfig();
    auto& buffer = channel->getPixelBuffer();
    auto& state = channel_states_[channel->getId()];

    const uint32_t interval = getEffectInterval(config.speed) / 4;

    if (tick - state.last_update_tick >= interval) {
        if (state.breathe.increasing) {
            state.breathe.brightness += 5;
            if (state.breathe.brightness >= 250) {
                state.breathe.brightness = 255;
                state.breathe.increasing = false;
            }
        } else {
            if (state.breathe.brightness <= 5) {
                state.breathe.brightness = 0;
                state.breathe.increasing = true;
            } else {
                state.breathe.brightness -= 5;
            }
        }
        state.last_update_tick = tick;
    }

    // Use gamma correction for smoother breathing
    const uint8_t gamma_brightness = gammaCorrect(state.breathe.brightness);
    const PixelColor color = config.color.scale(gamma_brightness);
    std::fill(buffer.begin(), buffer.end(), color);
}

void PixelEffectEngine::applyCyclic(PixelChannel* channel, uint32_t tick) {
    const auto& config = channel->getEffectConfig();
    auto& buffer = channel->getPixelBuffer();
    auto& state = channel_states_[channel->getId()];

    const uint32_t interval = getEffectInterval(config.speed);
    const size_t size = buffer.size();

    if (tick - state.last_update_tick >= interval) {
        state.cyclic.offset = (state.cyclic.offset + 1) % size;
        state.last_update_tick = tick;
    }

    std::fill(buffer.begin(), buffer.end(), PixelColor::Black());

    const int trail_length = std::min(5, static_cast<int>(size));
    for (int i = 0; i < trail_length; ++i) {
        const size_t idx = (state.cyclic.offset + i) % size;
        const uint8_t fade = static_cast<uint8_t>(255 - (i * 255 / trail_length));
        buffer[idx] = config.color.scale(fade);
    }
}

void PixelEffectEngine::applyRainbow(PixelChannel* channel, uint32_t tick) {
    const auto& config = channel->getEffectConfig();
    auto& buffer = channel->getPixelBuffer();
    auto& state = channel_states_[channel->getId()];

    const uint32_t interval = getEffectInterval(config.speed);
    const size_t size = buffer.size();

    if (tick - state.last_update_tick >= interval) {
        state.rainbow.offset = (state.rainbow.offset + 1) % 256;
        state.last_update_tick = tick;
    }

    for (size_t i = 0; i < size; ++i) {
        const uint8_t hue = static_cast<uint8_t>((i * 256 / size) + state.rainbow.offset);
        buffer[i] = PixelColor::fromHSV(hue, 255, config.brightness);
    }
}

void PixelEffectEngine::applyColorWipe(PixelChannel* channel, uint32_t tick) {
    const auto& config = channel->getEffectConfig();
    auto& buffer = channel->getPixelBuffer();
    auto& state = channel_states_[channel->getId()];

    const uint32_t interval = getEffectInterval(config.speed);
    const size_t size = buffer.size();

    if (tick - state.last_update_tick >= interval) {
        if (state.wipe.pixel < size) {
            state.wipe.pixel++;
        } else {
            state.wipe.clearing = !state.wipe.clearing;
            state.wipe.pixel = 0;
        }
        state.last_update_tick = tick;
    }

    const PixelColor fill = state.wipe.clearing ? PixelColor::Black() : config.color;
    const PixelColor rest = state.wipe.clearing ? config.color : PixelColor::Black();

    for (size_t i = 0; i < size; ++i) {
        buffer[i] = (i < state.wipe.pixel) ? fill : rest;
    }
}

void PixelEffectEngine::applyTheaterChase(PixelChannel* channel, uint32_t tick) {
    const auto& config = channel->getEffectConfig();
    auto& buffer = channel->getPixelBuffer();
    auto& state = channel_states_[channel->getId()];

    const uint32_t interval = getEffectInterval(config.speed);

    if (tick - state.last_update_tick >= interval) {
        state.chase.offset = (state.chase.offset + 1) % 3;
        state.last_update_tick = tick;
    }

    for (size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = ((i + state.chase.offset) % 3 == 0) ? config.color : PixelColor::Black();
    }
}

void PixelEffectEngine::applySparkle(PixelChannel* channel, uint32_t tick) {
    const auto& config = channel->getEffectConfig();
    auto& buffer = channel->getPixelBuffer();
    auto& state = channel_states_[channel->getId()];

    const uint32_t interval = getEffectInterval(config.speed) / 2;

    if (tick - state.last_update_tick >= interval) {
        std::fill(buffer.begin(), buffer.end(), PixelColor::Black());

        // Light random pixels (about 5% chance each)
        for (size_t i = 0; i < buffer.size(); ++i) {
            if ((fastRandom() % 20) == 0) {
                buffer[i] = config.color;
            }
        }
        state.last_update_tick = tick;
    }
}

// ============= NEW EFFECTS =============

void PixelEffectEngine::applyComet(PixelChannel* channel, uint32_t tick) {
    const auto& config = channel->getEffectConfig();
    auto& buffer = channel->getPixelBuffer();
    auto& state = channel_states_[channel->getId()];

    const uint32_t interval = getEffectInterval(config.speed);
    const int size = static_cast<int>(buffer.size());
    const int tail_length = std::max(3, size / 4);

    if (tick - state.last_update_tick >= interval) {
        state.comet.head++;
        if (state.comet.head >= size + tail_length) {
            state.comet.head = -tail_length;
        }
        state.last_update_tick = tick;
    }

    // Fade existing pixels
    for (auto& pixel : buffer) {
        pixel = pixel.scale(200);  // Fade by ~20%
    }

    // Draw comet head and tail
    for (int i = 0; i < tail_length; ++i) {
        const int pos = state.comet.head - i;
        if (pos >= 0 && pos < size) {
            const uint8_t brightness = static_cast<uint8_t>(255 - (i * 255 / tail_length));
            buffer[pos] = config.color.scale(brightness);
        }
    }
}

void PixelEffectEngine::applyFire(PixelChannel* channel, uint32_t tick) {
    const auto& config = channel->getEffectConfig();
    auto& buffer = channel->getPixelBuffer();
    auto& state = channel_states_[channel->getId()];

    const uint32_t interval = getEffectInterval(config.speed) / 2;
    const size_t size = buffer.size();

    if (tick - state.last_update_tick >= interval) {
        // Cool down every cell
        for (size_t i = 0; i < std::min(size, size_t(64)); ++i) {
            const uint8_t cooldown = fastRandomByte() % ((55 * 10 / size) + 2);
            state.fire.heat[i] = (state.fire.heat[i] > cooldown) ?
                state.fire.heat[i] - cooldown : 0;
        }

        // Heat rises - diffuse upward
        for (size_t i = std::min(size, size_t(64)) - 1; i >= 2; --i) {
            state.fire.heat[i] = (state.fire.heat[i - 1] +
                                  state.fire.heat[i - 2] +
                                  state.fire.heat[i - 2]) / 3;
        }

        // Randomly ignite new sparks at bottom
        if (fastRandomByte() < 120) {
            const int pos = fastRandomByte() % std::min(7, static_cast<int>(size));
            state.fire.heat[pos] = std::min(255,
                state.fire.heat[pos] + 160 + (fastRandomByte() % 96));
        }

        state.last_update_tick = tick;
    }

    // Map heat to color
    for (size_t i = 0; i < size; ++i) {
        const uint8_t heat = (i < 64) ? state.fire.heat[i] : 0;
        // Heat color: black -> red -> orange -> yellow -> white
        uint8_t r, g, b;
        if (heat < 85) {
            r = heat * 3;
            g = 0;
            b = 0;
        } else if (heat < 170) {
            r = 255;
            g = (heat - 85) * 3;
            b = 0;
        } else {
            r = 255;
            g = 255;
            b = (heat - 170) * 3;
        }
        buffer[i] = PixelColor(r, g, b).scale(config.brightness);
    }
}

void PixelEffectEngine::applyWave(PixelChannel* channel, uint32_t tick) {
    const auto& config = channel->getEffectConfig();
    auto& buffer = channel->getPixelBuffer();
    auto& state = channel_states_[channel->getId()];

    const uint32_t interval = getEffectInterval(config.speed) / 4;
    const size_t size = buffer.size();

    if (tick - state.last_update_tick >= interval) {
        state.wave.position++;
        state.last_update_tick = tick;
    }

    for (size_t i = 0; i < size; ++i) {
        // Create smooth sine wave
        const uint8_t phase = static_cast<uint8_t>(
            (i * 256 / size) + state.wave.position
        );
        const uint8_t brightness = sin_table_[phase];
        buffer[i] = config.color.scale(brightness);
    }
}

void PixelEffectEngine::applyTwinkle(PixelChannel* channel, uint32_t tick) {
    const auto& config = channel->getEffectConfig();
    auto& buffer = channel->getPixelBuffer();
    auto& state = channel_states_[channel->getId()];

    const uint32_t interval = getEffectInterval(config.speed) / 4;

    if (tick - state.last_update_tick >= interval) {
        // Fade all pixels slightly
        for (auto& pixel : buffer) {
            pixel = pixel.scale(245);
        }

        // Randomly brighten some pixels
        for (size_t i = 0; i < buffer.size(); ++i) {
            if ((fastRandom() % 50) == 0) {
                buffer[i] = config.color;
            }
        }
        state.last_update_tick = tick;
    }
}

void PixelEffectEngine::applyGradient(PixelChannel* channel, uint32_t tick) {
    const auto& config = channel->getEffectConfig();
    auto& buffer = channel->getPixelBuffer();
    auto& state = channel_states_[channel->getId()];

    const uint32_t interval = getEffectInterval(config.speed);
    const size_t size = buffer.size();

    if (tick - state.last_update_tick >= interval) {
        state.phase++;
        state.last_update_tick = tick;
    }

    // Create gradient from color to complementary color
    const PixelColor complement(
        static_cast<uint8_t>(255 - config.color.r),
        static_cast<uint8_t>(255 - config.color.g),
        static_cast<uint8_t>(255 - config.color.b)
    );

    for (size_t i = 0; i < size; ++i) {
        const uint8_t pos = static_cast<uint8_t>((i * 256 / size) + state.phase);
        const uint8_t blend_amount = sin_table_[pos];
        buffer[i] = config.color.blend(complement, blend_amount);
    }
}

void PixelEffectEngine::applyPulse(PixelChannel* channel, uint32_t tick) {
    const auto& config = channel->getEffectConfig();
    auto& buffer = channel->getPixelBuffer();
    auto& state = channel_states_[channel->getId()];

    const uint32_t interval = getEffectInterval(config.speed) / 8;
    const size_t size = buffer.size();
    const size_t center = size / 2;

    if (tick - state.last_update_tick >= interval) {
        state.phase++;
        state.last_update_tick = tick;
    }

    std::fill(buffer.begin(), buffer.end(), PixelColor::Black());

    // Create expanding pulse from center
    const uint8_t pulse_width = static_cast<uint8_t>(state.phase % (size / 2 + 10));

    for (size_t i = 0; i < size; ++i) {
        const int dist = std::abs(static_cast<int>(i) - static_cast<int>(center));
        if (dist <= pulse_width) {
            const uint8_t brightness = static_cast<uint8_t>(
                255 - (dist * 255 / (pulse_width + 1))
            );
            buffer[i] = config.color.scale(brightness);
        }
    }
}

void PixelEffectEngine::applyMeteor(PixelChannel* channel, uint32_t tick) {
    const auto& config = channel->getEffectConfig();
    auto& buffer = channel->getPixelBuffer();
    auto& state = channel_states_[channel->getId()];

    const uint32_t interval = getEffectInterval(config.speed);
    const int size = static_cast<int>(buffer.size());
    const int meteor_size = std::max(3, size / 8);

    if (tick - state.last_update_tick >= interval) {
        // Random decay of trail
        for (auto& pixel : buffer) {
            if (fastRandomByte() < 64) {
                pixel = pixel.scale(192);
            }
        }

        state.comet.head++;
        if (state.comet.head >= size * 2) {
            state.comet.head = 0;
        }

        // Draw meteor
        for (int i = 0; i < meteor_size; ++i) {
            const int pos = state.comet.head - i;
            if (pos >= 0 && pos < size) {
                const uint8_t brightness = static_cast<uint8_t>(255 - (i * 255 / meteor_size));
                buffer[pos] = config.color.scale(brightness);
            }
        }

        state.last_update_tick = tick;
    }
}

void PixelEffectEngine::applyRunningLights(PixelChannel* channel, uint32_t tick) {
    const auto& config = channel->getEffectConfig();
    auto& buffer = channel->getPixelBuffer();
    auto& state = channel_states_[channel->getId()];

    const uint32_t interval = getEffectInterval(config.speed) / 4;
    const size_t size = buffer.size();

    if (tick - state.last_update_tick >= interval) {
        state.phase++;
        state.last_update_tick = tick;
    }

    for (size_t i = 0; i < size; ++i) {
        // Create running wave pattern
        const uint8_t wave = sin_table_[(i * 32 + state.phase * 4) & 0xFF];
        buffer[i] = config.color.scale(wave);
    }
}

// ============= HELPER FUNCTIONS =============

uint32_t PixelEffectEngine::getEffectInterval(uint8_t speed) const noexcept {
    const uint32_t base_interval = update_rate_hz_ / 10;  // 100ms at 60Hz
    const uint8_t clamped_speed = std::clamp(speed, uint8_t(1), uint8_t(10));
    return base_interval * (11 - clamped_speed);
}

void PixelEffectEngine::ensureChannelState(int32_t channel_id) {
    if (channel_id >= static_cast<int32_t>(channel_states_.size())) {
        channel_states_.resize(channel_id + 1);
    }
}

uint8_t PixelEffectEngine::gammaCorrect(uint8_t value) noexcept {
    return ::gammaCorrect(value);  // Use portable version from pixel_core.h
}

void PixelEffectEngine::registerEffect(std::string_view name, std::string_view display_name, EffectFn fn) {
    effect_registry_[std::string(name)] = EffectEntry{std::move(fn), std::string(display_name)};
}

void PixelEffectEngine::unregisterEffect(std::string_view name) {
    effect_registry_.erase(std::string(name));
}

std::vector<PixelEffectEngine::EffectInfo> PixelEffectEngine::getAllEffects() const {
    std::vector<EffectInfo> effects;
    effects.reserve(effect_registry_.size());
    for (const auto& [id, entry] : effect_registry_) {
        effects.push_back({id, entry.display_name});
    }
    return effects;
}
