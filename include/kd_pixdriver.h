#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>
#include <functional>
#include <string>
#include <string_view>
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "pixel_core.h"

// Forward declarations
class PixelChannel;
class PixelEffectEngine;

// I2S callback function
extern "C" bool i2s_tx_callback(i2s_chan_handle_t handle, i2s_event_data_t* event, void* user_ctx);

struct ChannelConfig {
    gpio_num_t pin;
    uint16_t pixel_count;
    PixelFormat format = PixelFormat::RGB;
    uint32_t resolution_hz = 10000000;  // 10MHz default
    std::string name;

    // Strip wiring (per-IC). color_order reorders the R/G/B bytes; white_swap
    // flips the order of the two white bytes (RGBCCT only). ic_type is mostly
    // informational since all supported ICs share WS2812 800kHz timing.
    LedICType ic_type = LedICType::WS2812;
    ColorOrder color_order = ColorOrder::GRB;  // WS2812 default
    bool white_swap = false;

    ChannelConfig(gpio_num_t gpio_pin, uint16_t count,
                  PixelFormat fmt = PixelFormat::RGB,
                  std::string_view channel_name = "",
                  LedICType ic = LedICType::WS2812,
                  ColorOrder order = ColorOrder::GRB,
                  bool swap_white = false)
        : pin(gpio_pin), pixel_count(count), format(fmt), name(channel_name),
          ic_type(ic), color_order(order), white_swap(swap_white) {}
};

struct EffectConfig {
    std::string effect = "SOLID";
    PixelColor color{100, 100, 100, 0};
    uint8_t brightness = 255;
    uint8_t speed = 5;  // 1-10 scale
    bool enabled = true;
    std::vector<uint8_t> mask;

    // Custom effect callback (optional). When set it replaces the registered
    // effect named by `effect`: called once per frame on the driver task
    // (under PixelDriver::Lock) with the channel's pixel buffer and the frame
    // tick. Render at full scale - brightness and current limiting are
    // applied afterwards by the driver. Must not block.
    std::function<void(std::vector<PixelColor>&, uint32_t)> custom_effect;
};

class PixelDriver {
public:
    static constexpr uint8_t CURRENT_PER_CHANNEL_MA = 20;
    static constexpr uint32_t SYSTEM_RESERVE_MA = 400;

    /// Recursive RAII lock serialising the channel list and every channel's
    /// effect config across tasks. Every PixelDriver / PixelChannel entry
    /// point takes it internally (recursively, so a caller already holding
    /// it may call them); the driver task holds it for the CPU part of each
    /// frame (effects, current limiting, I2S encoding) and releases it while
    /// it sleeps between frames.
    ///
    /// Hold it yourself when a raw PixelChannel* from getChannel() must stay
    /// valid across several calls, or when a stop/remove/add/start sequence
    /// must be atomic to other tasks. stop() may be called with it held.
    /// Never take it from an ISR.
    struct Lock {
        Lock();
        ~Lock();
        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;
    };

    // Initialization
    static void initialize(uint32_t update_rate_hz = 60);
    static void shutdown();

    // Channel management. Channel ids are stable: the lowest free id is
    // reused, so the app's stop -> removeChannel -> addChannel -> start
    // reconfigure sequence yields the same id and finds the same persisted
    // settings (which are keyed by the data pin, not the id).
    static int32_t addChannel(const ChannelConfig& config);
    /// Remove a channel. Persisted effect settings are kept (and a pending
    /// unsaved change is flushed first) unless persist_forget is true, in
    /// which case the channel's NVS keys are erased.
    static bool removeChannel(int32_t channel_id, bool persist_forget = false);
    /// Erase the persisted effect settings for the strip on `pin`.
    static bool forgetChannelSettings(gpio_num_t pin);
    /// Raw pointer valid only while the caller holds PixelDriver::Lock (or
    /// for the duration of a single call from a task that does not remove
    /// channels). Prefer getEffectConfigCopy() for reads from other tasks.
    [[nodiscard]] static PixelChannel* getChannel(int32_t channel_id);
    [[nodiscard]] static PixelChannel* getMainChannel();
    [[nodiscard]] static std::vector<int32_t> getChannelIds();
    [[nodiscard]] static PixelEffectEngine* getEffectEngine() noexcept { return effect_engine_.get(); }

    // Global settings
    static void setCurrentLimit(int32_t limit_ma);
    [[nodiscard]] static int32_t getCurrentLimit() noexcept;
    /// Only while stopped (the render period and effect engine are rebuilt).
    /// False (and no change) if the driver is running or rate_hz is 0.
    static bool setUpdateRate(uint32_t rate_hz);
    [[nodiscard]] static uint32_t getUpdateRate() noexcept;

    // Control
    /// False if the driver task could not be created (running_ stays false).
    static bool start();
    /// Asks the driver task to exit after its current frame and joins it
    /// (bounded wait; logs an error and returns if it does not exit in time).
    /// Safe with PixelDriver::Lock held; from inside an effect callback it
    /// only flags the task, which exits after the frame.
    static void stop();
    [[nodiscard]] static bool isRunning() noexcept;

    // Batch operations
    static void setAllChannelsEffect(std::string_view effect_id);
    static void setAllChannelsColor(const PixelColor& color);
    static void setAllChannelsBrightness(uint8_t brightness);
    static void setAllChannelsEnabled(bool enabled);

    // Power management. Total = what the strips would draw after brightness
    // but before current limiting (the limiter's input); scaled = what is
    // actually being driven this frame.
    [[nodiscard]] static uint32_t getTotalCurrentConsumption();
    [[nodiscard]] static uint32_t getScaledCurrentConsumption();
    [[nodiscard]] static float getCurrentScaleFactor();

    // HTTP API (compiled in only with CONFIG_KD_PIXDRIVER_HTTP_API; otherwise
    // a logging no-op so callers still link).
    // register_fn lets the app route registration through a wrapper (e.g.
    // kd_common_api_register_uri_handler for CORS); defaults to the bare
    // httpd_register_uri_handler.
    using uri_register_fn = esp_err_t (*)(httpd_handle_t, const httpd_uri_t*);
    static void attach_api(httpd_handle_t server, uri_register_fn register_fn = nullptr);

private:
    PixelDriver() = delete;
    ~PixelDriver() = delete;
    PixelDriver(const PixelDriver&) = delete;
    PixelDriver& operator=(const PixelDriver&) = delete;

    static void driverTask(void* param);
    static void applyCurrentLimiting();
    [[nodiscard]] static uint32_t currentLimitScaleQ8();

    static std::vector<std::unique_ptr<PixelChannel>> channels_;
    static std::unique_ptr<PixelEffectEngine> effect_engine_;
    static int32_t main_channel_id_;
    static TaskHandle_t task_handle_;
    static SemaphoreHandle_t task_exit_sem_;  // given by the driver task as it exits
    static int32_t current_limit_ma_;
    static uint32_t update_rate_hz_;
    static std::atomic<bool> running_;     // request: keep rendering
    static std::atomic<bool> task_alive_;  // the driver task has not exited yet
    static bool initialized_;
};

class PixelChannel {
public:
    PixelChannel(int32_t id, const ChannelConfig& config);
    ~PixelChannel();

    // Neither copyable nor movable: the I2S task and ISR hold `this`.
    PixelChannel(const PixelChannel&) = delete;
    PixelChannel& operator=(const PixelChannel&) = delete;
    PixelChannel(PixelChannel&&) = delete;
    PixelChannel& operator=(PixelChannel&&) = delete;

    // Friend for I2S callback
    friend bool i2s_tx_callback(i2s_chan_handle_t handle, i2s_event_data_t* event, void* user_ctx);

    // Getters
    [[nodiscard]] int32_t getId() const noexcept { return id_; }
    [[nodiscard]] const ChannelConfig& getConfig() const noexcept { return config_; }
    /// Live effect config by reference. Only valid while the caller holds
    /// PixelDriver::Lock or runs inside an effect callback on the driver task
    /// (which holds the lock for the whole frame). Any other task must use
    /// getEffectConfigCopy(): the string/mask members are reassigned by the
    /// setters and a concurrent read would tear.
    [[nodiscard]] const EffectConfig& getEffectConfig() const noexcept { return effect_config_; }
    /// Snapshot of the effect config taken under PixelDriver::Lock.
    [[nodiscard]] EffectConfig getEffectConfigCopy() const;

    // Effect control (each takes PixelDriver::Lock)
    void setEffect(const EffectConfig& config);
    void setEffectByID(std::string_view effect_id);
    void setColor(const PixelColor& color) noexcept;
    void setBrightness(uint8_t brightness) noexcept;
    void setSpeed(uint8_t speed) noexcept;
    void setEnabled(bool enabled) noexcept;
    void setMask(const std::vector<uint8_t>& mask);
    void clearMask() noexcept;

    // Buffer access (driver task / effect callbacks; otherwise hold the lock)
    [[nodiscard]] const std::vector<PixelColor>& getPixelBuffer() const noexcept { return pixel_buffer_; }
    [[nodiscard]] std::vector<PixelColor>& getPixelBuffer() noexcept { return pixel_buffer_; }

    // Hardware interface
    bool initialize();
    void transmit();
    /// mA the strip would draw from the current pixel buffer at the channel's
    /// brightness, before current limiting (the limiter's input).
    [[nodiscard]] uint32_t getCurrentConsumption() const noexcept;
    /// mA actually driven this frame (from the scaled buffer).
    [[nodiscard]] uint32_t getScaledCurrentConsumption() const noexcept;
    void applyCurrentScaling(float scale_factor);
    /// Integer form: limit_q8 = 256 means no limiting. Brightness is folded
    /// in here (the only place it is applied).
    void applyScalingQ8(uint32_t limit_q8);

    // Persistence (keyed by the data pin: "ch_p<pin>:<field>")
    void saveToNVS() const;
    void loadFromNVS();

    /// Persist the effect config once it has stopped changing. Debounced so
    /// slider drags / rapid API calls become one NVS write instead of one
    /// per intermediate value. Call from a task that does NOT hold
    /// PixelDriver::Lock across the call; the driver task uses
    /// takeSettledSave() + writeToNVS() so the flash write happens outside
    /// the frame and the lock.
    void persistIfSettled();

    /// A config copy queued for an NVS write outside the lock.
    struct PendingSave {
        gpio_num_t pin;
        int32_t id;
        EffectConfig config;
    };
    /// Under PixelDriver::Lock: if the config is dirty and has been stable for
    /// the settle window, clear the dirty flag and append a copy to `out`.
    bool takeSettledSave(std::vector<PendingSave>& out);
    static void writeToNVS(const PendingSave& save);
    /// Write a still-dirty config now (used before the channel goes away).
    void flushPendingSave();

    /// Effect dispatch index cached from the engine's registry; re-resolved
    /// when the registry generation changes. Used by PixelEffectEngine.
    [[nodiscard]] int effectIndex(const PixelEffectEngine& engine);

private:
    /// Mark the effect config as needing persistence (any setter/API write)
    void markConfigDirty() noexcept;
    /// Resolve effect_config_.effect to an engine index (caller holds the lock)
    void resolveEffect();

    void setupI2S();
    void cleanup();
    void convertToI2SBuffer(const std::vector<PixelColor>& pixels);
    static void i2sTaskWrapper(void* param);
    void i2sTask();

    int32_t id_;
    ChannelConfig config_;
    EffectConfig effect_config_;
    int effect_index_;
    uint32_t effect_registry_gen_;

    std::vector<PixelColor> pixel_buffer_;
    std::vector<PixelColor> scaled_buffer_;
    std::vector<uint8_t> i2s_buffer_;

    i2s_chan_handle_t i2s_channel_ = nullptr;
    SemaphoreHandle_t transmit_semaphore_ = nullptr;
    SemaphoreHandle_t complete_semaphore_ = nullptr;
    TaskHandle_t i2s_task_handle_ = nullptr;
    bool initialized_ = false;
    std::atomic<bool> terminate_task_{ false };
    std::atomic<bool> i2s_task_exited_{ false };
    volatile size_t bytes_sent_ = 0;

    // Deferred NVS persistence: setters (API/WS/schedule tasks) mark dirty,
    // the driver task saves after the config has been stable for a while.
    std::atomic<bool> nvs_dirty_{ false };
    std::atomic<int64_t> nvs_dirty_at_us_{ 0 };
};
