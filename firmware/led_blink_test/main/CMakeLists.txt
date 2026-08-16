idf_component_register(SRCS "led_blink_test_main.c" "backend_client.c" "max98357_audio.c" "inmp441_audio.c" "oled_display.c"
                       PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_http_client esp-tls esp_timer esp_wifi mbedtls nvs_flash espressif__cjson
                       INCLUDE_DIRS "")
