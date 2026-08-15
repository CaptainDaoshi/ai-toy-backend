#include "backend_client.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "max98357_audio.h"
#include "sdkconfig.h"

#define CHAT_RESPONSE_CAPACITY 16384U
#define ERROR_RESPONSE_CAPACITY 512U
#define HTTP_TIMEOUT_MS 45000
#define BACKEND_REQUEST_ATTEMPTS 3
#define BACKEND_RETRY_DELAY_MS 2000

typedef struct {
    esp_http_client_handle_t client;
} http_audio_context_t;

static const char *TAG = "backend";

static void configure_common_headers(esp_http_client_handle_t client)
{
    ESP_ERROR_CHECK(esp_http_client_set_header(client, "Content-Type", "application/json"));
    ESP_ERROR_CHECK(esp_http_client_set_header(client, "X-Device-Token", CONFIG_BACKEND_DEVICE_TOKEN));
    if (strlen(CONFIG_BACKEND_API_TOKEN) > 0) {
        char authorization[sizeof("Bearer ") + sizeof(CONFIG_BACKEND_API_TOKEN)];
        snprintf(authorization, sizeof(authorization), "Bearer %s", CONFIG_BACKEND_API_TOKEN);
        ESP_ERROR_CHECK(esp_http_client_set_header(client, "Authorization", authorization));
    }
}

static esp_http_client_handle_t create_http_client(const char *url)
{
    const esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
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

    int offset = 0;
    while (offset < body_size) {
        const int written = esp_http_client_write(client, body + offset, body_size - offset);
        if (written <= 0) {
            ESP_LOGE(TAG, "HTTPS request body write failed");
            return false;
        }
        offset += written;
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

static char *build_chat_request(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL ||
        !cJSON_AddStringToObject(root, "device_id", CONFIG_BACKEND_DEVICE_ID) ||
        !cJSON_AddStringToObject(root, "message", CONFIG_BACKEND_TEST_MESSAGE) ||
        !cJSON_AddStringToObject(root, "response_mode", "normal")) {
        cJSON_Delete(root);
        return NULL;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static char *request_chat_reply(void)
{
    char url[sizeof(CONFIG_BACKEND_BASE_URL) + sizeof("/v1/chat/completions")];
    snprintf(url, sizeof(url), "%s/v1/chat/completions", CONFIG_BACKEND_BASE_URL);

    char *request_body = build_chat_request();
    if (request_body == NULL) {
        ESP_LOGE(TAG, "Failed to create chat JSON");
        return NULL;
    }

    esp_http_client_handle_t client = create_http_client(url);
    if (client == NULL) {
        cJSON_free(request_body);
        ESP_LOGE(TAG, "Failed to create HTTPS client");
        return NULL;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    configure_common_headers(client);

    ESP_LOGI(TAG,
             "Sending test message to %s as device_id '%s': %s",
             CONFIG_BACKEND_BASE_URL,
             CONFIG_BACKEND_DEVICE_ID,
             CONFIG_BACKEND_TEST_MESSAGE);

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

    esp_http_client_handle_t client = create_http_client(url);
    if (client == NULL) {
        cJSON_free(request_body);
        ESP_LOGE(TAG, "Failed to create speech HTTPS client");
        return false;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    configure_common_headers(client);

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
    const bool success = max98357_play_wav_stream(read_audio_http, &context);
    esp_http_client_cleanup(client);
    return success;
}

bool backend_request_and_speak(void)
{
    if (strlen(CONFIG_BACKEND_DEVICE_ID) == 0 || strlen(CONFIG_BACKEND_DEVICE_TOKEN) == 0) {
        ESP_LOGE(TAG, "Backend device_id/token is empty; run .\\idf.ps1 menuconfig first");
        return false;
    }

    char *reply = NULL;
    for (int attempt = 1; attempt <= BACKEND_REQUEST_ATTEMPTS && reply == NULL; ++attempt) {
        reply = request_chat_reply();
        if (reply == NULL && attempt < BACKEND_REQUEST_ATTEMPTS) {
            ESP_LOGW(TAG,
                     "Chat request attempt %d/%d failed; retrying in %d ms",
                     attempt,
                     BACKEND_REQUEST_ATTEMPTS,
                     BACKEND_RETRY_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(BACKEND_RETRY_DELAY_MS));
        }
    }
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
