/**
 * ===========================================================================
 * @file telegram_bot.h
 * @brief Giao tiếp với Telegram Bot API
 *
 * Module này xử lý: nhận tin nhắn qua long polling (getUpdates),
 * gửi tin nhắn reply (sendMessage). Sử dụng esp_http_client HTTPS.
 * ===========================================================================
 */

#ifndef TELEGRAM_BOT_H
#define TELEGRAM_BOT_H

#include "esp_err.h"
#include <stdint.h>

/**
 * @brief Cấu trúc chứa thông tin một tin nhắn Telegram nhận được
 */
typedef struct {
    int64_t update_id;               /**< ID update (dùng cho offset) */
    int64_t chat_id;                 /**< ID chat người gửi */
    char text[512];                  /**< Nội dung tin nhắn */
    char from_first_name[64];        /**< Tên người gửi */
} telegram_message_t;

/**
 * @brief Khởi tạo Telegram Bot client
 *
 * @return esp_err_t ESP_OK nếu thành công
 */
esp_err_t telegram_bot_init(void);

/**
 * @brief Nhận tin nhắn mới từ Telegram (long polling)
 *
 * Gọi getUpdates API với timeout. Block cho đến khi có tin nhắn
 * mới hoặc timeout.
 *
 * @param message Con trỏ đến cấu trúc để lưu tin nhắn nhận được
 * @return esp_err_t ESP_OK nếu có tin nhắn mới, ESP_ERR_NOT_FOUND nếu timeout
 */
esp_err_t telegram_bot_get_update(telegram_message_t *message);

/**
 * @brief Gửi tin nhắn text qua Telegram
 *
 * @param chat_id ID chat để gửi (thường lấy từ message nhận được)
 * @param text Nội dung tin nhắn (hỗ trợ UTF-8, emoji)
 * @return esp_err_t ESP_OK nếu gửi thành công
 */
esp_err_t telegram_bot_send_message(int64_t chat_id, const char *text);

/**
 * @brief Gửi tin nhắn đến chat_id mặc định (từ config.h)
 *
 * @param text Nội dung tin nhắn
 * @return esp_err_t ESP_OK nếu gửi thành công
 */
esp_err_t telegram_bot_send_default(const char *text);

#endif /* TELEGRAM_BOT_H */
