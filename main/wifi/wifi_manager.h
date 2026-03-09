/**
 * ===========================================================================
 * @file wifi_manager.h
 * @brief Quản lý kết nối WiFi STA và đồng bộ thời gian SNTP
 *
 * Module này xử lý: kết nối WiFi với retry tự động,
 * đồng bộ thời gian qua SNTP, và theo dõi trạng thái kết nối.
 * ===========================================================================
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Khởi tạo và kết nối WiFi STA
 *
 * Kết nối đến SSID/Password cấu hình trong config.h.
 * Block cho đến khi kết nối thành công hoặc hết retry.
 *
 * @return esp_err_t ESP_OK nếu kết nối thành công
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Khởi tạo và đồng bộ thời gian SNTP
 *
 * Nên gọi SAU khi WiFi đã kết nối thành công.
 * Block cho đến khi đồng bộ xong hoặc timeout.
 *
 * @return esp_err_t ESP_OK nếu đồng bộ thành công
 */
esp_err_t wifi_manager_start_sntp(void);

/**
 * @brief Kiểm tra WiFi đã kết nối chưa
 *
 * @return true Đã kết nối
 * @return false Chưa kết nối
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Lấy mức độ tín hiệu WiFi (0-3)
 */
int wifi_manager_get_level(void);

#endif /* WIFI_MANAGER_H */
