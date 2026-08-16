#include "inmp441_audio.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#define MIC_READ_WORDS 256U
#define MIC_WORDS_PER_FRAME 2U
#define MIC_WARMUP_US 500000LL
#define MIC_CALIBRATION_US 1500000LL
#define MIC_READ_TIMEOUT_MS 1000U
#define MIC_LEVEL_REPORT_FRAMES CONFIG_MIC_TEST_SAMPLE_RATE_HZ

static const char *TAG = "inmp441";

/* Static buffers keep microphone capture off the backend task's stack. */
static int32_t s_i2s_samples[MIC_READ_WORDS];
static int16_t s_pcm_samples[MIC_READ_WORDS / MIC_WORDS_PER_FRAME];
static int s_active_slot = -1;

uint32_t inmp441_sample_rate_hz(void)
{
    return CONFIG_MIC_TEST_SAMPLE_RATE_HZ;
}

static bool create_i2s_rx(i2s_chan_handle_t *rx_channel)
{
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    esp_err_t error = i2s_new_channel(&channel_config, NULL, rx_channel);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate I2S RX channel: %s", esp_err_to_name(error));
        return false;
    }

    const i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_MIC_TEST_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_MIC_TEST_SCK_GPIO,
            .ws = CONFIG_MIC_TEST_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = CONFIG_MIC_TEST_SD_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    error = i2s_channel_init_std_mode(*rx_channel, &standard_config);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure I2S RX: %s", esp_err_to_name(error));
        i2s_del_channel(*rx_channel);
        *rx_channel = NULL;
        return false;
    }
    return true;
}

static bool release_i2s_rx(i2s_chan_handle_t rx_channel)
{
    bool success = true;
    esp_err_t error = i2s_channel_disable(rx_channel);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to disable I2S RX: %s", esp_err_to_name(error));
        success = false;
    }
    error = i2s_del_channel(rx_channel);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to release I2S RX: %s", esp_err_to_name(error));
        success = false;
    }
    return success;
}

static size_t analyze_block(size_t word_count,
                            int32_t block_mean[2],
                            uint32_t block_peak[2])
{
    const size_t frame_count = word_count / MIC_WORDS_PER_FRAME;
    if (frame_count == 0) {
        return 0;
    }

    int64_t sums[2] = {0, 0};
    for (size_t frame = 0; frame < frame_count; ++frame) {
        for (size_t slot = 0; slot < MIC_WORDS_PER_FRAME; ++slot) {
            sums[slot] += s_i2s_samples[(frame * MIC_WORDS_PER_FRAME) + slot] >> 8;
        }
    }
    block_mean[0] = (int32_t)(sums[0] / (int64_t)frame_count);
    block_mean[1] = (int32_t)(sums[1] / (int64_t)frame_count);

    block_peak[0] = 0;
    block_peak[1] = 0;
    for (size_t frame = 0; frame < frame_count; ++frame) {
        for (size_t slot = 0; slot < MIC_WORDS_PER_FRAME; ++slot) {
            const int32_t sample =
                s_i2s_samples[(frame * MIC_WORDS_PER_FRAME) + slot] >> 8;
            int64_t deviation = (int64_t)sample - block_mean[slot];
            if (deviation < 0) {
                deviation = -deviation;
            }
            if ((uint32_t)deviation > block_peak[slot]) {
                block_peak[slot] = (uint32_t)deviation;
            }
        }
    }
    return frame_count;
}

bool inmp441_prepare(void)
{
    i2s_chan_handle_t rx_channel = NULL;
    if (!create_i2s_rx(&rx_channel)) {
        return false;
    }
    esp_err_t error = i2s_channel_enable(rx_channel);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S RX: %s", esp_err_to_name(error));
        i2s_del_channel(rx_channel);
        return false;
    }

    ESP_LOGI(TAG,
             "Microphone ready: %d Hz, WS GPIO%d, SCK GPIO%d, SD GPIO%d",
             CONFIG_MIC_TEST_SAMPLE_RATE_HZ,
             CONFIG_MIC_TEST_WS_GPIO,
             CONFIG_MIC_TEST_SCK_GPIO,
             CONFIG_MIC_TEST_SD_GPIO);
    ESP_LOGI(TAG, "Keep quiet for 2 seconds while the microphone slot is calibrated");

    const int64_t start_us = esp_timer_get_time();
    const int64_t warmup_end_us = start_us + MIC_WARMUP_US;
    const int64_t calibration_end_us = warmup_end_us + MIC_CALIBRATION_US;
    uint32_t calibration_peak[2] = {0, 0};

    while (esp_timer_get_time() < calibration_end_us) {
        size_t bytes_read = 0;
        error = i2s_channel_read(rx_channel,
                                 s_i2s_samples,
                                 sizeof(s_i2s_samples),
                                 &bytes_read,
                                 MIC_READ_TIMEOUT_MS);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "I2S read failed during calibration: %s", esp_err_to_name(error));
            release_i2s_rx(rx_channel);
            return false;
        }
        if (esp_timer_get_time() < warmup_end_us) {
            continue;
        }

        int32_t block_mean[2] = {0, 0};
        uint32_t block_peak[2] = {0, 0};
        analyze_block(bytes_read / sizeof(s_i2s_samples[0]), block_mean, block_peak);
        for (size_t slot = 0; slot < MIC_WORDS_PER_FRAME; ++slot) {
            if (block_peak[slot] > calibration_peak[slot]) {
                calibration_peak[slot] = block_peak[slot];
            }
        }
    }

    const size_t detected_slot = calibration_peak[1] > calibration_peak[0] ? 1U : 0U;
    if (calibration_peak[detected_slot] == 0) {
        ESP_LOGE(TAG, "Microphone stream is all zero; check power and GPIO4/5/6 wiring");
        release_i2s_rx(rx_channel);
        return false;
    }

    s_active_slot = (int)detected_slot;
    const bool released = release_i2s_rx(rx_channel);
    if (released) {
        ESP_LOGI(TAG,
                 "Microphone preparation passed: active SLOT%u, calibration peak=%lu",
                 (unsigned int)detected_slot,
                 (unsigned long)calibration_peak[detected_slot]);
    }
    return released;
}

bool inmp441_capture_pcm(uint32_t maximum_duration_ms,
                         inmp441_continue_cb_t should_continue,
                         void *continue_context,
                         inmp441_pcm_sink_t sink,
                         void *sink_context,
                         uint32_t *captured_frame_count)
{
    if (maximum_duration_ms == 0 || sink == NULL) {
        ESP_LOGE(TAG, "Maximum recording duration and PCM sink must be valid");
        return false;
    }
    if (s_active_slot < 0 && !inmp441_prepare()) {
        return false;
    }

    i2s_chan_handle_t rx_channel = NULL;
    if (!create_i2s_rx(&rx_channel)) {
        return false;
    }
    esp_err_t error = i2s_channel_enable(rx_channel);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S RX: %s", esp_err_to_name(error));
        i2s_del_channel(rx_channel);
        return false;
    }

    const uint64_t target_frames =
        ((uint64_t)CONFIG_MIC_TEST_SAMPLE_RATE_HZ * maximum_duration_ms) / 1000U;
    uint64_t captured_frames = 0;
    uint64_t report_absolute_sum = 0;
    uint32_t report_peak = 0;
    uint32_t report_frames = 0;
    const size_t active_slot = (size_t)s_active_slot;

    ESP_LOGI(TAG,
             "RECORDING START: hold the talk button, maximum %lu ms "
             "(active SLOT%u, digital gain x%d)",
             (unsigned long)maximum_duration_ms,
             (unsigned int)active_slot,
             CONFIG_MIC_RECORD_DIGITAL_GAIN);

    while (captured_frames < target_frames &&
           (should_continue == NULL || should_continue(continue_context))) {
        size_t bytes_read = 0;
        error = i2s_channel_read(rx_channel,
                                 s_i2s_samples,
                                 sizeof(s_i2s_samples),
                                 &bytes_read,
                                 MIC_READ_TIMEOUT_MS);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "I2S read failed while recording: %s", esp_err_to_name(error));
            release_i2s_rx(rx_channel);
            return false;
        }

        int32_t block_mean[2] = {0, 0};
        uint32_t block_peak[2] = {0, 0};
        size_t frame_count = analyze_block(bytes_read / sizeof(s_i2s_samples[0]),
                                           block_mean,
                                           block_peak);
        const uint64_t frames_remaining = target_frames - captured_frames;
        if ((uint64_t)frame_count > frames_remaining) {
            frame_count = (size_t)frames_remaining;
        }
        if (frame_count == 0) {
            continue;
        }

        for (size_t frame = 0; frame < frame_count; ++frame) {
            const int32_t sample24 =
                s_i2s_samples[(frame * MIC_WORDS_PER_FRAME) + active_slot] >> 8;
            const int64_t centered = (int64_t)sample24 - block_mean[active_slot];
            int64_t pcm = (centered * CONFIG_MIC_RECORD_DIGITAL_GAIN) >> 8;
            if (pcm > INT16_MAX) {
                pcm = INT16_MAX;
            } else if (pcm < INT16_MIN) {
                pcm = INT16_MIN;
            }
            s_pcm_samples[frame] = (int16_t)pcm;

            const uint32_t magnitude = pcm < 0 ? (uint32_t)(-pcm) : (uint32_t)pcm;
            report_absolute_sum += magnitude;
            if (magnitude > report_peak) {
                report_peak = magnitude;
            }
        }

        if (!sink(sink_context, s_pcm_samples, frame_count)) {
            ESP_LOGE(TAG, "PCM upload sink rejected microphone data");
            release_i2s_rx(rx_channel);
            return false;
        }
        captured_frames += frame_count;
        report_frames += (uint32_t)frame_count;

        if (report_frames >= MIC_LEVEL_REPORT_FRAMES) {
            ESP_LOGI(TAG,
                     "Recording level: avg=%lu peak=%lu, %llu/%llu frames",
                     (unsigned long)(report_absolute_sum / report_frames),
                     (unsigned long)report_peak,
                     (unsigned long long)captured_frames,
                     (unsigned long long)target_frames);
            report_absolute_sum = 0;
            report_peak = 0;
            report_frames = 0;
        }
    }

    const bool released = release_i2s_rx(rx_channel);
    if (captured_frame_count != NULL) {
        *captured_frame_count = (uint32_t)captured_frames;
    }
    ESP_LOGI(TAG,
             "RECORDING END: captured %llu PCM frames (%lu bytes)",
             (unsigned long long)captured_frames,
             (unsigned long)(captured_frames * sizeof(int16_t)));
    return released;
}
