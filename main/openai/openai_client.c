/**
 * ===========================================================================
 * @file openai_client.c
 * @brief Triển khai giao tiếp OpenAI API qua HTTPS
 *
 * Chat Completion: phân loại tin nhắn → trả về JSON action.
 * Embedding: tạo vector 384 chiều cho semantic search.
 * Sử dụng esp_http_client với TLS certificate bundle.
 * ===========================================================================
 */

#include "openai_client.h"
#include "config.h"
#include "json_parser.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "token_tracker.h"
#include "network_gatekeeper.h"

static const char *TAG = "openai_client";

/** Buffer tạm cho HTTP response (dynamically allocated) */
static char *http_response_buf = NULL;
static int http_response_len = 0;
static int http_response_max = 0;

/* --------------------------------------------------------------------------
 * HTTP Event Handler
 * -------------------------------------------------------------------------- */
static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    switch (event->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (http_response_buf != NULL &&
                http_response_len + event->data_len < http_response_max - 1) {
                memcpy(http_response_buf + http_response_len, event->data, event->data_len);
                http_response_len += event->data_len;
                http_response_buf[http_response_len] = '\0';
            }
            break;

        default:
            break;
    }
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Hàm nội bộ: Thực hiện HTTP POST đến OpenAI API
 * -------------------------------------------------------------------------- */
static esp_err_t openai_http_post(const char *endpoint, const char *body_str,
                                   char *output_buffer, size_t output_size)
{
    /* Xây dựng URL đầy đủ */
    char url[128];
    snprintf(url, sizeof(url), "%s%s", OPENAI_API_URL, endpoint);

    /* Tái sử dụng buffer đã được truyền vào, KHÔNG cấp phát (malloc) nhân đôi */
    http_response_buf = output_buffer;
    http_response_len = 0;
    http_response_max = (int)output_size;
    memset(http_response_buf, 0, output_size);

    /* Cấu hình HTTP client */
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,  /* 30s timeout cho LLM response */
        .buffer_size = 1024,
        .buffer_size_tx = 512,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Không thể tạo HTTP client");
        http_response_buf = NULL;
        return ESP_FAIL;
    }

    /* Thiết lập headers */
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", OPENAI_API_KEY);

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_post_field(client, body_str, strlen(body_str));

    /* Thực hiện request */
    network_lock();
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);

    esp_http_client_cleanup(client);
    network_unlock();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST thất bại: %s", esp_err_to_name(err));
        http_response_buf = NULL;
        return err;
    }

    if (status_code != 200) {
        ESP_LOGE(TAG, "OpenAI API trả về status %d", status_code);
        http_response_buf = NULL;
        return ESP_FAIL;
    }

    http_response_buf = NULL;
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Chat Completion: phân loại tin nhắn
 * -------------------------------------------------------------------------- */
esp_err_t openai_chat_completion(const char *system_prompt,
                                  const char *user_message,
                                  char *response_buffer,
                                  size_t buffer_size)
{
    if (system_prompt == NULL || user_message == NULL || response_buffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Gọi Chat Completion...");

    /* Xây dựng request body JSON */
    cJSON *body = cJSON_CreateObject();
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(body, "model", OPENAI_MODEL);
    cJSON_AddNumberToObject(body, "max_tokens", OPENAI_MAX_TOKENS);
    cJSON_AddNumberToObject(body, "temperature", 0.1);

    /* Messages array */
    cJSON *messages = cJSON_CreateArray();
    if (messages == NULL) {
        cJSON_Delete(body);
        return ESP_ERR_NO_MEM;
    }

    /* System message */
    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", system_prompt);
    cJSON_AddItemToArray(messages, sys_msg);

    /* User message */
    cJSON *usr_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(usr_msg, "role", "user");
    cJSON_AddStringToObject(usr_msg, "content", user_message);
    cJSON_AddItemToArray(messages, usr_msg);

    cJSON_AddItemToObject(body, "messages", messages);

    /* Serialize JSON */
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    if (body_str == NULL) {
        ESP_LOGE(TAG, "Không thể serialize request body");
        return ESP_ERR_NO_MEM;
    }

    /* Gọi API */
    char *api_response = (char *)malloc(OPENAI_REQUEST_BUFFER_SIZE);
    if (api_response == NULL) {
        free(body_str);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = openai_http_post("/chat/completions", body_str,
                                      api_response, OPENAI_REQUEST_BUFFER_SIZE);
    free(body_str);

    if (err != ESP_OK) {
        free(api_response);
        return err;
    }

    /* Parse response → lấy content */
    cJSON *root = json_parse_string(api_response);
    free(api_response);
    if (root == NULL) {
        ESP_LOGE(TAG, "Không thể parse response JSON");
        return ESP_FAIL;
    }

    cJSON *choices = json_get_array(root, "choices");
    if (choices == NULL || cJSON_GetArraySize(choices) == 0) {
        ESP_LOGE(TAG, "Không có choices trong response");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *first_choice = cJSON_GetArrayItem(choices, 0);
    cJSON *message = json_get_object(first_choice, "message");
    const char *content = json_get_string(message, "content", "");

    /* Copy content vào response buffer */
    strncpy(response_buffer, content, buffer_size - 1);
    response_buffer[buffer_size - 1] = '\0';

    ESP_LOGI(TAG, "Chat Completion response: %.100s...", response_buffer);

    /* Ghi nhận token usage */
    cJSON *usage = json_get_object(root, "usage");
    if (usage != NULL) {
        uint32_t prompt_tok = (uint32_t)json_get_int(usage, "prompt_tokens", 0);
        uint32_t completion_tok = (uint32_t)json_get_int(usage, "completion_tokens", 0);
        token_tracker_add(TOKEN_TYPE_CHAT, prompt_tok, completion_tok);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Embedding: tạo vector cho text
 * -------------------------------------------------------------------------- */
esp_err_t openai_create_embedding(const char *text,
                                   float *embedding_output,
                                   int dimensions)
{
    if (text == NULL || embedding_output == NULL || dimensions <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Tạo embedding (%d chiều) cho: %.50s...", dimensions, text);

    /* Xây dựng request body */
    cJSON *body = cJSON_CreateObject();
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(body, "model", OPENAI_EMBEDDING_MODEL);
    cJSON_AddStringToObject(body, "input", text);
    cJSON_AddNumberToObject(body, "dimensions", dimensions);

    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    if (body_str == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Gọi API */
    char *api_response = (char *)malloc(OPENAI_REQUEST_BUFFER_SIZE);
    if (api_response == NULL) {
        free(body_str);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = openai_http_post("/embeddings", body_str,
                                      api_response, OPENAI_REQUEST_BUFFER_SIZE);
    free(body_str);

    if (err != ESP_OK) {
        free(api_response);
        return err;
    }

    /* Parse response → lấy vector */
    cJSON *root = json_parse_string(api_response);
    free(api_response);
    if (root == NULL) {
        ESP_LOGE(TAG, "Không thể parse embedding response");
        return ESP_FAIL;
    }

    cJSON *data_array = json_get_array(root, "data");
    if (data_array == NULL || cJSON_GetArraySize(data_array) == 0) {
        ESP_LOGE(TAG, "Không có data trong embedding response");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *first_data = cJSON_GetArrayItem(data_array, 0);
    cJSON *embedding = json_get_array(first_data, "embedding");
    if (embedding == NULL) {
        ESP_LOGE(TAG, "Không tìm thấy embedding array");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    int count = cJSON_GetArraySize(embedding);
    if (count < dimensions) {
        ESP_LOGW(TAG, "Embedding chỉ có %d chiều, yêu cầu %d", count, dimensions);
    }

    /* Copy vector values */
    int copy_count = (count < dimensions) ? count : dimensions;
    for (int i = 0; i < copy_count; i++) {
        cJSON *val = cJSON_GetArrayItem(embedding, i);
        if (val != NULL && cJSON_IsNumber(val)) {
            embedding_output[i] = (float)val->valuedouble;
        } else {
            embedding_output[i] = 0.0f;
        }
    }

    /* Zero-fill phần còn lại nếu thiếu */
    for (int i = copy_count; i < dimensions; i++) {
        embedding_output[i] = 0.0f;
    }

    ESP_LOGI(TAG, "Tạo embedding thành công (%d chiều)", copy_count);

    /* Ước lượng token usage cho embedding (số từ ≈ token) */
    cJSON *usage = json_get_object(root, "usage");
    if (usage != NULL) {
        uint32_t total_tok = (uint32_t)json_get_int(usage, "total_tokens", 0);
        token_tracker_add(TOKEN_TYPE_EMBEDDING, total_tok, 0);
    }

    cJSON_Delete(root);
    return ESP_OK;
}
