#include "kd_pixdriver.h"
#include "pixel_effects.h"
#include "pixel_version.h"
#include "i2s_pixel_protocol.h"
#include "esp_log.h"
#include "nvs.h"
#include "cJSON.h"
#include "driver/i2s_std.h"
#include "driver/i2s_common.h"
#include <algorithm>
#include <cstring>

namespace {
constexpr const char* TAG = "kd_pixdriver";
constexpr const char* NVS_NAMESPACE = "pixdriver";
} // anonymous namespace

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
extern "C" IRAM_ATTR bool i2s_tx_callback(i2s_chan_handle_t handle,
                                           i2s_event_data_t* event,
                                           void* user_ctx) {
    auto* channel = static_cast<PixelChannel*>(user_ctx);
    if (channel && event) {
        channel->bytes_sent_ += event->size;
        if (channel->bytes_sent_ >= channel->i2s_buffer_.size()) {
            BaseType_t higher_priority_task_woken = pdFALSE;
            xSemaphoreGiveFromISR(channel->complete_semaphore_, &higher_priority_task_woken);
        }
    }
    return false;
}

// ============= PixelDriver Implementation =============

void PixelDriver::initialize(uint32_t update_rate_hz) {
    if (initialized_) return;

    update_rate_hz_ = update_rate_hz;
    effect_engine_ = std::make_unique<PixelEffectEngine>(update_rate_hz);
    initialized_ = true;
    ESP_LOGI(TAG, "PixelDriver initialized at %lu Hz", update_rate_hz);
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

    const int32_t id = next_channel_id_++;
    auto channel = std::make_unique<PixelChannel>(id, config);

    if (!channel->initialize()) {
        ESP_LOGE(TAG, "Failed to initialize channel %ld", id);
        return -1;
    }

    channel->loadFromNVS();

    if (main_channel_id_ == -1) {
        main_channel_id_ = id;
        ESP_LOGI(TAG, "Set channel %ld as main", id);
    }

    ESP_LOGI(TAG, "Added channel %ld: pin %d, %d pixels, %s",
             id, config.pin, config.pixel_count,
             config.format == PixelFormat::RGBW ? "RGBW" : "RGB");

    channels_.emplace_back(std::move(channel));
    return id;
}

bool PixelDriver::removeChannel(int32_t channel_id) {
    auto it = std::find_if(channels_.begin(), channels_.end(),
        [channel_id](const auto& ch) { return ch->getId() == channel_id; });

    if (it == channels_.end()) return false;

    if (main_channel_id_ == channel_id) {
        main_channel_id_ = -1;
        for (const auto& ch : channels_) {
            if (ch->getId() != channel_id) {
                main_channel_id_ = ch->getId();
                ESP_LOGI(TAG, "Set channel %ld as new main", main_channel_id_);
                break;
            }
        }
    }

    channels_.erase(it);
    ESP_LOGI(TAG, "Removed channel %ld", channel_id);
    return true;
}

PixelChannel* PixelDriver::getChannel(int32_t channel_id) {
    auto it = std::find_if(channels_.begin(), channels_.end(),
        [channel_id](const auto& ch) { return ch->getId() == channel_id; });
    return (it != channels_.end()) ? it->get() : nullptr;
}

PixelChannel* PixelDriver::getMainChannel() {
    return (main_channel_id_ != -1) ? getChannel(main_channel_id_) : nullptr;
}

std::vector<int32_t> PixelDriver::getChannelIds() {
    std::vector<int32_t> ids;
    ids.reserve(channels_.size());
    for (const auto& ch : channels_) {
        ids.push_back(ch->getId());
    }
    return ids;
}

void PixelDriver::setCurrentLimit(int32_t limit_ma) {
    current_limit_ma_ = limit_ma;
    ESP_LOGI(TAG, "Current limit: %ld mA", limit_ma);
}

int32_t PixelDriver::getCurrentLimit() noexcept {
    return current_limit_ma_;
}

void PixelDriver::setUpdateRate(uint32_t rate_hz) {
    update_rate_hz_ = rate_hz;
    effect_engine_ = std::make_unique<PixelEffectEngine>(rate_hz);
}

uint32_t PixelDriver::getUpdateRate() noexcept {
    return update_rate_hz_;
}

void PixelDriver::start() {
    if (running_ || !initialized_) return;

    running_ = true;
    xTaskCreate(driverTask, "pixdriver", 3072, nullptr, 7, &task_handle_);
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

bool PixelDriver::isRunning() noexcept {
    return running_;
}

void PixelDriver::setAllChannelsEffect(std::string_view effect_id) {
    for (auto& ch : channels_) {
        ch->setEffectByID(effect_id);
    }
}

void PixelDriver::setAllChannelsColor(const PixelColor& color) {
    for (auto& ch : channels_) {
        ch->setColor(color);
    }
}

void PixelDriver::setAllChannelsBrightness(uint8_t brightness) {
    for (auto& ch : channels_) {
        ch->setBrightness(brightness);
    }
}

void PixelDriver::setAllChannelsEnabled(bool enabled) {
    for (auto& ch : channels_) {
        ch->setEnabled(enabled);
    }
}

uint32_t PixelDriver::getTotalCurrentConsumption() {
    uint32_t total = 0;
    for (const auto& ch : channels_) {
        total += ch->getCurrentConsumption();
    }
    return total;
}

uint32_t PixelDriver::getScaledCurrentConsumption() {
    return static_cast<uint32_t>(getTotalCurrentConsumption() * getCurrentScaleFactor());
}

float PixelDriver::getCurrentScaleFactor() {
    if (current_limit_ma_ <= 0) return 1.0f;

    const uint32_t total = getTotalCurrentConsumption();
    const uint32_t available = (current_limit_ma_ > static_cast<int32_t>(SYSTEM_RESERVE_MA))
        ? (current_limit_ma_ - SYSTEM_RESERVE_MA) : 0;

    if (total <= available) return 1.0f;
    if (available == 0) return 0.0f;

    return static_cast<float>(available) / static_cast<float>(total);
}

void PixelDriver::driverTask(void* param) {
    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t update_period = pdMS_TO_TICKS(1000 / update_rate_hz_);
    uint32_t tick = 0;

    while (running_) {
        // Update effects
        for (auto& ch : channels_) {
            if (ch->getEffectConfig().enabled) {
                effect_engine_->updateEffect(ch.get(), tick);
            } else {
                auto& buffer = ch->getPixelBuffer();
                std::fill(buffer.begin(), buffer.end(), PixelColor::Black());
            }
        }

        // Apply current limiting and transmit
        applyCurrentLimiting();

        for (auto& ch : channels_) {
            ch->transmit();
        }

        tick++;
        vTaskDelayUntil(&last_wake_time, update_period);
    }

    vTaskDelete(nullptr);
}

void PixelDriver::applyCurrentLimiting() {
    const float scale = getCurrentScaleFactor();
    for (auto& ch : channels_) {
        ch->applyCurrentScaling(scale);
    }
}

// ============= PixelChannel Implementation =============

PixelChannel::PixelChannel(int32_t id, const ChannelConfig& config)
    : id_(id)
    , config_(config)
    , initialized_(false)
    , terminate_task_(false)
    , bytes_sent_(0) {

    // Pre-allocate all buffers
    pixel_buffer_.resize(config.pixel_count, PixelColor::Black());
    scaled_buffer_.resize(config.pixel_count, PixelColor::Black());

    const size_t bytes_per_pixel = (config.format == PixelFormat::RGBW)
        ? WS2812B_BYTES_PER_RGBW : WS2812B_BYTES_PER_RGB;
    const size_t buffer_size = (config.pixel_count * bytes_per_pixel) + WS2812B_RESET_BYTES;
    i2s_buffer_.resize(buffer_size, 0);

    // Default effect
    effect_config_.effect = "SOLID";
    effect_config_.color = PixelColor(100, 100, 100);
    effect_config_.brightness = 255;
    effect_config_.speed = 5;
    effect_config_.enabled = true;
}

PixelChannel::~PixelChannel() {
    cleanup();
}

bool PixelChannel::initialize() {
    if (initialized_) return true;

    transmit_semaphore_ = xSemaphoreCreateBinary();
    complete_semaphore_ = xSemaphoreCreateBinary();

    if (!transmit_semaphore_ || !complete_semaphore_) {
        ESP_LOGE(TAG, "Failed to create semaphores for channel %ld", id_);
        cleanup();
        return false;
    }

    setupI2S();

    if (!i2s_channel_) {
        ESP_LOGE(TAG, "Failed to setup I2S for channel %ld", id_);
        cleanup();
        return false;
    }

    char task_name[16];
    snprintf(task_name, sizeof(task_name), "i2s_%ld", id_);

    if (xTaskCreate(i2sTaskWrapper, task_name, 3072, this,
                    configMAX_PRIORITIES - 1, &i2s_task_handle_) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create I2S task for channel %ld", id_);
        cleanup();
        return false;
    }

    initialized_ = true;
    ESP_LOGI(TAG, "Channel %ld initialized", id_);
    return true;
}

void PixelChannel::setEffect(const EffectConfig& config) {
    effect_config_ = config;
    if (!config.mask.empty() && config.mask.size() == config_.pixel_count) {
        setMask(config.mask);
    }
}

void PixelChannel::setEffectByID(std::string_view effect_id) {
    effect_config_.effect = std::string(effect_id);
}

void PixelChannel::setColor(const PixelColor& color) noexcept {
    effect_config_.color = color;
}

void PixelChannel::setBrightness(uint8_t brightness) noexcept {
    effect_config_.brightness = brightness;
}

void PixelChannel::setSpeed(uint8_t speed) noexcept {
    effect_config_.speed = std::clamp(speed, uint8_t(1), uint8_t(10));
}

void PixelChannel::setEnabled(bool enabled) noexcept {
    effect_config_.enabled = enabled;
}

void PixelChannel::setMask(const std::vector<uint8_t>& mask) {
    if (mask.size() != config_.pixel_count) return;

    if (effect_config_.mask.size() != config_.pixel_count) {
        effect_config_.mask.resize(config_.pixel_count);
    }
    std::copy(mask.begin(), mask.end(), effect_config_.mask.begin());
}

void PixelChannel::clearMask() noexcept {
    effect_config_.mask.clear();
}

void PixelChannel::setupI2S() {
    i2s_chan_config_t chan_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);

    i2s_std_config_t std_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(WS2812B_BITRATE / 16 / 2),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_GPIO_UNUSED,
            .ws = I2S_GPIO_UNUSED,
            .dout = config_.pin,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    esp_err_t ret = i2s_new_channel(&chan_config, &i2s_channel_, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(ret));
        return;
    }

    ret = i2s_channel_init_std_mode(i2s_channel_, &std_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S: %s", esp_err_to_name(ret));
        i2s_del_channel(i2s_channel_);
        i2s_channel_ = nullptr;
        return;
    }

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
    }
}

void PixelChannel::cleanup() {
    if (!initialized_) return;

    terminate_task_ = true;
    if (transmit_semaphore_) {
        xSemaphoreGive(transmit_semaphore_);
    }

    if (i2s_task_handle_) {
        for (int i = 0; i < 100 && terminate_task_; ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (terminate_task_) {
            ESP_LOGW(TAG, "I2S task did not terminate gracefully");
            vTaskDelete(i2s_task_handle_);
        }
        i2s_task_handle_ = nullptr;
    }

    if (i2s_channel_) {
        i2s_del_channel(i2s_channel_);
        i2s_channel_ = nullptr;
    }

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

void PixelChannel::convertToI2SBuffer(const std::vector<PixelColor>& pixels) {
    const size_t bytes_per_pixel = (config_.format == PixelFormat::RGBW)
        ? WS2812B_BYTES_PER_RGBW : WS2812B_BYTES_PER_RGB;
    const size_t data_size = pixels.size() * bytes_per_pixel;

    // Clear reset bytes
    std::fill(i2s_buffer_.begin() + data_size, i2s_buffer_.end(), 0);

    for (size_t i = 0; i < pixels.size(); ++i) {
        const auto& pixel = pixels[i];
        const size_t base_idx = i * bytes_per_pixel;

        // Apply mask
        const bool masked = effect_config_.mask.empty() ||
            (i < effect_config_.mask.size() && effect_config_.mask[i]);

        const uint8_t g = masked ? pixel.g : 0;
        const uint8_t r = masked ? pixel.r : 0;
        const uint8_t b = masked ? pixel.b : 0;
        const uint8_t w = masked ? pixel.w : 0;

        // GRB order for WS2812
        const uint8_t* g_seq = ws2812b_color_lookup[g];
        const uint8_t* r_seq = ws2812b_color_lookup[r];
        const uint8_t* b_seq = ws2812b_color_lookup[b];

        for (int j = 0; j < WS2812B_BYTES_PER_COLOR; ++j) {
            i2s_buffer_[(base_idx + j) ^ 1] = g_seq[j];
            i2s_buffer_[(base_idx + WS2812B_BYTES_PER_COLOR + j) ^ 1] = r_seq[j];
            i2s_buffer_[(base_idx + 2 * WS2812B_BYTES_PER_COLOR + j) ^ 1] = b_seq[j];
        }

        if (config_.format == PixelFormat::RGBW) {
            const uint8_t* w_seq = ws2812b_color_lookup[w];
            for (int j = 0; j < WS2812B_BYTES_PER_COLOR; ++j) {
                i2s_buffer_[(base_idx + 3 * WS2812B_BYTES_PER_COLOR + j) ^ 1] = w_seq[j];
            }
        }
    }
}

void PixelChannel::transmit() {
    if (!initialized_) return;

    convertToI2SBuffer(scaled_buffer_);

    if (transmit_semaphore_) {
        xSemaphoreGive(transmit_semaphore_);
    }
}

uint32_t PixelChannel::getCurrentConsumption() const noexcept {
    uint32_t total_ma = 0;

    for (const auto& pixel : pixel_buffer_) {
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
    const float brightness_scale = effect_config_.brightness / 255.0f;
    const float combined_scale = brightness_scale * std::min(scale_factor, 1.0f);

    for (size_t i = 0; i < pixel_buffer_.size(); ++i) {
        const auto& orig = pixel_buffer_[i];
        scaled_buffer_[i] = PixelColor(
            static_cast<uint8_t>(orig.r * combined_scale),
            static_cast<uint8_t>(orig.g * combined_scale),
            static_cast<uint8_t>(orig.b * combined_scale),
            static_cast<uint8_t>(orig.w * combined_scale)
        );
    }
}

void PixelChannel::saveToNVS() const {
    nvs_handle_t handle;
    char key[16];
    snprintf(key, sizeof(key), "ch_%ld", id_);

    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for channel %ld", id_);
        return;
    }

    std::string effect_key = std::string(key) + ":eff";
    std::string color_key = std::string(key) + ":col";
    std::string bright_key = std::string(key) + ":brt";
    std::string speed_key = std::string(key) + ":spd";
    std::string enabled_key = std::string(key) + ":on";

    nvs_set_str(handle, effect_key.c_str(), effect_config_.effect.c_str());
    nvs_set_blob(handle, color_key.c_str(), &effect_config_.color, sizeof(PixelColor));
    nvs_set_u8(handle, bright_key.c_str(), effect_config_.brightness);
    nvs_set_u8(handle, speed_key.c_str(), effect_config_.speed);
    nvs_set_u8(handle, enabled_key.c_str(), effect_config_.enabled ? 1 : 0);

    nvs_commit(handle);
    nvs_close(handle);
}

void PixelChannel::loadFromNVS() {
    nvs_handle_t handle;
    char key[16];
    snprintf(key, sizeof(key), "ch_%ld", id_);

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGI(TAG, "No saved config for channel %ld", id_);
        return;
    }

    std::string effect_key = std::string(key) + ":eff";
    std::string color_key = std::string(key) + ":col";
    std::string bright_key = std::string(key) + ":brt";
    std::string speed_key = std::string(key) + ":spd";
    std::string enabled_key = std::string(key) + ":on";

    char effect_str[32] = {0};
    size_t len = sizeof(effect_str);
    if (nvs_get_str(handle, effect_key.c_str(), effect_str, &len) == ESP_OK) {
        effect_config_.effect = effect_str;
    }

    PixelColor color;
    size_t color_size = sizeof(PixelColor);
    if (nvs_get_blob(handle, color_key.c_str(), &color, &color_size) == ESP_OK) {
        effect_config_.color = color;
    }

    uint8_t val = 0;
    if (nvs_get_u8(handle, bright_key.c_str(), &val) == ESP_OK) {
        effect_config_.brightness = val;
    }
    if (nvs_get_u8(handle, speed_key.c_str(), &val) == ESP_OK) {
        effect_config_.speed = val;
    }
    if (nvs_get_u8(handle, enabled_key.c_str(), &val) == ESP_OK) {
        effect_config_.enabled = (val != 0);
    }

    nvs_close(handle);
}

void PixelChannel::i2sTaskWrapper(void* param) {
    static_cast<PixelChannel*>(param)->i2sTask();
}

void PixelChannel::i2sTask() {
    size_t bytes_written;

    ESP_LOGD(TAG, "I2S task started for channel %ld", id_);

    while (!terminate_task_) {
        if (xSemaphoreTake(transmit_semaphore_, portMAX_DELAY) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (terminate_task_) break;

        bytes_sent_ = 0;

        esp_err_t ret = i2s_channel_preload_data(i2s_channel_,
            i2s_buffer_.data(), i2s_buffer_.size(), &bytes_written);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S preload failed: %s", esp_err_to_name(ret));
            continue;
        }

        ret = i2s_channel_enable(i2s_channel_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S enable failed: %s", esp_err_to_name(ret));
            continue;
        }

        if (bytes_written < i2s_buffer_.size()) {
            ret = i2s_channel_write(i2s_channel_,
                &i2s_buffer_[bytes_written],
                i2s_buffer_.size() - bytes_written,
                &bytes_written, pdMS_TO_TICKS(1000));
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "I2S write error: %s", esp_err_to_name(ret));
            }
        }

        xSemaphoreTake(complete_semaphore_, portMAX_DELAY);
        i2s_channel_disable(i2s_channel_);
    }

    ESP_LOGD(TAG, "I2S task finished for channel %ld", id_);
    terminate_task_ = false;
    vTaskDelete(nullptr);
}

// ============= HTTP API Implementation =============

namespace {

// Handler to list available effects
esp_err_t led_effects_list_handler(httpd_req_t* req) {
    cJSON* root = cJSON_CreateArray();
    PixelEffectEngine* effect_engine = PixelDriver::getEffectEngine();
    std::vector<PixelEffectEngine::EffectInfo> effects = effect_engine->getAllEffects();

    for (size_t i = 0; i < effects.size(); ++i) {
        const PixelEffectEngine::EffectInfo* eff = &effects[i];
        if (eff) {
            cJSON* obj = cJSON_CreateObject();
            cJSON_AddStringToObject(obj, "name", eff->display_name.c_str());
            cJSON_AddStringToObject(obj, "id", eff->id.c_str());
            cJSON_AddItemToArray(root, obj);
        }
    }
    char* json = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

// Handler to get LED configuration (includes version for WASM sync)
esp_err_t led_config_get_handler(httpd_req_t* req) {
    cJSON* root = cJSON_CreateObject();

    // Add version info for WASM bundle synchronization
    cJSON_AddStringToObject(root, "version", PIXDRIVER_GIT_COMMIT);

    cJSON* channels = cJSON_CreateArray();
    std::vector<int32_t> channel_ids = PixelDriver::getChannelIds();
    for (size_t i = 0; i < channel_ids.size(); ++i) {
        const PixelChannel* ch = PixelDriver::getChannel(channel_ids[i]);
        if (ch) {
            ChannelConfig config = ch->getConfig();
            cJSON* ch_obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(ch_obj, "index", i);
            cJSON_AddNumberToObject(ch_obj, "num_leds", config.pixel_count);
            cJSON_AddStringToObject(ch_obj, "type", config.format == PixelFormat::RGB ? "RGB" : "RGBW");
            cJSON_AddItemToArray(channels, ch_obj);
        }
    }
    cJSON_AddItemToObject(root, "channels", channels);
    char* json = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

// Handler to get a single channel configuration (GET /api/led/channel/*)
esp_err_t led_channel_get_handler(httpd_req_t* req) {
    int channel_idx = -1;
    // Parse from URI path
    const char* uri = req->uri;
    const char* base = "/api/led/channel/";
    if (strncmp(uri, base, strlen(base)) == 0) {
        channel_idx = atoi(uri + strlen(base));
    }
    if (channel_idx < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid channel index");
        return ESP_FAIL;
    }
    const PixelChannel* ch = PixelDriver::getChannel(channel_idx);
    if (!ch) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Channel not found");
        return ESP_FAIL;
    }
    const ChannelConfig& cfg = ch->getConfig();
    const EffectConfig& eff = ch->getEffectConfig();
    cJSON* ch_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(ch_obj, "effect_id", eff.effect.c_str());
    cJSON_AddNumberToObject(ch_obj, "brightness", eff.brightness);
    cJSON_AddNumberToObject(ch_obj, "speed", eff.speed);
    cJSON_AddBoolToObject(ch_obj, "on", eff.enabled);
    cJSON* color_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(color_obj, "r", eff.color.r);
    cJSON_AddNumberToObject(color_obj, "g", eff.color.g);
    cJSON_AddNumberToObject(color_obj, "b", eff.color.b);
    if (cfg.format == PixelFormat::RGBW) {
        cJSON_AddNumberToObject(color_obj, "w", eff.color.w);
    }
    cJSON_AddItemToObject(ch_obj, "color", color_obj);
    char* json = cJSON_Print(ch_obj);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    cJSON_Delete(ch_obj);
    return ESP_OK;
}

// Handler to configure a channel (POST /api/led/channel/*)
esp_err_t led_channel_config_handler(httpd_req_t* req) {
    int channel_idx = -1;
    // Parse from URI path
    const char* uri = req->uri;
    const char* base = "/api/led/channel/";
    if (strncmp(uri, base, strlen(base)) == 0) {
        channel_idx = atoi(uri + strlen(base));
    }
    if (channel_idx < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid channel index");
        return ESP_FAIL;
    }
    PixelChannel* ch = PixelDriver::getChannel(channel_idx);
    if (!ch) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Channel not found");
        return ESP_FAIL;
    }

    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    cJSON* json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    cJSON* color = cJSON_GetObjectItem(json, "color");
    cJSON* brightness = cJSON_GetObjectItem(json, "brightness");
    cJSON* speed = cJSON_GetObjectItem(json, "speed");
    cJSON* on = cJSON_GetObjectItem(json, "on");
    cJSON* effect_id = cJSON_GetObjectItem(json, "effect_id");

    // Set effect config
    EffectConfig eff_cfg = ch->getEffectConfig();
    if (effect_id && cJSON_IsString(effect_id)) eff_cfg.effect = effect_id->valuestring;
    if (brightness && cJSON_IsNumber(brightness)) eff_cfg.brightness = brightness->valueint;
    if (speed && cJSON_IsNumber(speed)) eff_cfg.speed = speed->valueint;
    if (on && cJSON_IsBool(on)) eff_cfg.enabled = cJSON_IsTrue(on);
    if (color && cJSON_IsObject(color)) {
        cJSON* r = cJSON_GetObjectItem(color, "r");
        cJSON* g = cJSON_GetObjectItem(color, "g");
        cJSON* b = cJSON_GetObjectItem(color, "b");
        cJSON* w = cJSON_GetObjectItem(color, "w");
        if (cJSON_IsNumber(r)) eff_cfg.color.r = r->valueint;
        if (cJSON_IsNumber(g)) eff_cfg.color.g = g->valueint;
        if (cJSON_IsNumber(b)) eff_cfg.color.b = b->valueint;
        if (w && cJSON_IsNumber(w)) eff_cfg.color.w = w->valueint;
    }

    ch->setEffect(eff_cfg);

    cJSON_Delete(json);
    return led_channel_get_handler(req); // Return updated config
}

} // anonymous namespace

void PixelDriver::attach_api(httpd_handle_t server) {
    static httpd_uri_t effects_uri = {
        .uri = "/api/led/effects",
        .method = HTTP_GET,
        .handler = led_effects_list_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &effects_uri);

    static httpd_uri_t config_uri = {
        .uri = "/api/led/config",
        .method = HTTP_GET,
        .handler = led_config_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &config_uri);

    static httpd_uri_t channel_get_uri = {
        .uri = "/api/led/channel/*",
        .method = HTTP_GET,
        .handler = led_channel_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &channel_get_uri);

    static httpd_uri_t channel_post_uri = {
        .uri = "/api/led/channel/*",
        .method = HTTP_POST,
        .handler = led_channel_config_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &channel_post_uri);

    ESP_LOGI(TAG, "LED API attached (version: %s)", PIXDRIVER_GIT_COMMIT);
}
