#include "max98357_audio.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define AUDIO_BUFFER_SIZE 4096U
#define AUDIO_WRITE_TIMEOUT_MS 1000U
#define MAX_WAV_CHUNK_BYTES (4U * 1024U * 1024U)

static const char *TAG = "max98357";
/*
 * Keep the streaming buffers out of the calling task's stack.  The ESP-IDF
 * main task is configured with a 3584-byte stack in this project, while the
 * audio buffer alone is 4096 bytes.  Allocating it locally corrupts the task
 * stack before the first I2S sample can be written.
 */
static uint8_t s_audio_buffer[AUDIO_BUFFER_SIZE];
static const int16_t s_silence[320] = {0};

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static bool read_exact(max98357_audio_read_cb_t read_cb,
                       void *context,
                       uint8_t *buffer,
                       size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        const int read_bytes = read_cb(context, buffer + offset, size - offset);
        if (read_bytes <= 0) {
            ESP_LOGE(TAG, "WAV stream ended at byte %u/%u", (unsigned int)offset, (unsigned int)size);
            return false;
        }
        offset += (size_t)read_bytes;
    }
    return true;
}

static bool discard_bytes(max98357_audio_read_cb_t read_cb, void *context, size_t size)
{
    uint8_t discard[128];
    while (size > 0) {
        const size_t requested = size < sizeof(discard) ? size : sizeof(discard);
        if (!read_exact(read_cb, context, discard, requested)) {
            return false;
        }
        size -= requested;
    }
    return true;
}

static bool create_i2s_tx(uint32_t sample_rate_hz, i2s_chan_handle_t *tx_channel)
{
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    esp_err_t error = i2s_new_channel(&channel_config, tx_channel, NULL);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate I2S TX channel: %s", esp_err_to_name(error));
        return false;
    }

    const i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_AUDIO_TEST_BCLK_GPIO,
            .ws = CONFIG_AUDIO_TEST_LRCLK_GPIO,
            .dout = CONFIG_AUDIO_TEST_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    error = i2s_channel_init_std_mode(*tx_channel, &standard_config);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure I2S standard mode: %s", esp_err_to_name(error));
        i2s_del_channel(*tx_channel);
        *tx_channel = NULL;
        return false;
    }
    return true;
}

static bool write_all(i2s_chan_handle_t tx_channel, const uint8_t *data, size_t data_size)
{
    size_t offset = 0;
    while (offset < data_size) {
        size_t written = 0;
        const esp_err_t error = i2s_channel_write(tx_channel,
                                                  data + offset,
                                                  data_size - offset,
                                                  &written,
                                                  AUDIO_WRITE_TIMEOUT_MS);
        if (error != ESP_OK || written == 0) {
            ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(error));
            return false;
        }
        offset += written;
    }
    return true;
}

bool max98357_play_wav_stream(max98357_audio_read_cb_t read_cb, void *context)
{
    uint8_t riff_header[12];
    if (read_cb == NULL || !read_exact(read_cb, context, riff_header, sizeof(riff_header)) ||
        memcmp(riff_header, "RIFF", 4) != 0 || memcmp(riff_header + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Response is not a RIFF/WAVE stream");
        return false;
    }

    bool format_found = false;
    uint16_t audio_format = 0;
    uint16_t channel_count = 0;
    uint16_t bits_per_sample = 0;
    uint32_t sample_rate_hz = 0;
    uint32_t data_bytes = 0;

    while (data_bytes == 0) {
        uint8_t chunk_header[8];
        if (!read_exact(read_cb, context, chunk_header, sizeof(chunk_header))) {
            return false;
        }
        const uint32_t chunk_size = read_le32(chunk_header + 4);
        if (chunk_size > MAX_WAV_CHUNK_BYTES) {
            ESP_LOGE(TAG, "WAV chunk is too large: %lu", (unsigned long)chunk_size);
            return false;
        }

        if (memcmp(chunk_header, "fmt ", 4) == 0) {
            uint8_t format[16];
            if (chunk_size < sizeof(format) ||
                !read_exact(read_cb, context, format, sizeof(format)) ||
                !discard_bytes(read_cb, context, chunk_size - sizeof(format))) {
                return false;
            }
            audio_format = read_le16(format);
            channel_count = read_le16(format + 2);
            sample_rate_hz = read_le32(format + 4);
            bits_per_sample = read_le16(format + 14);
            format_found = true;
        } else if (memcmp(chunk_header, "data", 4) == 0) {
            data_bytes = chunk_size;
        } else if (!discard_bytes(read_cb, context, chunk_size)) {
            return false;
        }

        if ((chunk_size & 1U) != 0 && data_bytes == 0 && !discard_bytes(read_cb, context, 1)) {
            return false;
        }
    }

    if (!format_found || audio_format != 1 || channel_count != 1 || bits_per_sample != 16 ||
        sample_rate_hz < 8000 || sample_rate_hz > 96000 || (data_bytes & 1U) != 0) {
        ESP_LOGE(TAG,
                 "Unsupported WAV: PCM=%u, channels=%u, rate=%lu, bits=%u, bytes=%lu",
                 audio_format,
                 channel_count,
                 (unsigned long)sample_rate_hz,
                 bits_per_sample,
                 (unsigned long)data_bytes);
        return false;
    }

    i2s_chan_handle_t tx_channel = NULL;
    if (!create_i2s_tx(sample_rate_hz, &tx_channel)) {
        return false;
    }

    esp_err_t error = i2s_channel_enable(tx_channel);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S TX: %s", esp_err_to_name(error));
        i2s_del_channel(tx_channel);
        return false;
    }

    ESP_LOGI(TAG,
             "Playing network speech: %lu Hz, 16-bit mono, %lu bytes, BCLK GPIO%d, LRC GPIO%d, DIN GPIO%d",
             (unsigned long)sample_rate_hz,
             (unsigned long)data_bytes,
             CONFIG_AUDIO_TEST_BCLK_GPIO,
             CONFIG_AUDIO_TEST_LRCLK_GPIO,
             CONFIG_AUDIO_TEST_DOUT_GPIO);

    bool success = true;
    uint32_t remaining = data_bytes;
    while (remaining > 0) {
        const size_t requested = remaining < sizeof(s_audio_buffer) ? remaining : sizeof(s_audio_buffer);
        if (!read_exact(read_cb, context, s_audio_buffer, requested) ||
            !write_all(tx_channel, s_audio_buffer, requested)) {
            success = false;
            break;
        }
        remaining -= (uint32_t)requested;
    }

    if (success) {
        success = write_all(tx_channel, (const uint8_t *)s_silence, sizeof(s_silence));
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    error = i2s_channel_disable(tx_channel);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to disable I2S TX: %s", esp_err_to_name(error));
        success = false;
    }
    error = i2s_del_channel(tx_channel);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to release I2S TX: %s", esp_err_to_name(error));
        success = false;
    }

    if (success) {
        ESP_LOGI(TAG, "Network speech playback completed");
    }
    return success;
}
