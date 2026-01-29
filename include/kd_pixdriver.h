#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <functional>
#include <string>
#include <string_view>
#include "driver/gpio.h"
#include "driver/i2s_std.h"
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

    ChannelConfig(gpio_num_t gpio_pin, uint16_t count,
                  PixelFormat fmt = PixelFormat::RGB,
                  std::string_view channel_name = "")
        : pin(gpio_pin), pixel_count(count), format(fmt), name(channel_name) {}
};

struct EffectConfig {
    std::string effect = "SOLID";
    PixelColor color{100, 100, 100, 0};
    uint8_t brightness = 255;
    uint8_t speed = 5;  // 1-10 scale
    bool enabled = true;
    std::vector<uint8_t> mask;

    // Custom effect callback (optional)
    std::function<void(std::vector<PixelColor>&, uint32_t)> custom_effect;
};

class PixelDriver {
public:
    static constexpr uint8_t CURRENT_PER_CHANNEL_MA = 20;
    static constexpr uint32_t SYSTEM_RESERVE_MA = 400;

    // Initialization
    static void initialize(uint32_t update_rate_hz = 60);
    static void shutdown();

    // Channel management
    static int32_t addChannel(const ChannelConfig& config);
    static bool removeChannel(int32_t channel_id);
    [[nodiscard]] static PixelChannel* getChannel(int32_t channel_id);
    [[nodiscard]] static PixelChannel* getMainChannel();
    [[nodiscard]] static std::vector<int32_t> getChannelIds();
    [[nodiscard]] static PixelEffectEngine* getEffectEngine() noexcept { return effect_engine_.get(); }

    // Global settings
    static void setCurrentLimit(int32_t limit_ma);
    [[nodiscard]] static int32_t getCurrentLimit() noexcept;
    static void setUpdateRate(uint32_t rate_hz);
    [[nodiscard]] static uint32_t getUpdateRate() noexcept;

    // Control
    static void start();
    static void stop();
    [[nodiscard]] static bool isRunning() noexcept;

    // Batch operations
    static void setAllChannelsEffect(std::string_view effect_id);
    static void setAllChannelsColor(const PixelColor& color);
    static void setAllChannelsBrightness(uint8_t brightness);
    static void setAllChannelsEnabled(bool enabled);

    // Power management
    [[nodiscard]] static uint32_t getTotalCurrentConsumption();
    [[nodiscard]] static uint32_t getScaledCurrentConsumption();
    [[nodiscard]] static float getCurrentScaleFactor();

private:
    PixelDriver() = delete;
    ~PixelDriver() = delete;
    PixelDriver(const PixelDriver&) = delete;
    PixelDriver& operator=(const PixelDriver&) = delete;

    static void driverTask(void* param);
    static void applyCurrentLimiting();

    static std::vector<std::unique_ptr<PixelChannel>> channels_;
    static std::unique_ptr<PixelEffectEngine> effect_engine_;
    static int32_t main_channel_id_;
    static TaskHandle_t task_handle_;
    static int32_t current_limit_ma_;
    static uint32_t update_rate_hz_;
    static bool running_;
    static int32_t next_channel_id_;
    static bool initialized_;
};

class PixelChannel {
public:
    PixelChannel(int32_t id, const ChannelConfig& config);
    ~PixelChannel();

    // Non-copyable, movable
    PixelChannel(const PixelChannel&) = delete;
    PixelChannel& operator=(const PixelChannel&) = delete;
    PixelChannel(PixelChannel&&) = default;
    PixelChannel& operator=(PixelChannel&&) = default;

    // Friend for I2S callback
    friend bool i2s_tx_callback(i2s_chan_handle_t handle, i2s_event_data_t* event, void* user_ctx);

    // Getters
    [[nodiscard]] int32_t getId() const noexcept { return id_; }
    [[nodiscard]] const ChannelConfig& getConfig() const noexcept { return config_; }
    [[nodiscard]] const EffectConfig& getEffectConfig() const noexcept { return effect_config_; }

    // Effect control
    void setEffect(const EffectConfig& config);
    void setEffectByID(std::string_view effect_id);
    void setColor(const PixelColor& color) noexcept;
    void setBrightness(uint8_t brightness) noexcept;
    void setSpeed(uint8_t speed) noexcept;
    void setEnabled(bool enabled) noexcept;
    void setMask(const std::vector<uint8_t>& mask);
    void clearMask() noexcept;

    // Buffer access
    [[nodiscard]] const std::vector<PixelColor>& getPixelBuffer() const noexcept { return pixel_buffer_; }
    [[nodiscard]] std::vector<PixelColor>& getPixelBuffer() noexcept { return pixel_buffer_; }

    // Hardware interface
    bool initialize();
    void transmit();
    [[nodiscard]] uint32_t getCurrentConsumption() const noexcept;
    void applyCurrentScaling(float scale_factor);

    // Persistence
    void saveToNVS() const;
    void loadFromNVS();

private:
    void setupI2S();
    void cleanup();
    void convertToI2SBuffer(const std::vector<PixelColor>& pixels);
    static void i2sTaskWrapper(void* param);
    void i2sTask();

    int32_t id_;
    ChannelConfig config_;
    EffectConfig effect_config_;

    std::vector<PixelColor> pixel_buffer_;
    std::vector<PixelColor> scaled_buffer_;
    std::vector<uint8_t> i2s_buffer_;

    i2s_chan_handle_t i2s_channel_ = nullptr;
    SemaphoreHandle_t transmit_semaphore_ = nullptr;
    SemaphoreHandle_t complete_semaphore_ = nullptr;
    TaskHandle_t i2s_task_handle_ = nullptr;
    bool initialized_ = false;
    bool terminate_task_ = false;
    size_t bytes_sent_ = 0;
};
