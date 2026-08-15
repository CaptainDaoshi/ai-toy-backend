#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int (*max98357_audio_read_cb_t)(void *context, uint8_t *buffer, size_t size);

bool max98357_play_wav_stream(max98357_audio_read_cb_t read_cb, void *context);
