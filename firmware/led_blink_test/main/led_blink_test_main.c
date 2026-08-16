#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "backend_client.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "inmp441_audio.h"
#include "led_strip.h"
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"
#include "oled_display.h"
#include "sdkconfig.h"

#define RGB_LED_INDEX 0
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define APP_TASK_STACK_SIZE 14336
#define APP_TASK_PRIORITY 5
#define LED_TASK_STACK_SIZE 3072
#define LED_TASK_PRIORITY 3
#define LED_UPDATE_MS 40
#define BUTTON_SAMPLE_MS 10
#define BUTTON_SAMPLE_TICKS                                                     \
    ((pdMS_TO_TICKS(BUTTON_SAMPLE_MS) > 0) ? pdMS_TO_TICKS(BUTTON_SAMPLE_MS) : 1)
#define BUTTON_DEBOUNCE_TICKS                                                   \
    ((pdMS_TO_TICKS(CONFIG_BUTTON_DEBOUNCE_MS) > 0)                            \
         ? pdMS_TO_TICKS(CONFIG_BUTTON_DEBOUNCE_MS)                            \
         : 1)

typedef enum {
    LED_MODE_OFF,
    LED_MODE_CONNECTING,
    LED_MODE_READY,
    LED_MODE_LISTENING,
    LED_MODE_PROCESSING,
    LED_MODE_SPEAKING,
    LED_MODE_ERROR,
} led_mode_t;

typedef struct {
    bool release_pending;
    TickType_t release_started_at;
} talk_button_context_t;

typedef struct {
    gpio_num_t gpio;
    const char *name;
    bool raw_pressed;
    bool stable_pressed;
    bool armed;
    TickType_t raw_changed_at;
} button_state_t;

static const char *TAG = "wifi_test";
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count;
static volatile led_mode_t s_led_mode = LED_MODE_CONNECTING;
static volatile led_mode_t s_led_resume_mode = LED_MODE_READY;

static led_strip_handle_t configure_rgb_led(void)
{
    led_strip_handle_t strip = NULL;
    const led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_LED_TEST_RGB_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &strip));
    ESP_ERROR_CHECK(led_strip_clear(strip));
    return strip;
}

static void set_led_mode(led_mode_t mode)
{
    s_led_mode = mode;
    oled_display_status_t display_status = OLED_STATUS_ERROR;
    switch (mode) {
    case LED_MODE_OFF:
        display_status = OLED_STATUS_OFF;
        break;
    case LED_MODE_CONNECTING:
        display_status = OLED_STATUS_CONNECTING;
        break;
    case LED_MODE_READY:
        display_status = OLED_STATUS_READY;
        break;
    case LED_MODE_LISTENING:
        display_status = OLED_STATUS_LISTENING;
        break;
    case LED_MODE_PROCESSING:
        display_status = OLED_STATUS_PROCESSING;
        break;
    case LED_MODE_SPEAKING:
        display_status = OLED_STATUS_SPEAKING;
        break;
    case LED_MODE_ERROR:
    default:
        display_status = OLED_STATUS_ERROR;
        break;
    }
    oled_display_show_status(display_status);
}

static void led_task(void *argument)
{
    led_strip_handle_t strip = (led_strip_handle_t)argument;
    uint8_t pulse_step = 0;

    for (;;) {
        const led_mode_t mode = s_led_mode;
        const uint8_t brightness = CONFIG_LED_TEST_BRIGHTNESS;
        uint8_t red = 0;
        uint8_t green = 0;
        uint8_t blue = 0;

        switch (mode) {
        case LED_MODE_OFF:
            break;
        case LED_MODE_CONNECTING:
            blue = brightness;
            break;
        case LED_MODE_READY:
            green = brightness;
            break;
        case LED_MODE_LISTENING:
            green = brightness;
            blue = brightness;
            break;
        case LED_MODE_PROCESSING:
            red = brightness;
            green = brightness / 2U;
            break;
        case LED_MODE_SPEAKING: {
            const uint8_t triangle = pulse_step < 25U ? pulse_step : (uint8_t)(49U - pulse_step);
            const uint8_t level = (uint8_t)(2U + ((uint16_t)brightness * triangle) / 24U);
            red = level;
            blue = level;
            pulse_step = (uint8_t)((pulse_step + 1U) % 50U);
            break;
        }
        case LED_MODE_ERROR:
            red = brightness;
            break;
        }

        if (mode != LED_MODE_SPEAKING) {
            pulse_step = 0;
        }
        esp_err_t error = led_strip_set_pixel(strip, RGB_LED_INDEX, red, green, blue);
        if (error == ESP_OK) {
            error = led_strip_refresh(strip);
        }
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "RGB LED update failed: %s", esp_err_to_name(error));
        }
        vTaskDelay(pdMS_TO_TICKS(LED_UPDATE_MS));
    }
}

static void backend_audio_state_changed(bool playing, void *context)
{
    (void)context;
    if (playing) {
        s_led_resume_mode = s_led_mode;
        set_led_mode(LED_MODE_SPEAKING);
    } else {
        set_led_mode(s_led_resume_mode);
    }
}

static void wifi_event_handler(void *handler_arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)handler_arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_ERROR_CHECK(esp_wifi_connect());
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = event_data;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (s_retry_count < CONFIG_WIFI_TEST_MAXIMUM_RETRY) {
            ++s_retry_count;
            ESP_LOGW(TAG,
                     "Wi-Fi disconnected (reason %d), retry %d/%d",
                     event->reason,
                     s_retry_count,
                     CONFIG_WIFI_TEST_MAXIMUM_RETRY);
            ESP_ERROR_CHECK(esp_wifi_connect());
        } else {
            ESP_LOGE(TAG,
                     "Wi-Fi connection failed after %d retries (last reason %d)",
                     CONFIG_WIFI_TEST_MAXIMUM_RETRY,
                     event->reason);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        s_retry_count = 0;
        ESP_LOGI(TAG, "Got IPv4 address: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool connect_to_wifi(void)
{
    if (strlen(CONFIG_WIFI_TEST_SSID) == 0) {
        ESP_LOGE(TAG, "Wi-Fi SSID is empty; run .\\idf.ps1 menuconfig first");
        return false;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi event group");
        return false;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_LOGE(TAG, "Failed to create the default Wi-Fi station interface");
        return false;
    }

    const wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               &wifi_event_handler,
                                               NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,
                                               IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler,
                                               NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_TEST_SSID,
            .password = CONFIG_WIFI_TEST_PASSWORD,
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    if (strlen(CONFIG_WIFI_TEST_PASSWORD) == 0) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG,
             "Connecting to Wi-Fi SSID '%s' (maximum %d retries)",
             CONFIG_WIFI_TEST_SSID,
             CONFIG_WIFI_TEST_MAXIMUM_RETRY);

    const EventBits_t result = xEventGroupWaitBits(s_wifi_event_group,
                                                   WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                                   pdFALSE,
                                                   pdFALSE,
                                                   portMAX_DELAY);
    if ((result & WIFI_CONNECTED_BIT) == 0) {
        return false;
    }

    wifi_ap_record_t access_point = {0};
    if (esp_wifi_sta_get_ap_info(&access_point) == ESP_OK) {
        ESP_LOGI(TAG,
                 "Wi-Fi connected: channel %u, RSSI %d dBm",
                 access_point.primary,
                 access_point.rssi);
    }
    return true;
}

static void initialize_nvs(void)
{
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        error = nvs_flash_init();
    }
    ESP_ERROR_CHECK(error);
}

static bool configure_buttons(void)
{
    if (CONFIG_TALK_BUTTON_GPIO == CONFIG_POWER_BUTTON_GPIO) {
        ESP_LOGE(TAG, "Talk and power buttons cannot use the same GPIO");
        return false;
    }
    const gpio_config_t config = {
        .pin_bit_mask = (1ULL << CONFIG_TALK_BUTTON_GPIO) |
                        (1ULL << CONFIG_POWER_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const esp_err_t error = gpio_config(&config);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Button GPIO configuration failed: %s", esp_err_to_name(error));
        return false;
    }
    ESP_LOGI(TAG,
             "Buttons ready: talk GPIO%d, soft power GPIO%d, active low",
             CONFIG_TALK_BUTTON_GPIO,
             CONFIG_POWER_BUTTON_GPIO);
    return true;
}

static bool button_is_pressed(gpio_num_t gpio)
{
    return gpio_get_level(gpio) == 0;
}

static void initialize_button_state(button_state_t *button,
                                    gpio_num_t gpio,
                                    const char *name)
{
    button->gpio = gpio;
    button->name = name;
    button->raw_pressed = button_is_pressed(gpio);
    button->stable_pressed = button->raw_pressed;
    button->armed = !button->raw_pressed;
    button->raw_changed_at = xTaskGetTickCount();

    ESP_LOGI(TAG,
             "%s button initial level: GPIO%d=%d (%s)",
             name,
             gpio,
             gpio_get_level(gpio),
             button->raw_pressed ? "LOW/pressed" : "HIGH/released");
    if (button->raw_pressed) {
        ESP_LOGW(TAG,
                 "%s button is LOW at startup; it will be ignored until GPIO%d "
                 "returns HIGH. Check that GPIO and GND use opposite switch contact groups.",
                 name,
                 gpio);
    }
}

static bool button_pressed_event(button_state_t *button)
{
    const TickType_t now = xTaskGetTickCount();
    const bool raw_pressed = button_is_pressed(button->gpio);
    if (raw_pressed != button->raw_pressed) {
        button->raw_pressed = raw_pressed;
        button->raw_changed_at = now;
    }

    if (raw_pressed == button->stable_pressed ||
        (now - button->raw_changed_at) < BUTTON_DEBOUNCE_TICKS) {
        return false;
    }

    button->stable_pressed = raw_pressed;
    ESP_LOGI(TAG,
             "%s button %s: GPIO%d=%d",
             button->name,
             raw_pressed ? "PRESSED" : "RELEASED",
             button->gpio,
             raw_pressed ? 0 : 1);

    if (!raw_pressed) {
        button->armed = true;
        return false;
    }
    if (!button->armed) {
        return false;
    }

    button->armed = false;
    return true;
}

static void log_button_wiring_hint(void)
{
    ESP_LOGI(TAG,
             "Button test: released GPIO%d/GPIO%d must read 1; pressed must read 0",
             CONFIG_TALK_BUTTON_GPIO,
             CONFIG_POWER_BUTTON_GPIO);
    if (button_is_pressed(CONFIG_TALK_BUTTON_GPIO) ||
        button_is_pressed(CONFIG_POWER_BUTTON_GPIO)) {
        ESP_LOGW(TAG,
                 "At least one button input is already LOW. Disconnect its GND wire; "
                 "if it becomes HIGH, the switch is wired across the wrong two legs.");
    }
}

static bool continue_while_talk_button_held(void *context)
{
    talk_button_context_t *button = context;
    if (button_is_pressed(CONFIG_TALK_BUTTON_GPIO)) {
        button->release_pending = false;
        return true;
    }

    const TickType_t now = xTaskGetTickCount();
    if (!button->release_pending) {
        button->release_pending = true;
        button->release_started_at = now;
        return true;
    }
    if ((now - button->release_started_at) < pdMS_TO_TICKS(CONFIG_BUTTON_DEBOUNCE_MS)) {
        return true;
    }

    set_led_mode(LED_MODE_PROCESSING);
    return false;
}

static void app_task(void *argument)
{
    (void)argument;
    ESP_LOGI(TAG,
             "Application task started (stack high-water mark: %u bytes)",
             (unsigned int)uxTaskGetStackHighWaterMark(NULL));

    set_led_mode(LED_MODE_CONNECTING);
    if (!connect_to_wifi()) {
        set_led_mode(LED_MODE_ERROR);
        ESP_LOGE(TAG, "Wi-Fi startup failed");
        vTaskDelete(NULL);
        return;
    }

    set_led_mode(LED_MODE_READY);
    if (!backend_speak_text("WiFi连接成功")) {
        ESP_LOGW(TAG, "Wi-Fi success voice prompt could not be played");
    }

    const bool microphone_ready = inmp441_prepare();
    if (!backend_speak_text(microphone_ready ? "麦克风连接成功" : "麦克风连接失败")) {
        ESP_LOGW(TAG, "Microphone status voice prompt could not be played");
    }
    if (!microphone_ready) {
        set_led_mode(LED_MODE_ERROR);
        ESP_LOGE(TAG, "Microphone preparation failed; push-to-talk is disabled");
    } else {
        set_led_mode(LED_MODE_READY);
        ESP_LOGI(TAG, "Ready: hold GPIO%d button to talk", CONFIG_TALK_BUTTON_GPIO);
    }

    button_state_t talk_button_state;
    button_state_t power_button_state;
    initialize_button_state(&talk_button_state,
                            CONFIG_TALK_BUTTON_GPIO,
                            "Talk");
    initialize_button_state(&power_button_state,
                            CONFIG_POWER_BUTTON_GPIO,
                            "Power");
    log_button_wiring_hint();

    bool device_on = true;
    for (;;) {
        if (button_pressed_event(&power_button_state)) {
            if (device_on) {
                backend_speak_text("设备已关机");
                device_on = false;
                set_led_mode(LED_MODE_OFF);
                ESP_LOGI(TAG, "Soft standby enabled; press GPIO%d to wake",
                         CONFIG_POWER_BUTTON_GPIO);
            } else {
                device_on = true;
                set_led_mode(microphone_ready ? LED_MODE_READY : LED_MODE_ERROR);
                backend_speak_text("设备已开机");
                ESP_LOGI(TAG, "Soft standby disabled");
            }
            continue;
        }

        if (device_on && microphone_ready &&
            button_pressed_event(&talk_button_state)) {
            if ((xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) == 0) {
                set_led_mode(LED_MODE_ERROR);
                ESP_LOGW(TAG, "Talk ignored because Wi-Fi is disconnected");
                continue;
            }

            talk_button_context_t talk_button = {0};
            set_led_mode(LED_MODE_LISTENING);
            ESP_LOGI(TAG, "Talk button pressed; recording until release");
            const bool passed = backend_voice_roundtrip(continue_while_talk_button_held,
                                                         &talk_button);
            if (!passed) {
                set_led_mode(LED_MODE_ERROR);
                ESP_LOGE(TAG, "Push-to-talk round trip failed");
                vTaskDelay(pdMS_TO_TICKS(1000));
            } else {
                ESP_LOGI(TAG, "Push-to-talk round trip passed");
            }
            set_led_mode(LED_MODE_READY);
            ESP_LOGI(TAG,
                     "Application task minimum free stack: %u bytes",
                     (unsigned int)uxTaskGetStackHighWaterMark(NULL));
        }

        vTaskDelay(BUTTON_SAMPLE_TICKS);
    }
}

void app_main(void)
{
    led_strip_handle_t strip = configure_rgb_led();
    initialize_nvs();
    if (!oled_display_init()) {
        ESP_LOGW(TAG, "OLED unavailable; continuing without a display");
    }
    if (!configure_buttons()) {
        set_led_mode(LED_MODE_ERROR);
        return;
    }
    backend_set_audio_state_callback(backend_audio_state_changed, NULL);

    if (xTaskCreate(led_task,
                    "status_led",
                    LED_TASK_STACK_SIZE,
                    strip,
                    LED_TASK_PRIORITY,
                    NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create RGB LED task");
        return;
    }
    if (xTaskCreate(app_task,
                    "app_control",
                    APP_TASK_STACK_SIZE,
                    NULL,
                    APP_TASK_PRIORITY,
                    NULL) != pdPASS) {
        set_led_mode(LED_MODE_ERROR);
        ESP_LOGE(TAG, "Failed to create application task");
    }
}
