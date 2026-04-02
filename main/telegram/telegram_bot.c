/**
 * ===========================================================================
 * @file telegram_bot.c
 * @brief Triển khai giao tiếp Telegram Bot API qua HTTPS
 *
 * Sử dụng esp_http_client với TLS certificate bundle.
 * Long polling qua getUpdates, gửi reply qua sendMessage.
 * ===========================================================================
 */

#include "telegram_bot.h"
#include "config.h"
#include "json_parser.h"
#include "time_utils.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "display_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network_gatekeeper.h"

static const char *TAG = "telegram_bot";

/** Update ID cuối cùng đã xử lý (dùng cho long polling offset) */
static int64_t last_update_id = 0;

/** Buffer nhận HTTP response */
static char response_buffer[HTTP_BUFFER_SIZE];
static int response_buffer_len = 0;

/* --------------------------------------------------------------------------
 * HTTP Event Handler: thu thập response data
 * -------------------------------------------------------------------------- */
static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    switch (event->event_id) {
        case HTTP_EVENT_ON_HEADER:
            if (strcasecmp(event->header_key, "Date") == 0) {
                /* Đồng bộ thời gian nếu hệ thống chưa có thời gian chuẩn (trước 2024) */
                time_t now = time(NULL);
                if (now < 1704067200) { 
                    time_utils_set_time_from_http_date(event->header_value);
                    ESP_LOGI(TAG, "Đã cập nhật đồng hồ từ Telegram, làm mới màn hình...");
                    display_show_idle();
                }
            }
            break;

        case HTTP_EVENT_ON_DATA:
            /* Append data vào buffer */
            if (response_buffer_len + event->data_len < HTTP_BUFFER_SIZE - 1) {
                memcpy(response_buffer + response_buffer_len, event->data, event->data_len);
                response_buffer_len += event->data_len;
                response_buffer[response_buffer_len] = '\0';
            } else {
                ESP_LOGW(TAG, "HTTP response buffer đầy");
            }
            break;

        default:
            break;
    }
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Khởi tạo Telegram Bot
 * -------------------------------------------------------------------------- */
esp_err_t telegram_bot_init(void)
{
    ESP_LOGI(TAG, "Khởi tạo Telegram Bot...");
    ESP_LOGI(TAG, "Bot Token: %.10s...", TELEGRAM_BOT_TOKEN);
    last_update_id = 0;
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Nhận tin nhắn mới (long polling)
 * -------------------------------------------------------------------------- */
esp_err_t telegram_bot_get_update(telegram_message_t *message)
{
    if (message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Xây dựng URL cho getUpdates */
    char url[256];
    snprintf(url, sizeof(url),
        "%s%s/getUpdates?offset=%" PRId64 "&timeout=%d&limit=1",
        TELEGRAM_API_URL, TELEGRAM_BOT_TOKEN,
        last_update_id + 1, TELEGRAM_POLL_TIMEOUT_SEC);

    /* Reset response buffer */
    response_buffer_len = 0;
    memset(response_buffer, 0, sizeof(response_buffer));

    /* Cấu hình HTTP client */
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = (TELEGRAM_POLL_TIMEOUT_SEC + 10) * 1000,  /* Timeout > polling timeout */
        .buffer_size = 1024,
        .buffer_size_tx = 512,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Không thể tạo HTTP client");
        return ESP_FAIL;
    }

    /* Thực hiện request */
    network_lock();
    esp_err_t err = esp_http_client_perform(client);
    int status_code = -1;
    if (err == ESP_OK) {
        status_code = esp_http_client_get_status_code(client);
    }
    esp_http_client_cleanup(client);
    network_unlock();

    if (status_code != 200) {
        ESP_LOGW(TAG, "Telegram API trả về status: %d", status_code);
        return ESP_FAIL;
    }

    /* Parse response JSON */
    cJSON *root = json_parse_string(response_buffer);
    if (root == NULL) {
        ESP_LOGE(TAG, "Không thể parse response JSON");
        return ESP_FAIL;
    }

    /* Kiểm tra "ok" field */
    bool ok = json_get_bool(root, "ok", false);
    if (!ok) {
        ESP_LOGW(TAG, "Telegram API trả về ok=false");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    /* Lấy mảng "result" */
    cJSON *result_array = json_get_array(root, "result");
    if (result_array == NULL || cJSON_GetArraySize(result_array) == 0) {
        /* Không có tin nhắn mới (timeout) → bình thường */
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    /* Lấy update đầu tiên */
    cJSON *update = cJSON_GetArrayItem(result_array, 0);
    if (update == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    /* Parse update_id */
    message->update_id = json_get_int(update, "update_id", 0);
    last_update_id = message->update_id;
    message->is_callback = false;
    memset(message->callback_query, 0, sizeof(message->callback_query));

    /* Thử parse callback_query (nút bấm) */
    cJSON *cbq = json_get_object(update, "callback_query");
    if (cbq != NULL) {
        message->is_callback = true;
        const char *data = json_get_string(cbq, "data", "");
        strncpy(message->callback_query, data, sizeof(message->callback_query) - 1);
        
        /* Đối với callback, chat_id nằm trong field message.chat.id */
        cJSON *msg_obj = json_get_object(cbq, "message");
        if (msg_obj != NULL) {
            cJSON *chat = json_get_object(msg_obj, "chat");
            if (chat != NULL) {
                message->chat_id = (int64_t)json_get_double(chat, "id", 0);
            }
            cJSON *from = json_get_object(cbq, "from"); // From của nút bấm
            if (from != NULL) {
                const char *name = json_get_string(from, "first_name", "");
                strncpy(message->from_first_name, name, sizeof(message->from_first_name) - 1);
            }
        }
        cJSON_Delete(root);
        return ESP_OK;
    }

    /* Parse message object (tin nhắn thường) */
    cJSON *msg = json_get_object(update, "message");
    if (msg == NULL) {
        ESP_LOGW(TAG, "Update không chứa message");
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    /* Parse chat.id */
    cJSON *chat = json_get_object(msg, "chat");
    if (chat != NULL) {
        message->chat_id = json_get_int(chat, "id", 0);
    }

    /* Parse text */
    const char *text = json_get_string(msg, "text", "");
    strncpy(message->text, text, sizeof(message->text) - 1);
    message->text[sizeof(message->text) - 1] = '\0';

    /* Parse from.first_name */
    cJSON *from = json_get_object(msg, "from");
    if (from != NULL) {
        const char *name = json_get_string(from, "first_name", "");
        strncpy(message->from_first_name, name, sizeof(message->from_first_name) - 1);
        message->from_first_name[sizeof(message->from_first_name) - 1] = '\0';
    }

    ESP_LOGI(TAG, "Nhận tin nhắn từ %s: %s", message->from_first_name, message->text);

    cJSON_Delete(root);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Gửi tin nhắn qua Telegram
 * -------------------------------------------------------------------------- */
/** Gửi 1 lần (không retry) — logic nội bộ */
static esp_err_t _send_once(int64_t chat_id, const char *text)
{
    char url[256];
    snprintf(url, sizeof(url), "%s%s/sendMessage",
             TELEGRAM_API_URL, TELEGRAM_BOT_TOKEN);

    cJSON *body = cJSON_CreateObject();
    if (body == NULL) return ESP_ERR_NO_MEM;

    cJSON_AddNumberToObject(body, "chat_id", (double)chat_id);
    cJSON_AddStringToObject(body, "text", text);
    cJSON_AddStringToObject(body, "parse_mode", "HTML");

    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (body_str == NULL) return ESP_ERR_NO_MEM;

    response_buffer_len = 0;
    memset(response_buffer, 0, sizeof(response_buffer));

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) { free(body_str); return ESP_FAIL; }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body_str, strlen(body_str));

    network_lock();
    esp_err_t err = esp_http_client_perform(client);
    int status_code = -1;
    if (err == ESP_OK) {
        status_code = esp_http_client_get_status_code(client);
    }
    esp_http_client_cleanup(client);
    network_unlock();
    free(body_str);

    if (err != ESP_OK) return err;
    if (status_code != 200) {
        ESP_LOGW(TAG, "sendMessage status: %d | %.200s", status_code, response_buffer);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/** Số lần retry tối đa khi gửi tin nhắn thất bại */
#define SEND_MAX_RETRIES  1
/** Thời gian chờ giữa các lần retry (ms) */
#define SEND_RETRY_DELAY_MS  2000

esp_err_t telegram_bot_send_message(int64_t chat_id, const char *text)
{
    if (text == NULL || strlen(text) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = _send_once(chat_id, text);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Đã gửi tin nhắn (chat_id=%" PRId64 ")", chat_id);
        return ESP_OK;
    }

    /* Retry nếu thất bại */
    for (int i = 0; i < SEND_MAX_RETRIES; i++) {
        ESP_LOGW(TAG, "Gửi thất bại, retry %d/%d sau %dms...", i + 1, SEND_MAX_RETRIES, SEND_RETRY_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(SEND_RETRY_DELAY_MS));
        err = _send_once(chat_id, text);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Retry thành công (chat_id=%" PRId64 ")", chat_id);
            return ESP_OK;
        }
    }

    ESP_LOGE(TAG, "Gửi tin nhắn thất bại sau %d lần retry", SEND_MAX_RETRIES + 1);
    return err;
}

/* --------------------------------------------------------------------------
 * Gửi tin nhắn đến chat_id mặc định
 * -------------------------------------------------------------------------- */
esp_err_t telegram_bot_send_default(const char *text)
{
    int64_t default_chat_id = strtoll(TELEGRAM_CHAT_ID, NULL, 10);
    return telegram_bot_send_message(default_chat_id, text);
}

/* --------------------------------------------------------------------------
 * Gửi tin nhắn có kèm bàn phím Inline (Inline Keyboard)
 * -------------------------------------------------------------------------- */
esp_err_t telegram_bot_send_inline_keyboard(int64_t chat_id, const char *text, 
                                            const char *btn_text, const char *callback_data)
{
    if (text == NULL || btn_text == NULL || callback_data == NULL) return ESP_ERR_INVALID_ARG;

    char url[256];
    snprintf(url, sizeof(url), "%s%s/sendMessage", TELEGRAM_API_URL, TELEGRAM_BOT_TOKEN);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "chat_id", (double)chat_id);
    cJSON_AddStringToObject(body, "text", text);
    cJSON_AddStringToObject(body, "parse_mode", "HTML");

    /* Inline keyboard markup */
    cJSON *reply_markup = cJSON_CreateObject();
    cJSON *inline_kb_array = cJSON_CreateArray();
    cJSON *row = cJSON_CreateArray();
    cJSON *button = cJSON_CreateObject();
    cJSON_AddStringToObject(button, "text", btn_text);
    cJSON_AddStringToObject(button, "callback_data", callback_data);
    cJSON_AddItemToArray(row, button);
    cJSON_AddItemToArray(inline_kb_array, row);
    cJSON_AddItemToObject(reply_markup, "inline_keyboard", inline_kb_array);
    cJSON_AddItemToObject(body, "reply_markup", reply_markup);

    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    if (body_str == NULL) return ESP_ERR_NO_MEM;

    response_buffer_len = 0;
    memset(response_buffer, 0, sizeof(response_buffer));

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) { free(body_str); return ESP_FAIL; }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body_str, strlen(body_str));

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body_str);

    if (err != ESP_OK || status_code != 200) {
        ESP_LOGW(TAG, "Gửi inline keyboard thất bại (%d)", status_code);
        return ESP_FAIL;
    }
    return ESP_OK;
}
