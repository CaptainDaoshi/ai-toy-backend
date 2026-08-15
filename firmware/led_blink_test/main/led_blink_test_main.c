#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "backend_client.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define RGB_LED_INDEX 0
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define BACKEND_TASK_STACK_SIZE 12288
#define BACKEND_TASK_PRIORITY 5

typedef enum {
    RGB_STATUS_CONNECTING,
    RGB_STATUS_CONNECTED,
    RGB_STATUS_FAILED,
} rgb_status_t;

static const char *TAG = "wifi_test";
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count;

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

static void set_rgb_status(led_strip_handle_t strip, rgb_status_t status)
{
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;

    switch (status) {
    case RGB_STATUS_CONNECTING:
        blue = CONFIG_LED_TEST_BRIGHTNESS;
        break;
    case RGB_STATUS_CONNECTED:
        green = CONFIG_LED_TEST_BRIGHTNESS;
        break;
    case RGB_STATUS_FAILED:
        red = CONFIG_LED_TEST_BRIGHTNESS;
        break;
    }

    ESP_ERROR_CHECK(led_strip_set_pixel(strip, RGB_LED_INDEX, red, green, blue));
    ESP_ERROR_CHECK(led_strip_refresh(strip));
}

static void wifi_event_handler(void *handler_arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_ERROR_CHECK(esp_wifi_connect());
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = event_data;

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

static void backend_speech_task(void *argument)
{
    led_strip_handle_t strip = (led_strip_handle_t)argument;

    ESP_LOGI(TAG,
             "Backend speech task started (stack high-water mark: %u bytes)",
             (unsigned int)uxTaskGetStackHighWaterMark(NULL));
    const bool backend_test_passed = backend_request_and_speak();
    set_rgb_status(strip, backend_test_passed ? RGB_STATUS_CONNECTED : RGB_STATUS_FAILED);
    if (backend_test_passed) {
        ESP_LOGI(TAG, "Backend request and speech playback test passed");
    } else {
        ESP_LOGE(TAG, "Backend speech test failed; check the preceding HTTPS/audio log");
    }

    ESP_LOGI(TAG,
             "Backend speech task finished (minimum free stack: %u bytes)",
             (unsigned int)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}

void app_main(void)
{
    led_strip_handle_t strip = configure_rgb_led();
    set_rgb_status(strip, RGB_STATUS_CONNECTING);

    initialize_nvs();
    const bool connected = connect_to_wifi();

    if (connected) {
        ESP_LOGI(TAG, "Wi-Fi connected; starting backend speech test");
        if (xTaskCreate(backend_speech_task,
                        "backend_speech",
                        BACKEND_TASK_STACK_SIZE,
                        strip,
                        BACKEND_TASK_PRIORITY,
                        NULL) == pdPASS) {
            return;
        }

        set_rgb_status(strip, RGB_STATUS_FAILED);
        ESP_LOGE(TAG, "Failed to create backend speech task");
    } else {
        set_rgb_status(strip, RGB_STATUS_FAILED);
        ESP_LOGE(TAG, "Wi-Fi test failed; verify SSID, password, signal, and 2.4 GHz availability");
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
