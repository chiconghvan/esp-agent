/**
 * ===========================================================================
 * @file display_manager.h
 * @brief Quản lý nội dung hiển thị trên OLED SSD1306
 *
 * State machine: BOOT → IDLE ↔ RESULT/ALERT
 * ===========================================================================
 */

#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include "esp_err.h"
#include <stdint.h>

/**
 * @brief Khởi tạo display manager + SSD1306
 * @param sda_gpio Chân SDA
 * @param scl_gpio Chân SCL
 */
esp_err_t display_init(int sda_gpio, int scl_gpio);

/**
 * @brief Cập nhật boot progress (0-100)
 * @param percent Phần trăm hoàn thành
 * @param label Nhãn bước hiện tại (VD: "Ket noi WiFi...")
 */
void display_boot_progress(int percent, const char *label);

/**
 * @brief Chuyển sang màn hình Idle (gọi sau khi boot xong)
 * Tự động cập nhật thông tin deadline từ database
 */
void display_show_idle(void);

/**
 * @brief Hiển thị kết quả action (inverted bar + chi tiết)
 * @param action Loại action: "Tao task", "Xoa task", etc.
 * @param task_id ID task (0 nếu không có)
 * @param title Tiêu đề task (UTF-8 sẽ auto-strip dấu)
 */
void display_show_result(const char *action, uint32_t task_id, const char *title);

/**
 * @brief Hiển thị cảnh báo deadline
 * @param task_id ID task
 * @param title Tiêu đề task (UTF-8)
 * @param due_str Chuỗi thời hạn (VD: "09:00 05/03")
 * @param days_left Số ngày còn lại
 */
void display_show_alert(uint32_t task_id, const char *title,
                        const char *due_str, int32_t seconds_left);

/**
 * @brief Task FreeRTOS chạy nền, quản lý timeout quay về Idle
 * Gọi 1 lần trong main, task tự chạy liên tục
 */
void display_start_task(void);

#endif /* DISPLAY_MANAGER_H */
