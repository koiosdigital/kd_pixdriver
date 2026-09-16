#include "kd_pixdriver.h"
#include "pixel_effects.h"
#include "pixel_version.h"
#include "i2s_pixel_protocol.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "driver/i2s_std.h"
#include "driver/i2s_common.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if CONFIG_KD_PIXDRIVER_HTTP_API
#include "cJSON.h"
#endif

namespace {
    constexpr const char* TAG = "kd_pixdriver";
    constexpr const char* NVS_NAMESPACE = "pixdriver";

    // Below the Wi-Fi task (23) - a frame that starts a few hundred µs late
    // is invisible, a starved Wi-Fi driver is not - and above the driver task.
    constexpr UBaseType_t I2S_TASK_PRIORITY = 20;
    constexpr UBaseType_t DRIVER_TASK_PRIORITY = 7;
    constexpr uint32_t DRIVER_TASK_STACK = 4096;
    // stop() join budget: a frame plus one NVS write (done outside the lock).
    constexpr TickType_t STOP_JOIN_TIMEOUT = pdMS_TO_TICKS(500);
    // The driver task takes the lock with a timeout so a stop() issued by a
    // task that already holds the lock is observed promptly.
    constexpr TickType_t RENDER_LOCK_TIMEOUT = pdMS_TO_TICKS(20);
    // A transmit that never completes (DMA callback lost) must not wedge the
    // I2S task forever - the channel is disabled and the next frame retried.
    constexpr TickType_t TRANSMIT_COMPLETE_TIMEOUT = pdMS_TO_TICKS(1000);
    constexpr int64_t NVS_SETTLE_US = 2000000;  // 2s after the last change

    // Persisted per-strip settings are keyed by the data pin ("ch_p<pin>:x"),
    // which is what identifies a strip across reconfigures and reboots; the
    // pre-stable-id layout ("ch_<id>:x") is migrated on first load.
    constexpr const char* NVS_SUFFIXES[] = { "eff", "col", "brt", "spd", "on" };

    void nvsKey(char* out, size_t out_size, gpio_num_t pin, const char* suffix) {
        snprintf(out, out_size, "ch_p%d:%s", static_cast<int>(pin), suffix);
    }

    void legacyNvsKey(char* out, size_t out_size, int32_t id, const char* suffix) {
        snprintf(out, out_size, "ch_%ld:%s", static_cast<long>(id), suffix);
    }

    // Created on first use; C++11 guarantees the initialisation is thread-safe
    // (ESP-IDF implements __cxa_guard_* on FreeRTOS).
    SemaphoreHandle_t driverLock() {
        static SemaphoreHandle_t handle = xSemaphoreCreateRecursiveMutex();
        return handle;
    }

    // Per-frame lock acquisition for the driver task (timed; see stop()).
    bool tryLockFor(TickType_t ticks) {
        SemaphoreHandle_t h = driverLock();
        if (!h) return true;  // no mutex (allocation failed at boot): run unlocked
        return xSemaphoreTakeRecursive(h, ticks) == pdTRUE;
    }

    void unlock() {
        if (SemaphoreHandle_t h = driverLock()) xSemaphoreGiveRecursive(h);
    }
} // anonymous namespace

// ============= PixelDriver::Lock =============

PixelDriver::Lock::Lock() {
    if (SemaphoreHandle_t h = driverLock()) xSemaphoreTakeRecursive(h, portMAX_DELAY);
}

PixelDriver::Lock::~Lock() {
    unlock();
}

// Static member definitions
std::vector<std::unique_ptr<PixelChannel>> PixelDriver::channels_;
std::unique_ptr<PixelEffectEngine> PixelDriver::effect_engine_;
int32_t PixelDriver::main_channel_id_ = -1;
TaskHandle_t PixelDriver::task_handle_ = nullptr;
SemaphoreHandle_t PixelDriver::task_exit_sem_ = nullptr;
int32_t PixelDriver::current_limit_ma_ = -1;
uint32_t PixelDriver::update_rate_hz_ = 60;
std::atomic<bool> PixelDriver::running_{ false };
std::atomic<bool> PixelDriver::task_alive_{ false };
bool PixelDriver::initialized_ = false;

// I2S callback function (ISR context; IRAM-safe: touches only the channel
// object and the semaphore, both in internal RAM)
extern "C" IRAM_ATTR bool i2s_tx_callback(i2s_chan_handle_t handle,
    i2s_event_data_t* event,
    void* user_ctx) {
    (void)handle;
    auto* channel = static_cast<PixelChannel*>(user_ctx);
    BaseType_t higher_priority_task_woken = pdFALSE;
    if (channel && event && channel->complete_semaphore_) {
        channel->bytes_sent_ += event->size;
        if (channel->bytes_sent_ >= channel->i2s_buffer_.size()) {
            xSemaphoreGiveFromISR(channel->complete_semaphore_, &higher_priority_task_woken);
        }
    }
    return higher_priority_task_woken == pdTRUE;
}

// ============= PixelDriver Implementation =============

void PixelDriver::initialize(uint32_t update_rate_hz) {
    Lock lock;
    if (initialized_) return;

    update_rate_hz_ = std::clamp<uint32_t>(update_rate_hz, 1, 1000);
    effect_engine_ = std::make_unique<PixelEffectEngine>(update_rate_hz_);
    initialized_ = true;
    ESP_LOGI(TAG, "PixelDriver initialized at %lu Hz", static_cast<unsigned long>(update_rate_hz_));
}

void PixelDriver::shutdown() {
    stop();
    Lock lock;
    if (!initialized_) return;

    channels_.clear();
    effect_engine_.reset();
    main_channel_id_ = -1;
    initialized_ = false;
    ESP_LOGI(TAG, "PixelDriver shutdown");
}

int32_t PixelDriver::addChannel(const ChannelConfig& config) {
    Lock lock;
    if (!initialized_) {
        ESP_LOGE(TAG, "PixelDriver not initialized");
        return -1;
    }

    // Lowest free id: ids are what the API exposes and index the effect
    // engine's state table, so a remove/add reconfigure must hand back the
    // same id rather than an ever-increasing one.
    int32_t id = 0;
    while (getChannel(id) != nullptr) ++id;

    auto channel = std::make_unique<PixelChannel>(id, config);

    if (!channel->initialize()) {
        ESP_LOGE(TAG, "Failed to initialize channel %ld", static_cast<long>(id));
        return -1;
    }

    channel->loadFromNVS();
    if (effect_engine_) effect_engine_->resetChannelState(id);

    if (main_channel_id_ == -1) {
        main_channel_id_ = id;
        ESP_LOGI(TAG, "Set channel %ld as main", static_cast<long>(id));
    }

    ESP_LOGI(TAG, "Added channel %ld: pin %d, %u pixels, %s",
        static_cast<long>(id), static_cast<int>(config.pin), static_cast<unsigned>(config.pixel_count),
        config.format == PixelFormat::RGBW ? "RGBW"
            : config.format == PixelFormat::RGBCCT ? "RGBCCT" : "RGB");

    channels_.emplace_back(std::move(channel));
    return id;
}

bool PixelDriver::removeChannel(int32_t channel_id, bool persist_forget) {
    // The driver task holds the lock for the CPU part of every frame, so
    // once we have it nobody is inside this channel.
    Lock lock;
    auto it = std::find_if(channels_.begin(), channels_.end(),
        [channel_id](const auto& ch) { return ch->getId() == channel_id; });

    if (it == channels_.end()) return false;

    PixelChannel* ch = it->get();
    if (persist_forget) {
        forgetChannelSettings(ch->getConfig().pin);
    }
    else {
        // A change still inside the settle window would otherwise be lost
        // by the app's stop/remove/add reconfigure sequence.
        ch->flushPendingSave();
    }
    if (effect_engine_) effect_engine_->resetChannelState(channel_id);

    if (main_channel_id_ == channel_id) {
        main_channel_id_ = -1;
        for (const auto& other : channels_) {
            if (other->getId() != channel_id) {
                main_channel_id_ = other->getId();
                ESP_LOGI(TAG, "Set channel %ld as new main", static_cast<long>(main_channel_id_));
                break;
            }
        }
    }

    channels_.erase(it);  // ~PixelChannel joins its I2S task and releases the peripheral
    ESP_LOGI(TAG, "Removed channel %ld%s", static_cast<long>(channel_id),
        persist_forget ? " (settings erased)" : "");
    return true;
}

bool PixelDriver::forgetChannelSettings(gpio_num_t pin) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "forgetChannelSettings: nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }
    bool ok = true;
    char key[16];
    for (const char* suffix : NVS_SUFFIXES) {
        nvsKey(key, sizeof(key), pin, suffix);
        err = nvs_erase_key(handle, key);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "nvs_erase_key(%s) failed: %s", key, esp_err_to_name(err));
            ok = false;
        }
    }
    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
        ok = false;
    }
    nvs_close(handle);
    return ok;
}

PixelChannel* PixelDriver::getChannel(int32_t channel_id) {
    Lock lock;
    auto it = std::find_if(channels_.begin(), channels_.end(),
        [channel_id](const auto& ch) { return ch->getId() == channel_id; });
    return (it != channels_.end()) ? it->get() : nullptr;
}

PixelChannel* PixelDriver::getMainChannel() {
    Lock lock;
    return (main_channel_id_ != -1) ? getChannel(main_channel_id_) : nullptr;
}

std::vector<int32_t> PixelDriver::getChannelIds() {
    Lock lock;
    std::vector<int32_t> ids;
    ids.reserve(channels_.size());
    for (const auto& ch : channels_) {
        ids.push_back(ch->getId());
    }
    return ids;
}

void PixelDriver::setCurrentLimit(int32_t limit_ma) {
    Lock lock;
    current_limit_ma_ = limit_ma;
    ESP_LOGI(TAG, "Current limit: %ld mA", static_cast<long>(limit_ma));
}

int32_t PixelDriver::getCurrentLimit() noexcept {
    Lock lock;
    return current_limit_ma_;
}

bool PixelDriver::setUpdateRate(uint32_t rate_hz) {
    Lock lock;
    if (rate_hz == 0) return false;
    if (running_.load(std::memory_order_acquire)) {
        ESP_LOGE(TAG, "setUpdateRate: stop the driver first");
        return false;
    }
    update_rate_hz_ = std::clamp<uint32_t>(rate_hz, 1, 1000);
    if (initialized_) {
        // Channels re-resolve their effect index against the new engine
        // (its registry generation differs).
        effect_engine_ = std::make_unique<PixelEffectEngine>(update_rate_hz_);
    }
    return true;
}

uint32_t PixelDriver::getUpdateRate() noexcept {
    return update_rate_hz_;
}

bool PixelDriver::start() {
    Lock lock;
    if (!initialized_) {
        ESP_LOGE(TAG, "start: PixelDriver not initialized");
        return false;
    }
    if (running_.load(std::memory_order_acquire)) return true;

    if (!task_exit_sem_) {
        task_exit_sem_ = xSemaphoreCreateBinary();
        if (!task_exit_sem_) {
            ESP_LOGE(TAG, "start: failed to create exit semaphore");
            return false;
        }
    }

    // A previous stop() may have timed out with the task still unwinding.
    // Never run two driver tasks: wait a little longer, then refuse.
    if (task_alive_.load(std::memory_order_acquire)) {
        if (xSemaphoreTake(task_exit_sem_, STOP_JOIN_TIMEOUT) != pdTRUE &&
            task_alive_.load(std::memory_order_acquire)) {
            ESP_LOGE(TAG, "start: previous driver task still running");
            return false;
        }
    }
    xSemaphoreTake(task_exit_sem_, 0);  // clear a stale give

    running_.store(true, std::memory_order_release);
    task_alive_.store(true, std::memory_order_release);
    if (xTaskCreate(driverTask, "pixdriver", DRIVER_TASK_STACK, nullptr,
        DRIVER_TASK_PRIORITY, &task_handle_) != pdPASS) {
        running_.store(false, std::memory_order_release);
        task_alive_.store(false, std::memory_order_release);
        task_handle_ = nullptr;
        ESP_LOGE(TAG, "start: failed to create driver task");
        return false;
    }
    ESP_LOGI(TAG, "PixelDriver started");
    return true;
}

void PixelDriver::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;

    if (task_handle_ && xTaskGetCurrentTaskHandle() == task_handle_) {
        // From an effect callback: the loop sees running_ == false after
        // this frame and exits; joining ourselves is impossible.
        ESP_LOGW(TAG, "stop() called from the driver task; exiting after this frame");
        return;
    }

    // Join. The task never blocks on the driver lock indefinitely (timed
    // take), so this works even when the caller holds PixelDriver::Lock.
    const bool exited = task_exit_sem_ &&
        xSemaphoreTake(task_exit_sem_, STOP_JOIN_TIMEOUT) == pdTRUE;
    if (exited) {
        task_handle_ = nullptr;
        ESP_LOGI(TAG, "PixelDriver stopped");
    }
    else {
        ESP_LOGE(TAG, "Driver task did not exit within %u ms; it will finish on its own",
            static_cast<unsigned>(STOP_JOIN_TIMEOUT * portTICK_PERIOD_MS));
    }
}

bool PixelDriver::isRunning() noexcept {
    return running_.load(std::memory_order_acquire);
}

void PixelDriver::setAllChannelsEffect(std::string_view effect_id) {
    Lock lock;
    for (auto& ch : channels_) {
        ch->setEffectByID(effect_id);
    }
}

void PixelDriver::setAllChannelsColor(const PixelColor& color) {
    Lock lock;
    for (auto& ch : channels_) {
        ch->setColor(color);
    }
}

void PixelDriver::setAllChannelsBrightness(uint8_t brightness) {
    Lock lock;
    for (auto& ch : channels_) {
        ch->setBrightness(brightness);
    }
}

void PixelDriver::setAllChannelsEnabled(bool enabled) {
    Lock lock;
    for (auto& ch : channels_) {
        ch->setEnabled(enabled);
    }
}

uint32_t PixelDriver::getTotalCurrentConsumption() {
    Lock lock;
    uint32_t total = 0;
    for (const auto& ch : channels_) {
        total += ch->getCurrentConsumption();
    }
    return total;
}

uint32_t PixelDriver::getScaledCurrentConsumption() {
    Lock lock;
    uint32_t total = 0;
    for (const auto& ch : channels_) {
        total += ch->getScaledCurrentConsumption();
    }
    return total;
}

uint32_t PixelDriver::currentLimitScaleQ8() {
    if (current_limit_ma_ <= 0) return 256;

    const uint32_t total = getTotalCurrentConsumption();
    const uint32_t available = (current_limit_ma_ > static_cast<int32_t>(SYSTEM_RESERVE_MA))
        ? (static_cast<uint32_t>(current_limit_ma_) - SYSTEM_RESERVE_MA) : 0;

    if (total <= available) return 256;
    if (available == 0) return 0;
    return (available * 256u) / total;  // < 256
}

float PixelDriver::getCurrentScaleFactor() {
    Lock lock;
    return static_cast<float>(currentLimitScaleQ8()) / 256.0f;
}

void PixelDriver::driverTask(void* param) {
    (void)param;
    TickType_t last_wake_time = xTaskGetTickCount();
    const uint32_t rate = update_rate_hz_ ? update_rate_hz_ : 60;
    TickType_t update_period = pdMS_TO_TICKS(1000 / rate);
    if (update_period == 0) update_period = 1;
    uint32_t tick = 0;

    std::vector<PixelChannel::PendingSave> pending_saves;

    while (running_.load(std::memory_order_acquire)) {
        // Hold the driver lock for the CPU part of the frame: the effect
        // engine reads each channel's live config and setters on other
        // tasks reassign its string/mask members; removeChannel destroys
        // channels. Timed take so a stop() issued by a task that holds the
        // lock (the app's reconfigure sequence) is observed within a frame.
        if (tryLockFor(RENDER_LOCK_TIMEOUT)) {
            if (running_.load(std::memory_order_acquire)) {
                // Update effects
                for (auto& ch : channels_) {
                    if (ch->getEffectConfig().enabled && effect_engine_) {
                        effect_engine_->updateEffect(ch.get(), tick);
                    }
                    else {
                        auto& buffer = ch->getPixelBuffer();
                        std::fill(buffer.begin(), buffer.end(), PixelColor::Black());
                    }
                }

                // Apply brightness + current limiting and transmit
                applyCurrentLimiting();

                for (auto& ch : channels_) {
                    ch->transmit();
                    // Collect settled config changes; written below, outside
                    // the lock and the frame.
                    ch->takeSettledSave(pending_saves);
                }
                tick++;
            }
            unlock();

            for (const auto& save : pending_saves) {
                PixelChannel::writeToNVS(save);
            }
            pending_saves.clear();
        }

        vTaskDelayUntil(&last_wake_time, update_period);
    }

    task_alive_.store(false, std::memory_order_release);
    if (task_exit_sem_) xSemaphoreGive(task_exit_sem_);
    vTaskDelete(nullptr);
}

void PixelDriver::applyCurrentLimiting() {
    const uint32_t scale_q8 = currentLimitScaleQ8();
    for (auto& ch : channels_) {
        ch->applyScalingQ8(scale_q8);
    }
}

// ============= PixelChannel Implementation =============

PixelChannel::PixelChannel(int32_t id, const ChannelConfig& config)
    : id_(id)
    , config_(config)
    , effect_index_(PixelEffectEngine::kEffectUnknown)
    , effect_registry_gen_(0)
    , initialized_(false)
    , bytes_sent_(0) {

    // Pre-allocate all buffers
    pixel_buffer_.resize(config.pixel_count, PixelColor::Black());
    scaled_buffer_.resize(config.pixel_count, PixelColor::Black());

    // Encoded I2S bytes/pixel = 3 encoded bytes per WIRE channel. RGBCCT
    // (FW1906) pixels occupy 6 wire channels - two RGB groups per chip
    // frame - even though only 5 carry color; see wireChannelCount().
    const size_t bytes_per_pixel = WS2812B_BYTES_PER_COLOR * wireChannelCount(config.format);
    const size_t buffer_size = (config.pixel_count * bytes_per_pixel) + WS2812B_RESET_BYTES;
    i2s_buffer_.resize(buffer_size, 0);

    // Default effect
    effect_config_.effect = "SOLID";
    effect_config_.color = PixelColor(100, 100, 100);
    effect_config_.brightness = 255;
    effect_config_.speed = 5;
    effect_config_.enabled = true;
    resolveEffect();
}

PixelChannel::~PixelChannel() {
    cleanup();
}

bool PixelChannel::initialize() {
    if (initialized_) return true;

    transmit_semaphore_ = xSemaphoreCreateBinary();
    complete_semaphore_ = xSemaphoreCreateBinary();

    if (!transmit_semaphore_ || !complete_semaphore_) {
        ESP_LOGE(TAG, "Failed to create semaphores for channel %ld", static_cast<long>(id_));
        cleanup();
        return false;
    }

    setupI2S();

    if (!i2s_channel_) {
        ESP_LOGE(TAG, "Failed to setup I2S for channel %ld", static_cast<long>(id_));
        cleanup();
        return false;
    }

    char task_name[16];
    snprintf(task_name, sizeof(task_name), "i2s_%ld", static_cast<long>(id_));

    terminate_task_.store(false);
    i2s_task_exited_.store(false);
    // 2048 (not 1024): this task enters the I2S driver (channel_write/enable)
    // and formats ESP_LOGE(esp_err_to_name(...)) on error paths.
    const UBaseType_t prio = std::min<UBaseType_t>(I2S_TASK_PRIORITY, configMAX_PRIORITIES - 1);
    if (xTaskCreate(i2sTaskWrapper, task_name, 2048, this, prio, &i2s_task_handle_) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create I2S task for channel %ld", static_cast<long>(id_));
        i2s_task_handle_ = nullptr;
        cleanup();
        return false;
    }

    initialized_ = true;
    ESP_LOGI(TAG, "Channel %ld initialized", static_cast<long>(id_));
    return true;
}

void PixelChannel::markConfigDirty() noexcept {
    nvs_dirty_at_us_.store(esp_timer_get_time(), std::memory_order_relaxed);
    nvs_dirty_.store(true, std::memory_order_release);
}

bool PixelChannel::takeSettledSave(std::vector<PendingSave>& out) {
    if (!nvs_dirty_.load(std::memory_order_acquire)) return false;
    if (esp_timer_get_time() - nvs_dirty_at_us_.load(std::memory_order_relaxed) < NVS_SETTLE_US) return false;
    nvs_dirty_.store(false, std::memory_order_release);
    out.push_back(PendingSave{ config_.pin, id_, effect_config_ });
    return true;
}

void PixelChannel::persistIfSettled() {
    std::vector<PendingSave> pending;
    {
        PixelDriver::Lock lock;
        takeSettledSave(pending);
    }
    for (const auto& save : pending) writeToNVS(save);
}

void PixelChannel::flushPendingSave() {
    if (nvs_dirty_.exchange(false, std::memory_order_acq_rel)) {
        saveToNVS();
    }
}

EffectConfig PixelChannel::getEffectConfigCopy() const {
    PixelDriver::Lock lock;
    return effect_config_;
}

void PixelChannel::resolveEffect() {
    PixelEffectEngine* engine = PixelDriver::getEffectEngine();
    if (!engine) {
        effect_index_ = PixelEffectEngine::kEffectUnknown;
        effect_registry_gen_ = 0;
        return;
    }
    effect_index_ = engine->resolveEffect(effect_config_.effect);
    effect_registry_gen_ = engine->registryGeneration();
}

int PixelChannel::effectIndex(const PixelEffectEngine& engine) {
    if (effect_registry_gen_ != engine.registryGeneration()) {
        effect_index_ = engine.resolveEffect(effect_config_.effect);
        effect_registry_gen_ = engine.registryGeneration();
    }
    return effect_index_;
}

void PixelChannel::setEffect(const EffectConfig& config) {
    PixelDriver::Lock lock;
    effect_config_ = config;
    effect_config_.speed = std::clamp(effect_config_.speed, uint8_t(1), uint8_t(10));
    if (!effect_config_.mask.empty() && effect_config_.mask.size() != config_.pixel_count) {
        ESP_LOGW(TAG, "Channel %ld: mask size %u != %u pixels, ignoring mask",
            static_cast<long>(id_), static_cast<unsigned>(effect_config_.mask.size()),
            static_cast<unsigned>(config_.pixel_count));
        effect_config_.mask.clear();
    }
    resolveEffect();
    markConfigDirty();
}

void PixelChannel::setEffectByID(std::string_view effect_id) {
    PixelDriver::Lock lock;
    effect_config_.effect = std::string(effect_id);
    resolveEffect();
    markConfigDirty();
}

void PixelChannel::setColor(const PixelColor& color) noexcept {
    PixelDriver::Lock lock;
    effect_config_.color = color;
    markConfigDirty();
}

void PixelChannel::setBrightness(uint8_t brightness) noexcept {
    PixelDriver::Lock lock;
    effect_config_.brightness = brightness;
    markConfigDirty();
}

void PixelChannel::setSpeed(uint8_t speed) noexcept {
    PixelDriver::Lock lock;
    effect_config_.speed = std::clamp(speed, uint8_t(1), uint8_t(10));
    markConfigDirty();
}

void PixelChannel::setEnabled(bool enabled) noexcept {
    PixelDriver::Lock lock;
    effect_config_.enabled = enabled;
    markConfigDirty();
}

void PixelChannel::setMask(const std::vector<uint8_t>& mask) {
    if (mask.size() != config_.pixel_count) return;

    PixelDriver::Lock lock;
    if (effect_config_.mask.size() != config_.pixel_count) {
        effect_config_.mask.resize(config_.pixel_count);
    }
    std::copy(mask.begin(), mask.end(), effect_config_.mask.begin());
}

void PixelChannel::clearMask() noexcept {
    PixelDriver::Lock lock;
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
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    esp_err_t ret = i2s_new_channel(&chan_config, &i2s_channel_, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(ret));
        i2s_channel_ = nullptr;
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

// Idempotent; also runs for partially-initialised channels (failure paths
// in initialize()), so every step checks what actually exists.
void PixelChannel::cleanup() {
    if (i2s_task_handle_) {
        terminate_task_.store(true, std::memory_order_release);
        if (transmit_semaphore_) {
            xSemaphoreGive(transmit_semaphore_);
        }
        for (int i = 0; i < 100 && !i2s_task_exited_.load(std::memory_order_acquire); ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (!i2s_task_exited_.load(std::memory_order_acquire)) {
            // Last resort. The task may die mid-transfer with the channel
            // running; quiesce the hardware right away so the DMA stops
            // pulling from a buffer that is about to be freed.
            ESP_LOGW(TAG, "I2S task for channel %ld did not terminate gracefully, deleting it",
                static_cast<long>(id_));
            vTaskDelete(i2s_task_handle_);
            if (i2s_channel_) {
                esp_err_t ret = i2s_channel_disable(i2s_channel_);
                if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
                    ESP_LOGW(TAG, "i2s_channel_disable failed: %s", esp_err_to_name(ret));
                }
            }
        }
        i2s_task_handle_ = nullptr;
    }

    if (i2s_channel_) {
        // Disable (no-op when already disabled), detach the ISR callback so
        // it can never fire into this object or the semaphore once they are
        // gone, then delete.
        esp_err_t ret = i2s_channel_disable(i2s_channel_);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "i2s_channel_disable failed: %s", esp_err_to_name(ret));
        }
        i2s_event_callbacks_t none = {
            .on_recv = nullptr,
            .on_recv_q_ovf = nullptr,
            .on_sent = nullptr,
            .on_send_q_ovf = nullptr,
        };
        ret = i2s_channel_register_event_callback(i2s_channel_, &none, nullptr);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to clear I2S callbacks: %s", esp_err_to_name(ret));
        }
        ret = i2s_del_channel(i2s_channel_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2s_del_channel failed for channel %ld: %s",
                static_cast<long>(id_), esp_err_to_name(ret));
        }
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
    terminate_task_.store(false, std::memory_order_release);
    i2s_task_exited_.store(false, std::memory_order_release);
}

void PixelChannel::convertToI2SBuffer(const std::vector<PixelColor>& pixels) {
    // WIRE channels: 3 (RGB) / 4 (RGBW) / 6 (RGBCCT on FW1906 - two RGB
    // groups per chip frame, 6th output unused).
    const size_t channels = wireChannelCount(config_.format);
    const size_t bytes_per_pixel = WS2812B_BYTES_PER_COLOR * channels;
    const size_t data_size = pixels.size() * bytes_per_pixel;

    // Clear reset bytes
    std::fill(i2s_buffer_.begin() + data_size, i2s_buffer_.end(), 0);

    for (size_t i = 0; i < pixels.size(); ++i) {
        const auto& pixel = pixels[i];
        const size_t base_idx = i * bytes_per_pixel;

        // Apply mask
        const bool masked = effect_config_.mask.empty() ||
            (i < effect_config_.mask.size() && effect_config_.mask[i]);

        uint8_t r = masked ? pixel.r : 0;
        uint8_t g = masked ? pixel.g : 0;
        uint8_t b = masked ? pixel.b : 0;
        uint8_t w = masked ? pixel.w : 0;
        uint8_t cw = masked ? pixel.cw : 0;

        // White extraction for single-white (RGBW) strips: colors sourced from
        // RGB (solid colors, effects) leave w == 0, which wastes the dedicated
        // white die and renders whites as R+G+B glare. Move the common
        // component to the white channel; a pixel that explicitly sets w keeps
        // manual control. RGBCCT (dual white) is driven per-pixel end-to-end
        // (w = warm, cw = cool) with no auto-extraction, so effects/API have
        // full control of both white dies.
        if (config_.format == PixelFormat::RGBW && w == 0) {
            w = std::min(r, std::min(g, b));
            r = static_cast<uint8_t>(r - w);
            g = static_cast<uint8_t>(g - w);
            b = static_cast<uint8_t>(b - w);
        }

        // Assemble the per-pixel byte values in wire order: the R/G/B triple
        // per color_order, then the white channel(s). For RGBCCT (FW1906) the
        // frame is two RGB groups: [R,G,B][W1,W2,pad] - the two whites ride on
        // the chip's second group (warm then cool; white_swap flips them) and
        // the 6th output is unused, so its byte stays 0. If the whites appear
        // dead on a particular strip, its whites are wired to G2/B2 instead of
        // R2/G2 - the pad byte would then belong FIRST; adjust here.
        uint8_t seq_vals[6] = {0, 0, 0, 0, 0, 0};
        switch (config_.color_order) {
            case ColorOrder::RGB: seq_vals[0]=r; seq_vals[1]=g; seq_vals[2]=b; break;
            case ColorOrder::RBG: seq_vals[0]=r; seq_vals[1]=b; seq_vals[2]=g; break;
            case ColorOrder::GRB: seq_vals[0]=g; seq_vals[1]=r; seq_vals[2]=b; break;
            case ColorOrder::GBR: seq_vals[0]=g; seq_vals[1]=b; seq_vals[2]=r; break;
            case ColorOrder::BRG: seq_vals[0]=b; seq_vals[1]=r; seq_vals[2]=g; break;
            case ColorOrder::BGR: seq_vals[0]=b; seq_vals[1]=g; seq_vals[2]=r; break;
        }
        if (config_.format == PixelFormat::RGBCCT) {
            seq_vals[3] = config_.white_swap ? cw : w;
            seq_vals[4] = config_.white_swap ? w : cw;
        } else if (config_.format == PixelFormat::RGBW) {
            seq_vals[3] = w;
        }

        for (size_t k = 0; k < channels; ++k) {
            const uint8_t* seq = ws2812b_color_lookup[seq_vals[k]];
            const size_t cbase = base_idx + k * WS2812B_BYTES_PER_COLOR;
            for (int j = 0; j < WS2812B_BYTES_PER_COLOR; ++j) {
                i2s_buffer_[(cbase + j) ^ 1] = seq[j];
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

namespace {
    uint32_t bufferCurrentMa(const std::vector<PixelColor>& buffer, PixelFormat format) {
        uint32_t total_ma = 0;
        for (const auto& pixel : buffer) {
            total_ma += (pixel.r * PixelDriver::CURRENT_PER_CHANNEL_MA) / 255;
            total_ma += (pixel.g * PixelDriver::CURRENT_PER_CHANNEL_MA) / 255;
            total_ma += (pixel.b * PixelDriver::CURRENT_PER_CHANNEL_MA) / 255;
            if (format == PixelFormat::RGBW || format == PixelFormat::RGBCCT) {
                total_ma += (pixel.w * PixelDriver::CURRENT_PER_CHANNEL_MA) / 255;
            }
            if (format == PixelFormat::RGBCCT) {
                total_ma += (pixel.cw * PixelDriver::CURRENT_PER_CHANNEL_MA) / 255;
            }
        }
        return total_ma;
    }
} // anonymous namespace

uint32_t PixelChannel::getCurrentConsumption() const noexcept {
    // Post-brightness, pre-limit: what the limiter has to fit into budget.
    return (bufferCurrentMa(pixel_buffer_, config_.format) * effect_config_.brightness) / 255;
}

uint32_t PixelChannel::getScaledCurrentConsumption() const noexcept {
    return bufferCurrentMa(scaled_buffer_, config_.format);
}

void PixelChannel::applyCurrentScaling(float scale_factor) {
    const float clamped = std::clamp(scale_factor, 0.0f, 1.0f);
    applyScalingQ8(static_cast<uint32_t>(clamped * 256.0f + 0.5f));
}

void PixelChannel::applyScalingQ8(uint32_t limit_q8) {
    if (limit_q8 > 256) limit_q8 = 256;
    // brightness (0-255) x limit (0-256) -> 0-256, exact identity at 255/256.
    const uint32_t combined_q8 = (effect_config_.brightness * limit_q8 + 127) / 255;

    for (size_t i = 0; i < pixel_buffer_.size(); ++i) {
        const auto& orig = pixel_buffer_[i];
        scaled_buffer_[i] = PixelColor(
            static_cast<uint8_t>((orig.r * combined_q8) >> 8),
            static_cast<uint8_t>((orig.g * combined_q8) >> 8),
            static_cast<uint8_t>((orig.b * combined_q8) >> 8),
            static_cast<uint8_t>((orig.w * combined_q8) >> 8),
            static_cast<uint8_t>((orig.cw * combined_q8) >> 8)
        );
    }
}

void PixelChannel::writeToNVS(const PendingSave& save) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for channel %ld: %s",
            static_cast<long>(save.id), esp_err_to_name(err));
        return;
    }

    char key[16];
    const EffectConfig& cfg = save.config;

    nvsKey(key, sizeof(key), save.pin, "eff");
    err = nvs_set_str(handle, key, cfg.effect.c_str());
    if (err == ESP_OK) {
        nvsKey(key, sizeof(key), save.pin, "col");
        err = nvs_set_blob(handle, key, &cfg.color, sizeof(PixelColor));
    }
    if (err == ESP_OK) {
        nvsKey(key, sizeof(key), save.pin, "brt");
        err = nvs_set_u8(handle, key, cfg.brightness);
    }
    if (err == ESP_OK) {
        nvsKey(key, sizeof(key), save.pin, "spd");
        err = nvs_set_u8(handle, key, cfg.speed);
    }
    if (err == ESP_OK) {
        nvsKey(key, sizeof(key), save.pin, "on");
        err = nvs_set_u8(handle, key, cfg.enabled ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist channel %ld (pin %d) settings at %s: %s",
            static_cast<long>(save.id), static_cast<int>(save.pin), key, esp_err_to_name(err));
    }
    else {
        ESP_LOGD(TAG, "Persisted channel %ld (pin %d) effect config",
            static_cast<long>(save.id), static_cast<int>(save.pin));
    }
    nvs_close(handle);
}

void PixelChannel::saveToNVS() const {
    PendingSave save{ config_.pin, id_, getEffectConfigCopy() };
    writeToNVS(save);
}

void PixelChannel::loadFromNVS() {
    nvs_handle_t handle;
    // READWRITE: a legacy id-keyed record may need migrating.
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGI(TAG, "No saved config for channel %ld", static_cast<long>(id_));
        return;
    }

    // Read one key set (pin-keyed, or the legacy id-keyed layout) into
    // effect_config_. Returns true if any field was present.
    auto read_set = [&](bool legacy) {
        char key[16];
        bool found = false;
        auto make_key = [&](const char* suffix) {
            if (legacy) legacyNvsKey(key, sizeof(key), id_, suffix);
            else nvsKey(key, sizeof(key), config_.pin, suffix);
        };

        char effect_str[32] = { 0 };
        size_t len = sizeof(effect_str);
        make_key("eff");
        if (nvs_get_str(handle, key, effect_str, &len) == ESP_OK) {
            effect_config_.effect = effect_str;
            found = true;
        }

        PixelColor color;
        size_t color_size = sizeof(PixelColor);
        make_key("col");
        if (nvs_get_blob(handle, key, &color, &color_size) == ESP_OK && color_size == sizeof(PixelColor)) {
            effect_config_.color = color;
            found = true;
        }

        uint8_t val = 0;
        make_key("brt");
        if (nvs_get_u8(handle, key, &val) == ESP_OK) {
            effect_config_.brightness = val;
            found = true;
        }
        make_key("spd");
        if (nvs_get_u8(handle, key, &val) == ESP_OK) {
            effect_config_.speed = std::clamp(val, uint8_t(1), uint8_t(10));
            found = true;
        }
        make_key("on");
        if (nvs_get_u8(handle, key, &val) == ESP_OK) {
            effect_config_.enabled = (val != 0);
            found = true;
        }
        return found;
    };

    if (!read_set(false) && read_set(true)) {
        // One-time migration from the pre-stable-id layout: re-key by pin
        // and drop the abandoned id-keyed entries.
        ESP_LOGI(TAG, "Migrating channel %ld settings from id-keyed to pin-keyed (pin %d)",
            static_cast<long>(id_), static_cast<int>(config_.pin));
        nvs_close(handle);
        writeToNVS(PendingSave{ config_.pin, id_, effect_config_ });
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
            char key[16];
            for (const char* suffix : NVS_SUFFIXES) {
                legacyNvsKey(key, sizeof(key), id_, suffix);
                esp_err_t err = nvs_erase_key(handle, key);
                if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
                    ESP_LOGW(TAG, "nvs_erase_key(%s) failed: %s", key, esp_err_to_name(err));
                }
            }
            nvs_commit(handle);
            nvs_close(handle);
        }
    }
    else {
        nvs_close(handle);
    }

    resolveEffect();
}

void PixelChannel::i2sTaskWrapper(void* param) {
    static_cast<PixelChannel*>(param)->i2sTask();
}

void PixelChannel::i2sTask() {
    size_t bytes_written;

    ESP_LOGD(TAG, "I2S task started for channel %ld", static_cast<long>(id_));

    while (!terminate_task_.load(std::memory_order_acquire)) {
        if (xSemaphoreTake(transmit_semaphore_, portMAX_DELAY) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (terminate_task_.load(std::memory_order_acquire)) break;

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

        if (xSemaphoreTake(complete_semaphore_, TRANSMIT_COMPLETE_TIMEOUT) != pdTRUE) {
            ESP_LOGW(TAG, "I2S transmit completion timed out on channel %ld", static_cast<long>(id_));
        }
        i2s_channel_disable(i2s_channel_);
    }

    ESP_LOGD(TAG, "I2S task finished for channel %ld", static_cast<long>(id_));
    i2s_task_exited_.store(true, std::memory_order_release);
    vTaskDelete(nullptr);
}

// ============= HTTP API Implementation =============

#if CONFIG_KD_PIXDRIVER_HTTP_API

namespace {

    esp_err_t send_json(httpd_req_t* req, cJSON* root) {
        if (!root) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        char* json = cJSON_Print(root);
        cJSON_Delete(root);
        if (!json) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        httpd_resp_set_type(req, "application/json");
        esp_err_t ret = httpd_resp_send(req, json, strlen(json));
        free(json);
        return ret;
    }

    // Handler to list available effects
    esp_err_t led_effects_list_handler(httpd_req_t* req) {
        PixelEffectEngine* effect_engine = PixelDriver::getEffectEngine();
        if (!effect_engine) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Driver not initialized");
            return ESP_FAIL;
        }
        cJSON* root = cJSON_CreateArray();
        if (!root) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        std::vector<PixelEffectEngine::EffectInfo> effects = effect_engine->getAllEffects();

        for (const auto& eff : effects) {
            cJSON* obj = cJSON_CreateObject();
            if (!obj) continue;
            cJSON_AddStringToObject(obj, "name", eff.display_name.c_str());
            cJSON_AddStringToObject(obj, "id", eff.id.c_str());
            cJSON_AddItemToArray(root, obj);
        }
        return send_json(req, root);
    }

    // Handler to get LED configuration (includes version for WASM sync)
    esp_err_t led_config_get_handler(httpd_req_t* req) {
        cJSON* root = cJSON_CreateObject();
        if (!root) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        // Add version info for WASM bundle synchronization
        cJSON_AddStringToObject(root, "version", PIXDRIVER_GIT_COMMIT);

        cJSON* channels = cJSON_AddArrayToObject(root, "channels");
        if (channels) {
            PixelDriver::Lock lock;  // channel pointers stay valid while held
            std::vector<int32_t> channel_ids = PixelDriver::getChannelIds();
            for (size_t i = 0; i < channel_ids.size(); ++i) {
                const PixelChannel* ch = PixelDriver::getChannel(channel_ids[i]);
                if (!ch) continue;
                const ChannelConfig& config = ch->getConfig();
                cJSON* ch_obj = cJSON_CreateObject();
                if (!ch_obj) continue;
                cJSON_AddNumberToObject(ch_obj, "index", i);
                cJSON_AddNumberToObject(ch_obj, "num_leds", config.pixel_count);
                const char* type_str = "RGB";
                if (config.format == PixelFormat::RGBW) type_str = "RGBW";
                else if (config.format == PixelFormat::RGBCCT) type_str = "RGBCCT";
                cJSON_AddStringToObject(ch_obj, "type", type_str);
                cJSON_AddItemToArray(channels, ch_obj);
            }
        }
        return send_json(req, root);
    }

    int parse_channel_index(httpd_req_t* req) {
        const char* uri = req->uri;
        const char* base = "/api/led/channel/";
        if (strncmp(uri, base, strlen(base)) == 0) {
            return atoi(uri + strlen(base));
        }
        return -1;
    }

    // Handler to get a single channel configuration (GET /api/led/channel/*)
    esp_err_t led_channel_get_handler(httpd_req_t* req) {
        const int channel_idx = parse_channel_index(req);
        if (channel_idx < 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid channel index");
            return ESP_FAIL;
        }
        EffectConfig eff;
        PixelFormat fmt;
        {
            PixelDriver::Lock lock;
            const PixelChannel* ch = PixelDriver::getChannel(channel_idx);
            if (!ch) {
                httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Channel not found");
                return ESP_FAIL;
            }
            eff = ch->getEffectConfigCopy();
            fmt = ch->getConfig().format;
        }
        cJSON* ch_obj = cJSON_CreateObject();
        if (!ch_obj) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        cJSON_AddStringToObject(ch_obj, "effect_id", eff.effect.c_str());
        cJSON_AddNumberToObject(ch_obj, "brightness", eff.brightness);
        cJSON_AddNumberToObject(ch_obj, "speed", eff.speed);
        cJSON_AddBoolToObject(ch_obj, "on", eff.enabled);
        // Hex string per the documented LEDChannelState schema. On RGBW the
        // white channel is an internal detail (auto-derived by white
        // extraction), so 6-digit RGB is the full public state there. RGBCCT
        // strips drive both whites explicitly, so they are exposed as
        // separate w/cw fields (not packed into the hex).
        char color_hex[8];
        snprintf(color_hex, sizeof(color_hex), "#%02x%02x%02x",
            eff.color.r, eff.color.g, eff.color.b);
        cJSON_AddStringToObject(ch_obj, "color", color_hex);
        if (fmt == PixelFormat::RGBCCT) {
            cJSON_AddNumberToObject(ch_obj, "w", eff.color.w);    // warm white
            cJSON_AddNumberToObject(ch_obj, "cw", eff.color.cw);  // cool white
        }
        return send_json(req, ch_obj);
    }

    // Handler to configure a channel (POST /api/led/channel/*)
    esp_err_t led_channel_config_handler(httpd_req_t* req) {
        const int channel_idx = parse_channel_index(req);
        if (channel_idx < 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid channel index");
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

        PixelDriver::Lock lock;  // keep the channel alive across get/set
        PixelChannel* ch = PixelDriver::getChannel(channel_idx);
        if (!ch) {
            cJSON_Delete(json);
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Channel not found");
            return ESP_FAIL;
        }

        // Set effect config
        EffectConfig eff_cfg = ch->getEffectConfigCopy();
        if (effect_id && cJSON_IsString(effect_id) && effect_id->valuestring) {
            eff_cfg.effect = effect_id->valuestring;
        }
        if (brightness && cJSON_IsNumber(brightness)) eff_cfg.brightness = brightness->valueint;
        if (speed && cJSON_IsNumber(speed)) eff_cfg.speed = speed->valueint;
        if (on && cJSON_IsBool(on)) eff_cfg.enabled = cJSON_IsTrue(on);
        if (color && cJSON_IsString(color) && color->valuestring) {
            // Documented form: "#rrggbb" (optionally "#rrggbbww").
            const char* s = color->valuestring;
            if (*s == '#') s++;
            unsigned r = 0, g = 0, b = 0, w = 0;
            const size_t len = strlen(s);
            if (len == 6 && sscanf(s, "%02x%02x%02x", &r, &g, &b) == 3) {
                eff_cfg.color = PixelColor(static_cast<uint8_t>(r),
                    static_cast<uint8_t>(g), static_cast<uint8_t>(b));
            }
            else if (len == 8 && sscanf(s, "%02x%02x%02x%02x", &r, &g, &b, &w) == 4) {
                eff_cfg.color = PixelColor(static_cast<uint8_t>(r),
                    static_cast<uint8_t>(g), static_cast<uint8_t>(b),
                    static_cast<uint8_t>(w));
            }
        }
        else if (color && cJSON_IsObject(color)) {
            // Legacy object form {r,g,b,w} - kept for older API consumers
            cJSON* r = cJSON_GetObjectItem(color, "r");
            cJSON* g = cJSON_GetObjectItem(color, "g");
            cJSON* b = cJSON_GetObjectItem(color, "b");
            cJSON* w = cJSON_GetObjectItem(color, "w");
            if (cJSON_IsNumber(r)) eff_cfg.color.r = r->valueint;
            if (cJSON_IsNumber(g)) eff_cfg.color.g = g->valueint;
            if (cJSON_IsNumber(b)) eff_cfg.color.b = b->valueint;
            if (w && cJSON_IsNumber(w)) eff_cfg.color.w = w->valueint;
        }

        // RGBCCT white channels: top-level "w" (warm) / "cw" (cool), matching
        // the fields the channel GET returns. 0-255 each.
        cJSON* w_top = cJSON_GetObjectItem(json, "w");
        cJSON* cw_top = cJSON_GetObjectItem(json, "cw");
        if (w_top && cJSON_IsNumber(w_top)) {
            eff_cfg.color.w = static_cast<uint8_t>(w_top->valueint);
        }
        if (cw_top && cJSON_IsNumber(cw_top)) {
            eff_cfg.color.cw = static_cast<uint8_t>(cw_top->valueint);
        }

        ch->setEffect(eff_cfg);

        cJSON_Delete(json);
        return led_channel_get_handler(req); // Return updated config
    }

} // anonymous namespace

void PixelDriver::attach_api(httpd_handle_t server, uri_register_fn register_fn) {
    if (!server) {
        ESP_LOGE(TAG, "attach_api: no server handle");
        return;
    }
    if (!register_fn) register_fn = httpd_register_uri_handler;

    static httpd_uri_t effects_uri = {
        .uri = "/api/led/effects",
        .method = HTTP_GET,
        .handler = led_effects_list_handler,
        .user_ctx = NULL
    };
    register_fn(server, &effects_uri);

    static httpd_uri_t config_uri = {
        .uri = "/api/led/config",
        .method = HTTP_GET,
        .handler = led_config_get_handler,
        .user_ctx = NULL
    };
    register_fn(server, &config_uri);

    static httpd_uri_t channel_get_uri = {
        .uri = "/api/led/channel/*",
        .method = HTTP_GET,
        .handler = led_channel_get_handler,
        .user_ctx = NULL
    };
    register_fn(server, &channel_get_uri);

    static httpd_uri_t channel_post_uri = {
        .uri = "/api/led/channel/*",
        .method = HTTP_POST,
        .handler = led_channel_config_handler,
        .user_ctx = NULL
    };
    register_fn(server, &channel_post_uri);

    ESP_LOGI(TAG, "LED API attached (version: %s)", PIXDRIVER_GIT_COMMIT);
}

#else  // !CONFIG_KD_PIXDRIVER_HTTP_API

void PixelDriver::attach_api(httpd_handle_t server, uri_register_fn register_fn) {
    (void)server;
    (void)register_fn;
    ESP_LOGW(TAG, "Built-in LED HTTP API disabled (CONFIG_KD_PIXDRIVER_HTTP_API=n); nothing registered");
}

#endif  // CONFIG_KD_PIXDRIVER_HTTP_API
