#include "kd_pixdriver.h"
#include "pixel_effects.h"
#include "i2s_pixel_protocol.h"
#include "esp_log.h"
#include "nvs.h"
#include "driver/i2s_std.h"
#include "driver/i2s_common.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <unordered_map>
#include <functional>

static const char* TAG = "kd_pixdriver";
static const char* NVS_NAMESPACE = "pixdriver";

// Static member definitions
std::vector<std::unique_ptr<PixelChannel>> PixelDriver::channels_;
std::unique_ptr<PixelEffectEngine> PixelDriver::effect_engine_;
int32_t PixelDriver::main_channel_id_ = -1;
TaskHandle_t PixelDriver::task_handle_ = nullptr;
int32_t PixelDriver::current_limit_ma_ = -1;
uint32_t PixelDriver::update_rate_hz_ = 60;
bool PixelDriver::running_ = false;
int32_t PixelDriver::next_channel_id_ = 0;
bool PixelDriver::initialized_ = false;

// I2S callback function
extern "C" IRAM_ATTR bool i2s_tx_callback(i2s_chan_handle_t handle, i2s_event_data_t* event, void* user_ctx) {
    PixelChannel* channel = static_cast<PixelChannel*>(user_ctx);

    if (channel && event) {
        channel->bytes_sent_ += event->size;
        if (channel->bytes_sent_ >= channel->i2s_buffer_.size()) {
            // Signal that transmission is complete
            BaseType_t higher_priority_task_woken = pdFALSE;
            xSemaphoreGiveFromISR(channel->complete_semaphore_, &higher_priority_task_woken);
        }
    }

    return false;
}

// PixelColor implementation
PixelColor PixelColor::fromHSV(uint8_t hue, uint8_t saturation, uint8_t value) {
    uint8_t r, g, b;

    if (saturation == 0) {
        return PixelColor(value, value, value, 0);
    }

    uint8_t region = hue / 43;
    uint8_t remainder = (hue - (region * 43)) * 6;

    uint8_t p = (value * (255 - saturation)) >> 8;
    uint8_t q = (value * (255 - ((saturation * remainder) >> 8))) >> 8;
    uint8_t t = (value * (255 - ((saturation * (255 - remainder)) >> 8))) >> 8;

    switch (region) {
    case 0: r = value; g = t; b = p; break;
    case 1: r = q; g = value; b = p; break;
    case 2: r = p; g = value; b = t; break;
    case 3: r = p; g = q; b = value; break;
    case 4: r = t; g = p; b = value; break;
    default: r = value; g = p; b = q; break;
    }

    return PixelColor(r, g, b, 0);
}

PixelColor PixelColor::scale(uint8_t brightness) const {
    if (brightness == 255) return *this;

    return PixelColor(
        (r * brightness) / 255,
        (g * brightness) / 255,
        (b * brightness) / 255,
        (w * brightness) / 255
    );
}

// PixelDriver implementation
void PixelDriver::initialize(uint32_t update_rate_hz) {
    if (initialized_) return;

    update_rate_hz_ = update_rate_hz;
    effect_engine_ = std::make_unique<PixelEffectEngine>(update_rate_hz);
    // register_builtin_pixel_effects(); // No longer needed, handled in PixelEffectEngine
    initialized_ = true;
    ESP_LOGI(TAG, "PixelDriver initialized with %lu Hz update rate", update_rate_hz);
}

void PixelDriver::shutdown() {
    if (!initialized_) return;

    stop();
    channels_.clear();
    effect_engine_.reset();
    main_channel_id_ = -1;
    next_channel_id_ = 0;
    initialized_ = false;
    ESP_LOGI(TAG, "PixelDriver shutdown");
}

int32_t PixelDriver::addChannel(const ChannelConfig& config) {
    if (!initialized_) {
        ESP_LOGE(TAG, "PixelDriver not initialized");
        return -1;
    }

    int32_t id = next_channel_id_++;
    auto channel = std::make_unique<PixelChannel>(id, config);

    if (!channel->initialize()) {
        ESP_LOGE(TAG, "Failed to initialize channel %ld", id);
        return -1;
    }

    // Load persisted configuration
    channel->loadFromNVS();

    // Track the first channel as main channel
    if (main_channel_id_ == -1) {
        main_channel_id_ = id;
        ESP_LOGI(TAG, "Set channel %ld as main channel", id);
    }

    channels_.emplace_back(std::move(channel));

    ESP_LOGI(TAG, "Added channel %ld: pin %d, %d pixels, %s",
        id, config.pin, config.pixel_count,
        config.format == PixelFormat::RGBW ? "RGBW" : "RGB");

    return id;
}

bool PixelDriver::removeChannel(int32_t channel_id) {
    auto it = std::find_if(channels_.begin(), channels_.end(),
        [channel_id](const auto& channel) {
            return channel->getId() == channel_id;
        });

    if (it != channels_.end()) {
        // If removing the main channel, update main channel ID
        if (main_channel_id_ == channel_id) {
            main_channel_id_ = -1;
            // Find the next available channel to be main
            for (const auto& channel : channels_) {
                if (channel->getId() != channel_id) {
                    main_channel_id_ = channel->getId();
                    ESP_LOGI(TAG, "Set channel %ld as new main channel", main_channel_id_);
                    break;
                }
            }
        }

        channels_.erase(it);
        ESP_LOGI(TAG, "Removed channel %ld", channel_id);
        return true;
    }

    return false;
}

PixelChannel* PixelDriver::getChannel(int32_t channel_id) {
    auto it = std::find_if(channels_.begin(), channels_.end(),
        [channel_id](const auto& channel) {
            return channel->getId() == channel_id;
        });

    return (it != channels_.end()) ? it->get() : nullptr;
}

PixelChannel* PixelDriver::getMainChannel() {
    if (main_channel_id_ == -1) return nullptr;
    return getChannel(main_channel_id_);
}

std::vector<int32_t> PixelDriver::getChannelIds() {
    std::vector<int32_t> ids;
    ids.reserve(channels_.size());

    for (const auto& channel : channels_) {
        ids.push_back(channel->getId());
    }

    return ids;
}

void PixelDriver::setCurrentLimit(int32_t limit_ma) {
    current_limit_ma_ = limit_ma;
    ESP_LOGI(TAG, "Current limit set to %ld mA", limit_ma);
}

int32_t PixelDriver::getCurrentLimit() {
    return current_limit_ma_;
}

void PixelDriver::setUpdateRate(uint32_t rate_hz) {
    update_rate_hz_ = rate_hz;
    effect_engine_ = std::make_unique<PixelEffectEngine>(rate_hz);
}

uint32_t PixelDriver::getUpdateRate() {
    return update_rate_hz_;
}

void PixelDriver::start() {
    if (running_ || !initialized_) return;

    running_ = true;
    xTaskCreate(driverTask, "pixdriver", 4096, nullptr, 7, &task_handle_);
    ESP_LOGI(TAG, "PixelDriver started");
}

void PixelDriver::stop() {
    if (!running_) return;

    running_ = false;
    if (task_handle_) {
        vTaskDelete(task_handle_);
        task_handle_ = nullptr;
    }
    ESP_LOGI(TAG, "PixelDriver stopped");
}

bool PixelDriver::isRunning() {
    return running_;
}

void PixelDriver::setAllChannelsEffect(const std::string& effect_id) {
    for (auto& channel : channels_) {
        EffectConfig config = channel->getEffectConfig();
        config.effect = effect_id;
        channel->setEffect(config);
    }
}

void PixelDriver::setAllChannelsColor(const PixelColor& color) {
    for (auto& channel : channels_) {
        channel->setColor(color);
    }
}

void PixelDriver::setAllChannelsBrightness(uint8_t brightness) {
    for (auto& channel : channels_) {
        channel->setBrightness(brightness);
    }
}

void PixelDriver::setAllChannelsEnabled(bool enabled) {
    for (auto& channel : channels_) {
        channel->setEnabled(enabled);
    }
}

uint32_t PixelDriver::getTotalCurrentConsumption() {
    uint32_t total = 0;
    for (const auto& channel : channels_) {
        total += channel->getCurrentConsumption();
    }
    return total;
}

uint32_t PixelDriver::getScaledCurrentConsumption() {
    float scale = getCurrentScaleFactor();
    return static_cast<uint32_t>(getTotalCurrentConsumption() * scale);
}

float PixelDriver::getCurrentScaleFactor() {
    if (current_limit_ma_ <= 0) return 1.0f;

    uint32_t total_current = getTotalCurrentConsumption();
    uint32_t available_current = (current_limit_ma_ > SYSTEM_RESERVE_MA) ?
        (current_limit_ma_ - SYSTEM_RESERVE_MA) : 0;

    if (total_current <= available_current) return 1.0f;
    if (available_current == 0) return 0.0f;

    return static_cast<float>(available_current) / total_current;
}

void PixelDriver::driverTask(void* param) {
    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t update_period = pdMS_TO_TICKS(1000 / update_rate_hz_);
    uint32_t tick = 0;

    while (running_) {
        // Update effects for all channels
        for (auto& channel : channels_) {
            if (channel->getEffectConfig().enabled) {
                effect_engine_->updateEffect(channel.get(), tick);
            }
            else {
                channel->getPixelBuffer().assign(channel->getConfig().pixel_count, PixelColor(0, 0, 0, 0));
            }
        }

        // Apply current limiting and transmit
        applyCurrentLimiting();

        for (auto& channel : channels_) {
            channel->transmit();
        }

        tick++;
        vTaskDelayUntil(&last_wake_time, update_period);
    }

    vTaskDelete(nullptr);
}

void PixelDriver::applyCurrentLimiting() {
    float scale_factor = getCurrentScaleFactor();

    for (auto& channel : channels_) {
        channel->applyCurrentScaling(scale_factor);
    }
}

// PixelChannel implementation
PixelChannel::PixelChannel(int32_t id, const ChannelConfig& config)
    : id_(id), config_(config), initialized_(false), terminate_task_(false), bytes_sent_(0) {

    pixel_buffer_.resize(config.pixel_count, PixelColor(0, 0, 0, 0));
    scaled_buffer_.resize(config.pixel_count, PixelColor(0, 0, 0, 0));

    // Initialize with default effect
    effect_config_.effect = "SOLID";
    effect_config_.color = PixelColor(100, 100, 100, 0);
    effect_config_.brightness = 255;
    effect_config_.speed = 5;
    effect_config_.enabled = true;
}

PixelChannel::~PixelChannel() {
    cleanup();
}

bool PixelChannel::initialize() {
    if (initialized_) return true;

    // Create semaphores
    transmit_semaphore_ = xSemaphoreCreateBinary();
    complete_semaphore_ = xSemaphoreCreateBinary();

    if (!transmit_semaphore_ || !complete_semaphore_) {
        ESP_LOGE(TAG, "Failed to create semaphores for channel %ld", id_);
        cleanup();
        return false;
    }

    // Setup I2S
    setupI2S();

    if (!i2s_channel_) {
        ESP_LOGE(TAG, "Failed to setup I2S for channel %ld", id_);
        cleanup();
        return false;
    }

    // Create I2S task
    char task_name[32];
    snprintf(task_name, sizeof(task_name), "i2s_ch_%ld", id_);

    if (xTaskCreate(i2sTaskWrapper, task_name, 8192, this,
        configMAX_PRIORITIES - 1, &i2s_task_handle_) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create I2S task for channel %ld", id_);
        cleanup();
        return false;
    }

    initialized_ = true;
    ESP_LOGI(TAG, "Initialized channel %ld with I2S", id_);
    return true;
}

void PixelChannel::setEffect(const EffectConfig& effect_config) {
    effect_config_ = effect_config;

    // Resize mask if provided
    if (!effect_config.mask.empty() && effect_config.mask.size() == config_.pixel_count) {
        setMask(effect_config.mask);
    }
}

void PixelChannel::setColor(const PixelColor& color) {
    effect_config_.color = color;
}

void PixelChannel::setBrightness(uint8_t brightness) {
    effect_config_.brightness = brightness;
}

void PixelChannel::setSpeed(uint8_t speed) {
    effect_config_.speed = std::clamp(speed, uint8_t(1), uint8_t(10));
}

void PixelChannel::setEnabled(bool enabled) {
    effect_config_.enabled = enabled;
}

void PixelChannel::setMask(const std::vector<uint8_t>& mask) {
    if (mask.size() == config_.pixel_count) {
        effect_config_.mask = mask;
    }
}

void PixelChannel::clearMask() {
    effect_config_.mask.clear();
}

void PixelChannel::setupI2S() {
    // Create I2S TX channel
    i2s_chan_config_t chan_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);

    i2s_std_config_t std_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(WS2812B_BITRATE / 16 / 2), // 16-bit, 2 channels per slot
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_GPIO_UNUSED,
            .ws = I2S_GPIO_UNUSED,
            .dout = config_.pin,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    esp_err_t ret = i2s_new_channel(&chan_config, &i2s_channel_, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel for pin %d: %s", config_.pin, esp_err_to_name(ret));
        return;
    }

    ret = i2s_channel_init_std_mode(i2s_channel_, &std_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S std mode: %s", esp_err_to_name(ret));
        i2s_del_channel(i2s_channel_);
        i2s_channel_ = nullptr;
        return;
    }

    // Register I2S event callbacks
    i2s_event_callbacks_t callbacks = {
        .on_recv = nullptr,
        .on_recv_q_ovf = nullptr,
        .on_sent = i2s_tx_callback,
        .on_send_q_ovf = nullptr,
    };

    ret = i2s_channel_register_event_callback(i2s_channel_, &callbacks, this);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register I2S callbacks: %s", esp_err_to_name(ret));
        i2s_del_channel(i2s_channel_);
        i2s_channel_ = nullptr;
        return;
    }
}

void PixelChannel::cleanup() {
    if (initialized_) {
        // Signal task to terminate
        terminate_task_ = true;
        if (transmit_semaphore_) {
            xSemaphoreGive(transmit_semaphore_);
        }

        // Wait for task to terminate
        if (i2s_task_handle_) {
            for (int i = 0; i < 100 && terminate_task_; ++i) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            if (terminate_task_) {
                ESP_LOGW(TAG, "I2S task did not terminate gracefully for channel %ld", id_);
                vTaskDelete(i2s_task_handle_);
            }
            i2s_task_handle_ = nullptr;
        }

        // Clean up I2S
        if (i2s_channel_) {
            i2s_del_channel(i2s_channel_);
            i2s_channel_ = nullptr;
        }

        // Clean up semaphores
        if (transmit_semaphore_) {
            vSemaphoreDelete(transmit_semaphore_);
            transmit_semaphore_ = nullptr;
        }
        if (complete_semaphore_) {
            vSemaphoreDelete(complete_semaphore_);
            complete_semaphore_ = nullptr;
        }

        initialized_ = false;
        terminate_task_ = false;
    }
}

std::vector<uint8_t> PixelChannel::convertToI2SBuffer(const std::vector<PixelColor>& pixels) {
    size_t bytes_per_pixel = (config_.format == PixelFormat::RGBW) ? WS2812B_BYTES_PER_RGBW : WS2812B_BYTES_PER_RGB;
    size_t buffer_size = (pixels.size() * bytes_per_pixel) + WS2812B_RESET_BYTES;

    i2s_buffer_.resize(buffer_size);
    memset(i2s_buffer_.data(), 0, buffer_size); // Initialize reset bytes

    for (size_t i = 0; i < pixels.size(); ++i) {
        const auto& pixel = pixels[i];
        size_t base_idx = i * bytes_per_pixel;

        // Apply mask if present
        bool masked = effect_config_.mask.empty() ||
            (i < effect_config_.mask.size() && effect_config_.mask[i]);

        uint8_t g_val = masked ? pixel.g : 0;
        uint8_t r_val = masked ? pixel.r : 0;
        uint8_t b_val = masked ? pixel.b : 0;
        uint8_t w_val = masked ? pixel.w : 0;

        // Convert colors using lookup table - GRB order for WS2812
        const uint8_t* g_sequence = ws2812b_color_lookup[g_val];
        const uint8_t* r_sequence = ws2812b_color_lookup[r_val];
        const uint8_t* b_sequence = ws2812b_color_lookup[b_val];

        // Copy G, R, B sequences with proper byte ordering for I2S
        for (int j = 0; j < WS2812B_BYTES_PER_COLOR; ++j) {
            i2s_buffer_[(base_idx + j) ^ 1] = g_sequence[j];
            i2s_buffer_[(base_idx + WS2812B_BYTES_PER_COLOR + j) ^ 1] = r_sequence[j];
            i2s_buffer_[(base_idx + 2 * WS2812B_BYTES_PER_COLOR + j) ^ 1] = b_sequence[j];
        }

        // Add white channel for RGBW
        if (config_.format == PixelFormat::RGBW) {
            const uint8_t* w_sequence = ws2812b_color_lookup[w_val];
            for (int j = 0; j < WS2812B_BYTES_PER_COLOR; ++j) {
                i2s_buffer_[(base_idx + 3 * WS2812B_BYTES_PER_COLOR + j) ^ 1] = w_sequence[j];
            }
        }
    }

    return i2s_buffer_;
}

void PixelChannel::transmit() {
    if (!initialized_) return;

    convertToI2SBuffer(scaled_buffer_);

    // Signal I2S task to transmit
    if (transmit_semaphore_) {
        xSemaphoreGive(transmit_semaphore_);
    }
}

uint32_t PixelChannel::getCurrentConsumption() const {
    uint32_t total_ma = 0;

    for (const auto& pixel : pixel_buffer_) {
        // Each color channel at 255 brightness consumes 20mA
        total_ma += (pixel.r * PixelDriver::CURRENT_PER_CHANNEL_MA) / 255;
        total_ma += (pixel.g * PixelDriver::CURRENT_PER_CHANNEL_MA) / 255;
        total_ma += (pixel.b * PixelDriver::CURRENT_PER_CHANNEL_MA) / 255;
        if (config_.format == PixelFormat::RGBW) {
            total_ma += (pixel.w * PixelDriver::CURRENT_PER_CHANNEL_MA) / 255;
        }
    }

    return total_ma;
}

void PixelChannel::applyCurrentScaling(float scale_factor) {
    // First, copy pixel_buffer_ to scaled_buffer_ with brightness scaling applied
    float brightness_scale = effect_config_.brightness / 255.0f;

    for (size_t i = 0; i < pixel_buffer_.size(); ++i) {
        const auto& original = pixel_buffer_[i];
        scaled_buffer_[i] = PixelColor(
            static_cast<uint8_t>(original.r * brightness_scale),
            static_cast<uint8_t>(original.g * brightness_scale),
            static_cast<uint8_t>(original.b * brightness_scale),
            static_cast<uint8_t>(original.w * brightness_scale)
        );
    }

    // Then, apply current limiting if needed
    if (scale_factor < 1.0f) {
        for (auto& pixel : scaled_buffer_) {
            pixel = PixelColor(
                static_cast<uint8_t>(pixel.r * scale_factor),
                static_cast<uint8_t>(pixel.g * scale_factor),
                static_cast<uint8_t>(pixel.b * scale_factor),
                static_cast<uint8_t>(pixel.w * scale_factor)
            );
        }
    }
}

void PixelChannel::saveToNVS() const {
    nvs_handle_t nvs_handle;
    char key[32];
    snprintf(key, sizeof(key), "ch_%ld", id_);

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for channel %ld: %s", id_, esp_err_to_name(err));
        return;
    }

    // Save effect type as string
    err = nvs_set_str(nvs_handle, (std::string(key) + ":effect").c_str(), effect_config_.effect.c_str());
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save effect type for channel %ld: %s", id_, esp_err_to_name(err));
    }
    // Save other fields as needed (color, brightness, speed, enabled, etc.)
    err = nvs_set_blob(nvs_handle, (std::string(key) + ":color").c_str(), &effect_config_.color, sizeof(PixelColor));
    nvs_set_u8(nvs_handle, (std::string(key) + ":brightness").c_str(), effect_config_.brightness);
    nvs_set_u8(nvs_handle, (std::string(key) + ":speed").c_str(), effect_config_.speed);
    nvs_set_u8(nvs_handle, (std::string(key) + ":enabled").c_str(), effect_config_.enabled ? 1 : 0);
    // Mask and other fields can be added similarly if needed

    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
}

void PixelChannel::loadFromNVS() {
    nvs_handle_t nvs_handle;
    char key[32];
    snprintf(key, sizeof(key), "ch_%ld", id_);

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved config for channel %ld, using defaults", id_);
        return;
    }

    // Load effect type as string
    char effect_str[32] = { 0 };
    size_t effect_str_len = sizeof(effect_str);
    if (nvs_get_str(nvs_handle, (std::string(key) + ":effect").c_str(), effect_str, &effect_str_len) == ESP_OK) {
        effect_config_.effect = effect_str;
    }
    // Load other fields as needed
    PixelColor color;
    size_t color_size = sizeof(PixelColor);
    if (nvs_get_blob(nvs_handle, (std::string(key) + ":color").c_str(), &color, &color_size) == ESP_OK) {
        effect_config_.color = color;
    }
    uint8_t val = 0;
    if (nvs_get_u8(nvs_handle, (std::string(key) + ":brightness").c_str(), &val) == ESP_OK) effect_config_.brightness = val;
    if (nvs_get_u8(nvs_handle, (std::string(key) + ":speed").c_str(), &val) == ESP_OK) effect_config_.speed = val;
    if (nvs_get_u8(nvs_handle, (std::string(key) + ":enabled").c_str(), &val) == ESP_OK) effect_config_.enabled = (val != 0);
    // Mask and other fields can be added similarly if needed

    nvs_close(nvs_handle);
}

void PixelChannel::i2sTaskWrapper(void* param) {
    static_cast<PixelChannel*>(param)->i2sTask();
}

void PixelChannel::i2sTask() {
    std::vector<uint8_t> local_buffer;
    size_t bytes_written;

    ESP_LOGD(TAG, "I2S task started for channel %ld", id_);

    while (!terminate_task_) {
        // Wait for transmission request
        if (xSemaphoreTake(transmit_semaphore_, portMAX_DELAY) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (terminate_task_) {
            break;
        }

        // Make a local copy of the buffer
        local_buffer = i2s_buffer_;
        bytes_sent_ = 0;

        // Preload and enable I2S
        esp_err_t ret = i2s_channel_preload_data(i2s_channel_, local_buffer.data(),
            local_buffer.size(), &bytes_written);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to preload I2S data: %s", esp_err_to_name(ret));
            continue;
        }

        ret = i2s_channel_enable(i2s_channel_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(ret));
            continue;
        }

        // Write remaining data if any
        if (bytes_written < local_buffer.size()) {
            ret = i2s_channel_write(i2s_channel_,
                &local_buffer[bytes_written],
                local_buffer.size() - bytes_written,
                &bytes_written,
                pdMS_TO_TICKS(1000));
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "I2S write timeout or error: %s", esp_err_to_name(ret));
            }
        }

        // Wait for transmission to complete
        xSemaphoreTake(complete_semaphore_, portMAX_DELAY);

        // Disable I2S channel
        i2s_channel_disable(i2s_channel_);
    }

    ESP_LOGD(TAG, "I2S task finished for channel %ld", id_);
    terminate_task_ = false;
    vTaskDelete(nullptr);
}
