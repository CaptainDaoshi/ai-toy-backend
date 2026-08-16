#include "oled_display.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define OLED_WIDTH 128U
#define OLED_HEIGHT 32U
#define OLED_PAGE_COUNT (OLED_HEIGHT / 8U)
#define OLED_FRAMEBUFFER_SIZE (OLED_WIDTH * OLED_PAGE_COUNT)
#define OLED_TRANSFER_TIMEOUT_MS 50
#define OLED_FIRST_ADDRESS 0x3CU
#define OLED_SECOND_ADDRESS 0x3DU
#define OLED_CHARACTER_WIDTH 6U
#define OLED_MAX_LINE_LENGTH (OLED_WIDTH / OLED_CHARACTER_WIDTH)
#define OLED_TASK_STACK_SIZE 3072
#define OLED_TASK_PRIORITY 2
#define OLED_THINKING_FRAME_MS 180
#define OLED_SPEAKING_FRAME_MS 120

static const char *TAG = "oled";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_device;
static uint8_t s_framebuffer[OLED_FRAMEBUFFER_SIZE];
static bool s_ready;
static bool s_powered;
static bool s_update_error_logged;
static TaskHandle_t s_task;
static volatile oled_display_status_t s_requested_status = OLED_STATUS_CONNECTING;

/* Five vertical columns per 5x7 uppercase glyph. Stored in flash. */
static const uint8_t s_font_uppercase[26][5] = {
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* A */
    {0x7F, 0x49, 0x49, 0x49, 0x36}, /* B */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* C */
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* D */
    {0x7F, 0x49, 0x49, 0x49, 0x41}, /* E */
    {0x7F, 0x09, 0x09, 0x09, 0x01}, /* F */
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, /* G */
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* H */
    {0x00, 0x41, 0x7F, 0x41, 0x00}, /* I */
    {0x20, 0x40, 0x41, 0x3F, 0x01}, /* J */
    {0x7F, 0x08, 0x14, 0x22, 0x41}, /* K */
    {0x7F, 0x40, 0x40, 0x40, 0x40}, /* L */
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, /* M */
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* N */
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* O */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* P */
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* Q */
    {0x7F, 0x09, 0x19, 0x29, 0x46}, /* R */
    {0x46, 0x49, 0x49, 0x49, 0x31}, /* S */
    {0x01, 0x01, 0x7F, 0x01, 0x01}, /* T */
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* U */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* V */
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, /* W */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* X */
    {0x07, 0x08, 0x70, 0x08, 0x07}, /* Y */
    {0x61, 0x51, 0x49, 0x45, 0x43}, /* Z */
};

static esp_err_t oled_write_commands(const uint8_t *commands, size_t length)
{
    if (length == 0 || length > 31U) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t packet[32] = {0};
    memcpy(&packet[1], commands, length);
    return i2c_master_transmit(s_device,
                               packet,
                               length + 1U,
                               OLED_TRANSFER_TIMEOUT_MS);
}

static esp_err_t oled_write_framebuffer(void)
{
    const uint8_t address_commands[] = {
        0x21, 0x00, OLED_WIDTH - 1U,
        0x22, 0x00, OLED_PAGE_COUNT - 1U,
    };
    esp_err_t error = oled_write_commands(address_commands,
                                          sizeof(address_commands));
    if (error != ESP_OK) {
        return error;
    }

    const uint8_t data_control = 0x40;
    i2c_master_transmit_multi_buffer_info_t buffers[] = {
        {
            .write_buffer = &data_control,
            .buffer_size = sizeof(data_control),
        },
        {
            .write_buffer = s_framebuffer,
            .buffer_size = sizeof(s_framebuffer),
        },
    };
    return i2c_master_multi_buffer_transmit(s_device,
                                            buffers,
                                            sizeof(buffers) / sizeof(buffers[0]),
                                            OLED_TRANSFER_TIMEOUT_MS);
}

static void oled_draw_text(uint8_t page, const char *text)
{
    if (page >= OLED_PAGE_COUNT || text == NULL) {
        return;
    }

    size_t length = 0;
    while (text[length] != '\0' && length < OLED_MAX_LINE_LENGTH) {
        ++length;
    }
    size_t x = (OLED_WIDTH - length * OLED_CHARACTER_WIDTH) / 2U;
    for (size_t index = 0; index < length; ++index) {
        const char character = text[index];
        const uint8_t *glyph = NULL;
        if (character >= 'A' && character <= 'Z') {
            glyph = s_font_uppercase[(size_t)(character - 'A')];
        }
        for (size_t column = 0; column < 5U && x < OLED_WIDTH; ++column) {
            s_framebuffer[(size_t)page * OLED_WIDTH + x++] =
                glyph != NULL ? glyph[column] : 0;
        }
        if (x < OLED_WIDTH) {
            s_framebuffer[(size_t)page * OLED_WIDTH + x++] = 0;
        }
    }
}

static void oled_render_lines(const char *line_1,
                              const char *line_2,
                              const char *line_3,
                              const char *line_4)
{
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    oled_draw_text(0, line_1);
    oled_draw_text(1, line_2);
    oled_draw_text(2, line_3);
    oled_draw_text(3, line_4);
}

static void oled_set_pixel(uint8_t x, uint8_t y)
{
    if (x >= OLED_WIDTH || y >= OLED_HEIGHT) {
        return;
    }
    s_framebuffer[(size_t)(y / 8U) * OLED_WIDTH + x] |=
        (uint8_t)(1U << (y % 8U));
}

static void oled_draw_horizontal_line(uint8_t x, uint8_t y, uint8_t width)
{
    for (uint8_t column = 0; column < width; ++column) {
        oled_set_pixel((uint8_t)(x + column), y);
    }
}

static void oled_draw_vertical_line(uint8_t x, uint8_t y, uint8_t height)
{
    for (uint8_t row = 0; row < height; ++row) {
        oled_set_pixel(x, (uint8_t)(y + row));
    }
}

static void oled_draw_rectangle(uint8_t x,
                                uint8_t y,
                                uint8_t width,
                                uint8_t height)
{
    if (width == 0 || height == 0) {
        return;
    }
    oled_draw_horizontal_line(x, y, width);
    oled_draw_horizontal_line(x, (uint8_t)(y + height - 1U), width);
    oled_draw_vertical_line(x, y, height);
    oled_draw_vertical_line((uint8_t)(x + width - 1U), y, height);
}

static void oled_fill_rectangle(uint8_t x,
                                uint8_t y,
                                uint8_t width,
                                uint8_t height)
{
    for (uint8_t row = 0; row < height; ++row) {
        oled_draw_horizontal_line(x, (uint8_t)(y + row), width);
    }
}

static void oled_draw_eye(uint8_t x, int8_t pupil_offset, bool blink)
{
    const uint8_t eye_y = 11U;
    const uint8_t eye_width = 22U;
    if (blink) {
        oled_draw_horizontal_line(x, (uint8_t)(eye_y + 4U), eye_width);
        return;
    }

    oled_draw_rectangle(x, eye_y, eye_width, 9U);
    int pupil_x = (int)x + 9 + pupil_offset;
    if (pupil_x < (int)x + 2) {
        pupil_x = (int)x + 2;
    } else if (pupil_x > (int)x + (int)eye_width - 6) {
        pupil_x = (int)x + (int)eye_width - 6;
    }
    oled_fill_rectangle((uint8_t)pupil_x, (uint8_t)(eye_y + 3U), 4U, 4U);
}

static void oled_render_thinking_face(uint8_t frame)
{
    static const int8_t pupil_offsets[] = {-4, -2, 0, 2, 4, 2, 0, -2};
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    oled_draw_text(0, "THINKING");

    const bool blink = (frame % 12U) == 10U;
    const int8_t pupil_offset = pupil_offsets[frame %
                                                  (sizeof(pupil_offsets) /
                                                   sizeof(pupil_offsets[0]))];
    oled_draw_eye(23U, pupil_offset, blink);
    oled_draw_eye(83U, pupil_offset, blink);

    for (uint8_t dot = 0; dot < 3U; ++dot) {
        const bool raised = ((frame + dot) % 3U) == 0U;
        const uint8_t dot_y = raised ? 25U : 28U;
        oled_fill_rectangle((uint8_t)(56U + dot * 7U), dot_y, 3U, 3U);
    }
}

static void oled_render_speaking_face(uint8_t frame)
{
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    oled_draw_text(0, "SPEAKING");

    const bool blink = (frame % 14U) == 12U;
    oled_draw_eye(23U, 0, blink);
    oled_draw_eye(83U, 0, blink);

    switch (frame % 4U) {
    case 0:
        oled_draw_horizontal_line(54U, 27U, 20U);
        break;
    case 1:
        oled_draw_rectangle(56U, 25U, 16U, 5U);
        break;
    case 2:
        oled_draw_rectangle(52U, 23U, 24U, 9U);
        oled_fill_rectangle(57U, 28U, 14U, 3U);
        break;
    case 3:
    default:
        oled_draw_horizontal_line(54U, 25U, 20U);
        oled_draw_horizontal_line(57U, 28U, 14U);
        oled_draw_horizontal_line(61U, 30U, 6U);
        break;
    }
}

static bool oled_present_frame(void)
{
    esp_err_t error = oled_write_framebuffer();
    if (error == ESP_OK && !s_powered) {
        const uint8_t display_on = 0xAF;
        error = oled_write_commands(&display_on, 1);
        if (error == ESP_OK) {
            s_powered = true;
        }
    }
    if (error != ESP_OK) {
        if (!s_update_error_logged) {
            ESP_LOGE(TAG, "OLED update failed: %s", esp_err_to_name(error));
            s_update_error_logged = true;
        }
        return false;
    }
    s_update_error_logged = false;
    return true;
}

static void oled_render_static_status(oled_display_status_t status)
{
    switch (status) {
    case OLED_STATUS_CONNECTING:
        oled_render_lines("AI TOY", "WIFI", "CONNECTING", "");
        break;
    case OLED_STATUS_READY:
        oled_render_lines("AI TOY", "WIFI CONNECTED", "MIC READY", "HOLD TALK");
        break;
    case OLED_STATUS_LISTENING:
        oled_render_lines("AI TOY", "LISTENING", "RELEASE BUTTON", "TO SEND");
        break;
    case OLED_STATUS_ERROR:
        oled_render_lines("AI TOY", "ERROR", "CHECK SERIAL", "");
        break;
    case OLED_STATUS_OFF:
    case OLED_STATUS_PROCESSING:
    case OLED_STATUS_SPEAKING:
    default:
        memset(s_framebuffer, 0, sizeof(s_framebuffer));
        break;
    }
}

static void oled_task(void *argument)
{
    (void)argument;
    oled_display_status_t active_status = (oled_display_status_t)-1;
    uint8_t frame = 0;
    ESP_LOGI(TAG,
             "OLED animation task started (stack high-water mark: %u bytes)",
             (unsigned int)uxTaskGetStackHighWaterMark(NULL));

    for (;;) {
        const oled_display_status_t requested_status = s_requested_status;
        const bool status_changed = requested_status != active_status;
        if (status_changed) {
            active_status = requested_status;
            frame = 0;
        }

        if (active_status == OLED_STATUS_OFF) {
            if (s_powered) {
                const uint8_t display_off = 0xAE;
                const esp_err_t error = oled_write_commands(&display_off, 1);
                if (error != ESP_OK) {
                    ESP_LOGE(TAG, "OLED power-off failed: %s", esp_err_to_name(error));
                } else {
                    s_powered = false;
                }
            }
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        TickType_t wait_ticks = portMAX_DELAY;
        bool refresh = status_changed;
        if (active_status == OLED_STATUS_PROCESSING) {
            oled_render_thinking_face(frame++);
            wait_ticks = pdMS_TO_TICKS(OLED_THINKING_FRAME_MS);
            refresh = true;
        } else if (active_status == OLED_STATUS_SPEAKING) {
            oled_render_speaking_face(frame++);
            wait_ticks = pdMS_TO_TICKS(OLED_SPEAKING_FRAME_MS);
            refresh = true;
        } else if (status_changed) {
            oled_render_static_status(active_status);
        }

        if (refresh) {
            oled_present_frame();
        }
        ulTaskNotifyTake(pdTRUE, wait_ticks);
    }
}

bool oled_display_init(void)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = CONFIG_OLED_SDA_GPIO,
        .scl_io_num = CONFIG_OLED_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t error = i2c_new_master_bus(&bus_config, &s_bus);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus initialization failed: %s", esp_err_to_name(error));
        return false;
    }

    uint8_t address = OLED_FIRST_ADDRESS;
    error = i2c_master_probe(s_bus, address, OLED_TRANSFER_TIMEOUT_MS);
    if (error != ESP_OK) {
        address = OLED_SECOND_ADDRESS;
        error = i2c_master_probe(s_bus, address, OLED_TRANSFER_TIMEOUT_MS);
    }
    if (error != ESP_OK) {
        ESP_LOGE(TAG,
                 "OLED not found at 0x%02X or 0x%02X; check SDA GPIO%d, SCL GPIO%d, VCC and GND",
                 OLED_FIRST_ADDRESS,
                 OLED_SECOND_ADDRESS,
                 CONFIG_OLED_SDA_GPIO,
                 CONFIG_OLED_SCL_GPIO);
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
        return false;
    }

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = CONFIG_OLED_I2C_FREQUENCY_HZ,
    };
    error = i2c_master_bus_add_device(s_bus, &device_config, &s_device);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "OLED device setup failed: %s", esp_err_to_name(error));
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
        return false;
    }

    const uint8_t initialization_commands[] = {
        0xAE,
        0xD5, 0x80,
        0xA8, OLED_HEIGHT - 1U,
        0xD3, 0x00,
        0x40,
        0x8D, 0x14,
        0x20, 0x00,
        0xA1,
        0xC8,
        0xDA, 0x02,
        0x81, 0x8F,
        0xD9, 0xF1,
        0xDB, 0x40,
        0xA4,
        0xA6,
    };
    error = oled_write_commands(initialization_commands,
                                sizeof(initialization_commands));
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "SSD1306 initialization failed: %s", esp_err_to_name(error));
        i2c_master_bus_rm_device(s_device);
        i2c_del_master_bus(s_bus);
        s_device = NULL;
        s_bus = NULL;
        return false;
    }

    s_powered = false;
    s_requested_status = OLED_STATUS_CONNECTING;
    if (xTaskCreate(oled_task,
                    "oled_animation",
                    OLED_TASK_STACK_SIZE,
                    NULL,
                    OLED_TASK_PRIORITY,
                    &s_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OLED animation task");
        i2c_master_bus_rm_device(s_device);
        i2c_del_master_bus(s_bus);
        s_device = NULL;
        s_bus = NULL;
        return false;
    }
    s_ready = true;
    ESP_LOGI(TAG,
             "SSD1306 ready: 128x32, address 0x%02X, SDA GPIO%d, SCL GPIO%d",
             address,
             CONFIG_OLED_SDA_GPIO,
             CONFIG_OLED_SCL_GPIO);
    return true;
}

void oled_display_show_status(oled_display_status_t status)
{
    if (!s_ready || status < OLED_STATUS_OFF || status > OLED_STATUS_ERROR) {
        return;
    }
    s_requested_status = status;
    xTaskNotifyGive(s_task);
}
