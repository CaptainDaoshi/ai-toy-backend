#pragma once

#include <stdbool.h>

typedef enum {
    OLED_STATUS_OFF,
    OLED_STATUS_CONNECTING,
    OLED_STATUS_READY,
    OLED_STATUS_LISTENING,
    OLED_STATUS_PROCESSING,
    OLED_STATUS_SPEAKING,
    OLED_STATUS_ERROR,
} oled_display_status_t;

bool oled_display_init(void);
void oled_display_show_status(oled_display_status_t status);
