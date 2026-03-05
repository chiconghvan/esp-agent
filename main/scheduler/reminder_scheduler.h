/**
 * ===========================================================================
 * @file reminder_scheduler.h
 * @brief Bộ lập lịch nhắc nhở tự động
 *
 * Kiểm tra định kỳ các task có reminder đến hạn,
 * gửi notification qua Telegram. Không gọi LLM.
 * ===========================================================================
 */

#ifndef REMINDER_SCHEDULER_H
#define REMINDER_SCHEDULER_H

#include "esp_err.h"

/**
 * @brief Khởi tạo và bắt đầu scheduler
 *
 * Tạo esp_timer periodic để kiểm tra reminders.
 * Cần gọi SAU khi database và Telegram đã init.
 *
 * @return esp_err_t ESP_OK nếu thành công
 */
esp_err_t reminder_scheduler_start(void);

/**
 * @brief Dừng scheduler
 *
 * @return esp_err_t ESP_OK nếu thành công
 */
esp_err_t reminder_scheduler_stop(void);

#endif /* REMINDER_SCHEDULER_H */
