#include "pixel_preview.h"
#include "pixel_platform.h"
#include <algorithm>
#include <cmath>
#include <cctype>

namespace {

inline bool equalsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(a[i])) !=
            std::toupper(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

inline uint32_t fastRandom() {
    return pixel_random();
}

inline uint8_t fastRandomByte() {
    return pixel_random_byte();
}

} // anonymous namespace

PixelPreview::PixelPreview(uint16_t led_count, bool is_rgbw, uint32_t update_rate_hz)
    : led_count_(led_count)
    , is_rgbw_(is_rgbw)
    , update_rate_hz_(update_rate_hz)
    , current_effect_("SOLID")
    , color_(100, 100, 100, 0)
    , brightness_(255)
    , speed_(5)
    , tick_(0)
    , state_()
    , heat_map_(std::max(64, static_cast<int>(led_count)))
    , buffer_(led_count)
    , output_rgba_(led_count * 4) {
}

void PixelPreview::setEffect(const std::string& effect_id) {
    if (!equalsIgnoreCase(current_effect_, effect_id)) {
        current_effect_ = effect_id;
        // Reset state when effect changes
        state_ = EffectState();
        std::fill(heat_map_.begin(), heat_map_.end(), 0);
    }
}

void PixelPreview::setColor(uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
    color_ = PixelColor(r, g, b, w);
}

void PixelPreview::setBrightness(uint8_t brightness) {
    brightness_ = brightness;
}

void PixelPreview::setSpeed(uint8_t speed) {
    speed_ = std::clamp(speed, uint8_t(1), uint8_t(10));
}

void PixelPreview::tick() {
    // Dispatch to appropriate effect
    if (equalsIgnoreCase(current_effect_, "SOLID")) {
        applySolid();
    } else if (equalsIgnoreCase(current_effect_, "BLINK")) {
        applyBlink();
    } else if (equalsIgnoreCase(current_effect_, "BREATHE")) {
        applyBreathe();
    } else if (equalsIgnoreCase(current_effect_, "CYCLIC")) {
        applyCyclic();
    } else if (equalsIgnoreCase(current_effect_, "RAINBOW")) {
        applyRainbow();
    } else if (equalsIgnoreCase(current_effect_, "COLOR_WIPE")) {
        applyColorWipe();
    } else if (equalsIgnoreCase(current_effect_, "THEATER_CHASE")) {
        applyTheaterChase();
    } else if (equalsIgnoreCase(current_effect_, "SPARKLE")) {
        applySparkle();
    } else if (equalsIgnoreCase(current_effect_, "COMET")) {
        applyComet();
    } else if (equalsIgnoreCase(current_effect_, "FIRE")) {
        applyFire();
    } else if (equalsIgnoreCase(current_effect_, "WAVE")) {
        applyWave();
    } else if (equalsIgnoreCase(current_effect_, "TWINKLE")) {
        applyTwinkle();
    } else if (equalsIgnoreCase(current_effect_, "GRADIENT")) {
        applyGradient();
    } else if (equalsIgnoreCase(current_effect_, "PULSE")) {
        applyPulse();
    } else if (equalsIgnoreCase(current_effect_, "METEOR")) {
        applyMeteor();
    } else if (equalsIgnoreCase(current_effect_, "RUNNING_LIGHTS")) {
        applyRunningLights();
    } else {
        // Default to solid
        applySolid();
    }

    tick_++;
}

void PixelPreview::reset() {
    tick_ = 0;
    state_ = EffectState();
    std::fill(buffer_.begin(), buffer_.end(), PixelColor::Black());
    std::fill(heat_map_.begin(), heat_map_.end(), 0);
}

void PixelPreview::setRandomSeed(uint32_t seed) {
    pixel_set_random_seed(seed);
}

const uint8_t* PixelPreview::getFrameData() const {
    // Convert buffer to RGBA format
    for (size_t i = 0; i < buffer_.size(); ++i) {
        const auto& pixel = buffer_[i];
        output_rgba_[i * 4 + 0] = pixel.r;
        output_rgba_[i * 4 + 1] = pixel.g;
        output_rgba_[i * 4 + 2] = pixel.b;
        output_rgba_[i * 4 + 3] = is_rgbw_ ? pixel.w : 255;  // Alpha or W channel
    }
    return output_rgba_.data();
}

size_t PixelPreview::getFrameSize() const {
    return output_rgba_.size();
}

std::vector<std::string> PixelPreview::getEffectList() {
    return {
        "SOLID", "BLINK", "BREATHE", "CYCLIC", "RAINBOW",
        "COLOR_WIPE", "THEATER_CHASE", "SPARKLE", "COMET",
        "FIRE", "WAVE", "TWINKLE", "GRADIENT", "PULSE",
        "METEOR", "RUNNING_LIGHTS"
    };
}

uint32_t PixelPreview::getEffectInterval(uint8_t speed) const {
    const uint32_t base_interval = update_rate_hz_ / 10;  // 100ms at 60Hz
    const uint8_t clamped_speed = std::clamp(speed, uint8_t(1), uint8_t(10));
    return base_interval * (11 - clamped_speed);
}

// ============= Effect Implementations =============

void PixelPreview::applySolid() {
    std::fill(buffer_.begin(), buffer_.end(), color_);
}

void PixelPreview::applyBlink() {
    const uint32_t interval = getEffectInterval(speed_);

    if (tick_ - state_.last_update_tick >= interval) {
        state_.direction = !state_.direction;
        state_.last_update_tick = tick_;
    }

    const PixelColor color = state_.direction ? color_ : PixelColor::Black();
    std::fill(buffer_.begin(), buffer_.end(), color);
}

void PixelPreview::applyBreathe() {
    const uint32_t interval = getEffectInterval(speed_) / 4;

    if (tick_ - state_.last_update_tick >= interval) {
        if (state_.breathe.increasing) {
            state_.breathe.brightness += 5;
            if (state_.breathe.brightness >= 250) {
                state_.breathe.brightness = 255;
                state_.breathe.increasing = false;
            }
        } else {
            if (state_.breathe.brightness <= 5) {
                state_.breathe.brightness = 0;
                state_.breathe.increasing = true;
            } else {
                state_.breathe.brightness -= 5;
            }
        }
        state_.last_update_tick = tick_;
    }

    const uint8_t gamma_brightness = gammaCorrect(state_.breathe.brightness);
    const PixelColor color = color_.scale(gamma_brightness);
    std::fill(buffer_.begin(), buffer_.end(), color);
}

void PixelPreview::applyCyclic() {
    const uint32_t interval = getEffectInterval(speed_);
    const size_t size = buffer_.size();

    if (tick_ - state_.last_update_tick >= interval) {
        state_.cyclic.offset = (state_.cyclic.offset + 1) % size;
        state_.last_update_tick = tick_;
    }

    std::fill(buffer_.begin(), buffer_.end(), PixelColor::Black());

    const int trail_length = std::min(5, static_cast<int>(size));
    for (int i = 0; i < trail_length; ++i) {
        const size_t idx = (state_.cyclic.offset + i) % size;
        const uint8_t fade = static_cast<uint8_t>(255 - (i * 255 / trail_length));
        buffer_[idx] = color_.scale(fade);
    }
}

void PixelPreview::applyRainbow() {
    const uint32_t interval = getEffectInterval(speed_);
    const size_t size = buffer_.size();

    if (tick_ - state_.last_update_tick >= interval) {
        state_.rainbow.offset = (state_.rainbow.offset + 1) % 256;
        state_.last_update_tick = tick_;
    }

    for (size_t i = 0; i < size; ++i) {
        const uint8_t hue = static_cast<uint8_t>((i * 256 / size) + state_.rainbow.offset);
        buffer_[i] = PixelColor::fromHSV(hue, 255, brightness_);
    }
}

void PixelPreview::applyColorWipe() {
    const uint32_t interval = getEffectInterval(speed_);
    const size_t size = buffer_.size();

    if (tick_ - state_.last_update_tick >= interval) {
        if (state_.wipe.pixel < size) {
            state_.wipe.pixel++;
        } else {
            state_.wipe.clearing = !state_.wipe.clearing;
            state_.wipe.pixel = 0;
        }
        state_.last_update_tick = tick_;
    }

    const PixelColor fill = state_.wipe.clearing ? PixelColor::Black() : color_;
    const PixelColor rest = state_.wipe.clearing ? color_ : PixelColor::Black();

    for (size_t i = 0; i < size; ++i) {
        buffer_[i] = (i < state_.wipe.pixel) ? fill : rest;
    }
}

void PixelPreview::applyTheaterChase() {
    const uint32_t interval = getEffectInterval(speed_);

    if (tick_ - state_.last_update_tick >= interval) {
        state_.chase.offset = (state_.chase.offset + 1) % 3;
        state_.last_update_tick = tick_;
    }

    for (size_t i = 0; i < buffer_.size(); ++i) {
        buffer_[i] = ((i + state_.chase.offset) % 3 == 0) ? color_ : PixelColor::Black();
    }
}

void PixelPreview::applySparkle() {
    const uint32_t interval = getEffectInterval(speed_) / 2;

    if (tick_ - state_.last_update_tick >= interval) {
        std::fill(buffer_.begin(), buffer_.end(), PixelColor::Black());

        for (size_t i = 0; i < buffer_.size(); ++i) {
            if ((fastRandom() % 20) == 0) {
                buffer_[i] = color_;
            }
        }
        state_.last_update_tick = tick_;
    }
}

void PixelPreview::applyComet() {
    const uint32_t interval = getEffectInterval(speed_);
    const int size = static_cast<int>(buffer_.size());
    const int tail_length = std::max(3, size / 4);

    if (tick_ - state_.last_update_tick >= interval) {
        state_.comet.head++;
        if (state_.comet.head >= size + tail_length) {
            state_.comet.head = -tail_length;
        }
        state_.last_update_tick = tick_;
    }

    // Fade existing pixels
    for (auto& pixel : buffer_) {
        pixel = pixel.scale(200);
    }

    // Draw comet head and tail
    for (int i = 0; i < tail_length; ++i) {
        const int pos = state_.comet.head - i;
        if (pos >= 0 && pos < size) {
            const uint8_t brightness = static_cast<uint8_t>(255 - (i * 255 / tail_length));
            buffer_[pos] = color_.scale(brightness);
        }
    }
}

void PixelPreview::applyFire() {
    const uint32_t interval = getEffectInterval(speed_) / 2;
    const size_t size = buffer_.size();
    const size_t heat_size = heat_map_.size();

    if (tick_ - state_.last_update_tick >= interval) {
        // Cool down every cell
        for (size_t i = 0; i < heat_size; ++i) {
            const uint8_t cooldown = fastRandomByte() % ((55 * 10 / std::max(size, size_t(1))) + 2);
            heat_map_[i] = (heat_map_[i] > cooldown) ? heat_map_[i] - cooldown : 0;
        }

        // Heat rises - diffuse upward
        for (size_t i = heat_size - 1; i >= 2; --i) {
            heat_map_[i] = (heat_map_[i - 1] + heat_map_[i - 2] + heat_map_[i - 2]) / 3;
        }

        // Randomly ignite new sparks at bottom
        if (fastRandomByte() < 120) {
            const int pos = fastRandomByte() % std::min(7, static_cast<int>(heat_size));
            heat_map_[pos] = std::min(255, heat_map_[pos] + 160 + (fastRandomByte() % 96));
        }

        state_.last_update_tick = tick_;
    }

    // Map heat to color
    for (size_t i = 0; i < size; ++i) {
        const uint8_t heat = (i < heat_size) ? heat_map_[i] : 0;
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
        buffer_[i] = PixelColor(r, g, b).scale(brightness_);
    }
}

void PixelPreview::applyWave() {
    const uint32_t interval = getEffectInterval(speed_) / 4;
    const size_t size = buffer_.size();

    if (tick_ - state_.last_update_tick >= interval) {
        state_.wave.position++;
        state_.last_update_tick = tick_;
    }

    for (size_t i = 0; i < size; ++i) {
        const uint8_t phase = static_cast<uint8_t>((i * 256 / size) + state_.wave.position);
        const uint8_t brightness = SIN_TABLE[phase];
        buffer_[i] = color_.scale(brightness);
    }
}

void PixelPreview::applyTwinkle() {
    const uint32_t interval = getEffectInterval(speed_) / 4;

    if (tick_ - state_.last_update_tick >= interval) {
        for (auto& pixel : buffer_) {
            pixel = pixel.scale(245);
        }

        for (size_t i = 0; i < buffer_.size(); ++i) {
            if ((fastRandom() % 50) == 0) {
                buffer_[i] = color_;
            }
        }
        state_.last_update_tick = tick_;
    }
}

void PixelPreview::applyGradient() {
    const uint32_t interval = getEffectInterval(speed_);
    const size_t size = buffer_.size();

    if (tick_ - state_.last_update_tick >= interval) {
        state_.phase++;
        state_.last_update_tick = tick_;
    }

    const PixelColor complement(
        static_cast<uint8_t>(255 - color_.r),
        static_cast<uint8_t>(255 - color_.g),
        static_cast<uint8_t>(255 - color_.b)
    );

    for (size_t i = 0; i < size; ++i) {
        const uint8_t pos = static_cast<uint8_t>((i * 256 / size) + state_.phase);
        const uint8_t blend_amount = SIN_TABLE[pos];
        buffer_[i] = color_.blend(complement, blend_amount);
    }
}

void PixelPreview::applyPulse() {
    const uint32_t interval = getEffectInterval(speed_) / 8;
    const size_t size = buffer_.size();
    const size_t center = size / 2;

    if (tick_ - state_.last_update_tick >= interval) {
        state_.phase++;
        state_.last_update_tick = tick_;
    }

    std::fill(buffer_.begin(), buffer_.end(), PixelColor::Black());

    const uint8_t pulse_width = static_cast<uint8_t>(state_.phase % (size / 2 + 10));

    for (size_t i = 0; i < size; ++i) {
        const int dist = std::abs(static_cast<int>(i) - static_cast<int>(center));
        if (dist <= pulse_width) {
            const uint8_t brightness = static_cast<uint8_t>(255 - (dist * 255 / (pulse_width + 1)));
            buffer_[i] = color_.scale(brightness);
        }
    }
}

void PixelPreview::applyMeteor() {
    const uint32_t interval = getEffectInterval(speed_);
    const int size = static_cast<int>(buffer_.size());
    const int meteor_size = std::max(3, size / 8);

    if (tick_ - state_.last_update_tick >= interval) {
        for (auto& pixel : buffer_) {
            if (fastRandomByte() < 64) {
                pixel = pixel.scale(192);
            }
        }

        state_.comet.head++;
        if (state_.comet.head >= size * 2) {
            state_.comet.head = 0;
        }

        for (int i = 0; i < meteor_size; ++i) {
            const int pos = state_.comet.head - i;
            if (pos >= 0 && pos < size) {
                const uint8_t brightness = static_cast<uint8_t>(255 - (i * 255 / meteor_size));
                buffer_[pos] = color_.scale(brightness);
            }
        }

        state_.last_update_tick = tick_;
    }
}

void PixelPreview::applyRunningLights() {
    const uint32_t interval = getEffectInterval(speed_) / 4;
    const size_t size = buffer_.size();

    if (tick_ - state_.last_update_tick >= interval) {
        state_.phase++;
        state_.last_update_tick = tick_;
    }

    for (size_t i = 0; i < size; ++i) {
        const uint8_t wave = SIN_TABLE[(i * 32 + state_.phase * 4) & 0xFF];
        buffer_[i] = color_.scale(wave);
    }
}
