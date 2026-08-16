#pragma once

#include <stdbool.h>

typedef bool (*backend_record_continue_cb_t)(void *context);
typedef void (*backend_audio_state_cb_t)(bool playing, void *context);

void backend_set_audio_state_callback(backend_audio_state_cb_t callback, void *context);
bool backend_speak_text(const char *text);
bool backend_voice_roundtrip(backend_record_continue_cb_t should_continue,
                             void *continue_context);
