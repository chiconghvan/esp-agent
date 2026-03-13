/**
 * ===========================================================================
 * @file log_server.h
 * @brief Web Log Viewer: Hiển thị log ESP32 qua trình duyệt web
 *
 * - Ring buffer 50 dòng trong RAM
 * - HTTP server cổng 8080 phục vụ trang log
 * ===========================================================================
 */

#ifndef LOG_SERVER_H
#define LOG_SERVER_H

#include "esp_err.h"

/**
 * @brief Khởi tạo ring buffer và hook ESP_LOG
 * Gọi CÀI SỚM NHẤT trong app_main(), trước mọi ESP_LOG khác.
 */
esp_err_t log_server_init(void);

/**
 * @brief Khởi động HTTP server cổng 8080
 * Gọi SAU khi WiFi đã kết nối.
 */
esp_err_t log_server_start(void);

#endif /* LOG_SERVER_H */
