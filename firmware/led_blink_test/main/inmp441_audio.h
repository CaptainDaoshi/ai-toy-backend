#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef bool (*inmp441_pcm_sink_t)(void *context,
                                    const int16_t *samples,
                                    size_t sample_count);
typedef bool (*inmp441_continue_cb_t)(void *context);

uint32_t inmp441_sample_rate_hz(void);

bool inmp441_prepare(void);

bool inmp441_capture_pcm(uint32_t maximum_duration_ms,
                         inmp441_continue_cb_t should_continue,
                         void *continue_context,
                         inmp441_pcm_sink_t sink,
                         void *sink_context,
                         uint32_t *captured_frame_count);
