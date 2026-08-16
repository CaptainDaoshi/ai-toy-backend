#include "backend_client.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "inmp441_audio.h"
#include "max98357_audio.h"
#include "sdkconfig.h"

#define CHAT_RESPONSE_CAPACITY 16384U
#define TRANSCRIPTION_RESPONSE_CAPACITY 2048U
#define ERROR_RESPONSE_CAPACITY 512U
#define HTTP_TIMEOUT_MS 45000
#define BACKEND_REQUEST_ATTEMPTS 3
#define BACKEND_RETRY_DELAY_MS 2000

typedef struct {
    esp_http_client_handle_t client;
} http_audio_context_t;

typedef struct {
    esp_http_client_handle_t client;
} pcm_upload_context_t;

typedef struct {
    char content_type[64];
} http_response_metadata_t;

static const char *TAG = "backend";
static backend_audio_state_cb_t s_audio_state_callback;
static void *s_audio_state_context;

void backend_set_audio_state_callback(backend_audio_state_cb_t callback, void *context)
{
    s_audio_state_callback = callback;
    s_audio_state_context = context;
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    http_response_metadata_t *metadata = event->user_data;
    if (metadata != NULL && event->event_id == HTTP_EVENT_ON_HEADER &&
        event->header_key != NULL && event->header_value != NULL &&
        strcasecmp(event->header_key, "Content-Type") == 0) {
        snprintf(metadata->content_type,
                 sizeof(metadata->content_type),
                 "%s",
                 event->header_value);
    }
    return ESP_OK;
}

static void configure_common_headers(esp_http_client_handle_t client, const char *content_type)
{
    ESP_ERROR_CHECK(esp_http_client_set_header(client, "Content-Type", content_type));
    ESP_ERROR_CHECK(esp_http_client_set_header(client, "X-Device-Token", CONFIG_BACKEND_DEVICE_TOKEN));
    if (strlen(CONFIG_BACKEND_API_TOKEN) > 0) {
        char authorization[sizeof("Bearer ") + sizeof(CONFIG_BACKEND_API_TOKEN)];
        snprintf(authorization, sizeof(authorization), "Bearer %s", CONFIG_BACKEND_API_TOKEN);
        ESP_ERROR_CHECK(esp_http_client_set_header(client, "Authorization", authorization));
    }
}

static bool write_all(esp_http_client_handle_t client, const void *data, size_t size)
{
    const char *bytes = data;
    size_t offset = 0;
    while (offset < size) {
        const int written = esp_http_client_write(client,
                                                  bytes + offset,
                                                  (int)(size - offset));
        if (written <= 0) {
            ESP_LOGE(TAG, "HTTPS request body write failed");
            return false;
        }
        offset += (size_t)written;
    }
    return true;
}

static bool write_http_chunk(esp_http_client_handle_t client, const void *data, size_t size)
{
    char prefix[24];
    const int prefix_size = snprintf(prefix, sizeof(prefix), "%x\r\n", (unsigned int)size);
    if (prefix_size <= 0 || prefix_size >= (int)sizeof(prefix)) {
        ESP_LOGE(TAG, "Failed to format HTTP chunk prefix");
        return false;
    }
    return write_all(client, prefix, (size_t)prefix_size) &&
           (size == 0 || write_all(client, data, size)) &&
           write_all(client, "\r\n", 2);
}

static bool finish_http_chunks(esp_http_client_handle_t client)
{
    return write_all(client, "0\r\n\r\n", 5);
}

static esp_http_client_handle_t create_http_client(const char *url,
                                                   http_response_metadata_t *metadata)
{
    const esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .event_handler = http_event_handler,
        .user_data = metadata,
    };
    return esp_http_client_init(&config);
}

static bool write_request_body(esp_http_client_handle_t client, const char *body)
{
    const int body_size = (int)strlen(body);
    esp_err_t error = esp_http_client_open(client, body_size);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "HTTPS connection failed: %s", esp_err_to_name(error));
        return false;
    }

    if (!write_all(client, body, (size_t)body_size)) {
        return false;
    }

    if (esp_http_client_fetch_headers(client) < 0) {
        ESP_LOGE(TAG, "HTTPS response header read failed");
        return false;
    }
    return true;
}

static int read_http_response(esp_http_client_handle_t client, char *buffer, size_t capacity)
{
    const int64_t content_length = esp_http_client_get_content_length(client);
    if (content_length >= (int64_t)capacity) {
        ESP_LOGE(TAG,
                 "HTTPS response is too large: %lld bytes, maximum %u; expected JSON (the tunnel may have returned an HTML error page)",
                 (long long)content_length,
                 (unsigned int)(capacity - 1));
        return -1;
    }

    const int read_bytes = esp_http_client_read_response(client, buffer, (int)capacity - 1);
    if (read_bytes < 0) {
        ESP_LOGE(TAG, "HTTPS response read failed");
        return -1;
    }
    buffer[read_bytes] = '\0';

    if (!esp_http_client_is_complete_data_received(client)) {
        ESP_LOGE(TAG,
                 "HTTPS response ended early: read %d of %lld bytes",
                 read_bytes,
                 (long long)content_length);
        return -1;
    }
    if (read_bytes > 0 && buffer[0] == '<') {
        ESP_LOGE(TAG, "HTTPS endpoint returned HTML instead of JSON; check the intranet tunnel status");
        return -1;
    }
    return read_bytes;
}

static void log_http_error(esp_http_client_handle_t client, const char *operation)
{
    char response[ERROR_RESPONSE_CAPACITY] = {0};
    const int status = esp_http_client_get_status_code(client);
    const int received = read_http_response(client, response, sizeof(response));
    if (received > 0) {
        ESP_LOGE(TAG, "%s returned HTTP %d: %s", operation, status, response);
    } else {
        ESP_LOGE(TAG, "%s returned HTTP %d", operation, status);
    }
}

static char *build_chat_request(const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL ||
        !cJSON_AddStringToObject(root, "device_id", CONFIG_BACKEND_DEVICE_ID) ||
        !cJSON_AddStringToObject(root, "message", message) ||
        !cJSON_AddStringToObject(root, "response_mode", "normal")) {
        cJSON_Delete(root);
        return NULL;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static char *request_chat_reply(const char *message)
{
    char url[sizeof(CONFIG_BACKEND_BASE_URL) + sizeof("/v1/chat/completions")];
    snprintf(url, sizeof(url), "%s/v1/chat/completions", CONFIG_BACKEND_BASE_URL);

    char *request_body = build_chat_request(message);
    if (request_body == NULL) {
        ESP_LOGE(TAG, "Failed to create chat JSON");
        return NULL;
    }

    esp_http_client_handle_t client = create_http_client(url, NULL);
    if (client == NULL) {
        cJSON_free(request_body);
        ESP_LOGE(TAG, "Failed to create HTTPS client");
        return NULL;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    configure_common_headers(client, "application/json");

    ESP_LOGI(TAG,
             "Sending recognized text to %s as device_id '%s': %s",
             CONFIG_BACKEND_BASE_URL,
             CONFIG_BACKEND_DEVICE_ID,
             message);

    bool success = write_request_body(client, request_body);
    cJSON_free(request_body);
    if (!success) {
        esp_http_client_cleanup(client);
        return NULL;
    }

    const int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        log_http_error(client, "Chat request");
        esp_http_client_cleanup(client);
        return NULL;
    }

    char *response = calloc(1, CHAT_RESPONSE_CAPACITY);
    if (response == NULL || read_http_response(client, response, CHAT_RESPONSE_CAPACITY) < 0) {
        free(response);
        esp_http_client_cleanup(client);
        return NULL;
    }
    esp_http_client_cleanup(client);

    cJSON *root = cJSON_Parse(response);
    free(response);
    if (root == NULL) {
        ESP_LOGE(TAG, "Chat response is not valid JSON");
        return NULL;
    }
    const cJSON *reply = cJSON_GetObjectItemCaseSensitive(root, "reply");
    char *reply_copy = cJSON_IsString(reply) && reply->valuestring[0] != '\0'
                           ? strdup(reply->valuestring)
                           : NULL;
    cJSON_Delete(root);
    if (reply_copy == NULL) {
        ESP_LOGE(TAG, "Chat response does not contain a non-empty reply");
    }
    return reply_copy;
}

static char *build_speech_request(const char *reply)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL ||
        !cJSON_AddStringToObject(root, "device_id", CONFIG_BACKEND_DEVICE_ID) ||
        !cJSON_AddStringToObject(root, "text", reply)) {
        cJSON_Delete(root);
        return NULL;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static int read_audio_http(void *context, uint8_t *buffer, size_t size)
{
    http_audio_context_t *audio = context;
    return esp_http_client_read(audio->client, (char *)buffer, (int)size);
}

static bool request_and_play_speech(const char *reply)
{
    char url[sizeof(CONFIG_BACKEND_BASE_URL) + sizeof("/v1/speech")];
    snprintf(url, sizeof(url), "%s/v1/speech", CONFIG_BACKEND_BASE_URL);
    char *request_body = build_speech_request(reply);
    if (request_body == NULL) {
        ESP_LOGE(TAG, "Failed to create speech JSON");
        return false;
    }

    esp_http_client_handle_t client = create_http_client(url, NULL);
    if (client == NULL) {
        cJSON_free(request_body);
        ESP_LOGE(TAG, "Failed to create speech HTTPS client");
        return false;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    configure_common_headers(client, "application/json");

    const bool request_sent = write_request_body(client, request_body);
    cJSON_free(request_body);
    if (!request_sent) {
        esp_http_client_cleanup(client);
        return false;
    }

    const int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        log_http_error(client, "Speech request");
        esp_http_client_cleanup(client);
        return false;
    }

    http_audio_context_t context = {.client = client};
    if (s_audio_state_callback != NULL) {
        s_audio_state_callback(true, s_audio_state_context);
    }
    const bool success = max98357_play_wav_stream(read_audio_http, &context);
    if (s_audio_state_callback != NULL) {
        s_audio_state_callback(false, s_audio_state_context);
    }
    esp_http_client_cleanup(client);
    return success;
}

bool backend_speak_text(const char *text)
{
    if (text == NULL || text[0] == '\0' || strlen(CONFIG_BACKEND_DEVICE_TOKEN) == 0) {
        ESP_LOGE(TAG, "Speech prompt text or backend device token is empty");
        return false;
    }
    return request_and_play_speech(text);
}

static void write_le16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
    destination[2] = (uint8_t)(value >> 16);
    destination[3] = (uint8_t)(value >> 24);
}

static void build_pcm_wav_header(uint8_t header[44], uint32_t sample_rate, uint32_t data_size)
{
    memcpy(header, "RIFF", 4);
    write_le32(header + 4, 36U + data_size);
    memcpy(header + 8, "WAVEfmt ", 8);
    write_le32(header + 16, 16U);
    write_le16(header + 20, 1U);
    write_le16(header + 22, 1U);
    write_le32(header + 24, sample_rate);
    write_le32(header + 28, sample_rate * sizeof(int16_t));
    write_le16(header + 32, sizeof(int16_t));
    write_le16(header + 34, 16U);
    memcpy(header + 36, "data", 4);
    write_le32(header + 40, data_size);
}

static bool upload_pcm_samples(void *context,
                               const int16_t *samples,
                               size_t sample_count)
{
    pcm_upload_context_t *upload = context;
    return write_http_chunk(upload->client,
                            samples,
                            sample_count * sizeof(samples[0]));
}

static char *request_transcription(backend_record_continue_cb_t should_continue,
                                   void *continue_context)
{
    char url[sizeof(CONFIG_BACKEND_BASE_URL) +
             sizeof("/v1/audio/transcriptions?device_id=") +
             sizeof(CONFIG_BACKEND_DEVICE_ID)];
    snprintf(url,
             sizeof(url),
             "%s/v1/audio/transcriptions?device_id=%s",
             CONFIG_BACKEND_BASE_URL,
             CONFIG_BACKEND_DEVICE_ID);

    const uint32_t sample_rate = inmp441_sample_rate_hz();
    const uint32_t maximum_duration_ms = CONFIG_MIC_MAX_RECORD_DURATION_SECONDS * 1000U;
    const uint32_t maximum_frame_count = (sample_rate * maximum_duration_ms) / 1000U;
    const uint32_t maximum_pcm_size = maximum_frame_count * sizeof(int16_t);
    uint8_t wav_header[44];
    build_pcm_wav_header(wav_header, sample_rate, maximum_pcm_size);

    http_response_metadata_t response_metadata = {0};
    esp_http_client_handle_t client = create_http_client(url, &response_metadata);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to create transcription HTTPS client");
        return NULL;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    configure_common_headers(client, "audio/wav");

    ESP_LOGI(TAG,
             "Opening chunked microphone upload: %lu Hz mono PCM, maximum %lu ms",
             (unsigned long)sample_rate,
             (unsigned long)maximum_duration_ms);
    esp_err_t error = esp_http_client_open(client, -1);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Transcription HTTPS connection failed: %s", esp_err_to_name(error));
        esp_http_client_cleanup(client);
        return NULL;
    }

    pcm_upload_context_t upload = {.client = client};
    uint32_t captured_frames = 0;
    if (!write_http_chunk(client, wav_header, sizeof(wav_header)) ||
        !inmp441_capture_pcm(maximum_duration_ms,
                             should_continue,
                             continue_context,
                             upload_pcm_samples,
                             &upload,
                             &captured_frames) ||
        !finish_http_chunks(client)) {
        esp_http_client_cleanup(client);
        return NULL;
    }
    ESP_LOGI(TAG,
             "Microphone upload finished: %lu frames, %lu PCM bytes",
             (unsigned long)captured_frames,
             (unsigned long)(captured_frames * sizeof(int16_t)));
    if (esp_http_client_fetch_headers(client) < 0) {
        ESP_LOGE(TAG, "Transcription response header read failed");
        esp_http_client_cleanup(client);
        return NULL;
    }

    const int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        log_http_error(client, "Transcription request");
        esp_http_client_cleanup(client);
        return NULL;
    }

    if (response_metadata.content_type[0] != '\0' &&
        strstr(response_metadata.content_type, "application/json") == NULL) {
        ESP_LOGE(TAG,
                 "Transcription returned unexpected Content-Type '%s'; expected JSON "
                 "(an HTML response usually means the tunnel intercepted the API request)",
                 response_metadata.content_type);
        esp_http_client_cleanup(client);
        return NULL;
    }

    char response[TRANSCRIPTION_RESPONSE_CAPACITY] = {0};
    if (read_http_response(client, response, sizeof(response)) < 0) {
        esp_http_client_cleanup(client);
        return NULL;
    }
    esp_http_client_cleanup(client);

    cJSON *root = cJSON_Parse(response);
    if (root == NULL) {
        ESP_LOGE(TAG, "Transcription response is not valid JSON");
        return NULL;
    }
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    char *text_copy = cJSON_IsString(text) && text->valuestring[0] != '\0'
                          ? strdup(text->valuestring)
                          : NULL;
    cJSON_Delete(root);
    if (text_copy == NULL) {
        ESP_LOGE(TAG, "Transcription response does not contain recognized text");
    }
    return text_copy;
}

bool backend_voice_roundtrip(backend_record_continue_cb_t should_continue,
                             void *continue_context)
{
    if (strlen(CONFIG_BACKEND_DEVICE_ID) == 0 || strlen(CONFIG_BACKEND_DEVICE_TOKEN) == 0) {
        ESP_LOGE(TAG, "Backend device_id/token is empty; run .\\idf.ps1 menuconfig first");
        return false;
    }

    char *transcript = request_transcription(should_continue, continue_context);
    if (transcript == NULL) {
        return false;
    }
    ESP_LOGI(TAG, "Recognized speech: %s", transcript);

    char *reply = NULL;
    for (int attempt = 1; attempt <= BACKEND_REQUEST_ATTEMPTS && reply == NULL; ++attempt) {
        reply = request_chat_reply(transcript);
        if (reply == NULL && attempt < BACKEND_REQUEST_ATTEMPTS) {
            ESP_LOGW(TAG,
                     "Chat request attempt %d/%d failed; retrying in %d ms",
                     attempt,
                     BACKEND_REQUEST_ATTEMPTS,
                     BACKEND_RETRY_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(BACKEND_RETRY_DELAY_MS));
        }
    }
    free(transcript);
    if (reply == NULL) {
        return false;
    }
    ESP_LOGI(TAG, "Backend reply: %s", reply);

    bool success = false;
    for (int attempt = 1; attempt <= BACKEND_REQUEST_ATTEMPTS && !success; ++attempt) {
        success = request_and_play_speech(reply);
        if (!success && attempt < BACKEND_REQUEST_ATTEMPTS) {
            ESP_LOGW(TAG,
                     "Speech request attempt %d/%d failed; retrying in %d ms",
                     attempt,
                     BACKEND_REQUEST_ATTEMPTS,
                     BACKEND_RETRY_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(BACKEND_RETRY_DELAY_MS));
        }
    }
    free(reply);
    return success;
}
