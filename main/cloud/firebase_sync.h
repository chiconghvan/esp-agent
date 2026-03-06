/**
 * ===========================================================================
 * @file firebase_sync.h
 * @brief Module đồng bộ dữ liệu với Firebase Realtime Database
 *
 * Cho phép thiết bị ESP32 tự động đồng bộ CSDL Task lên Firebase.
 * Chạy trên nền tảng FreeRTOS Task độc lập để trách block main thread.
 * ===========================================================================
 */

#ifndef FIREBASE_SYNC_H
#define FIREBASE_SYNC_H

#include "task_database.h"
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Khởi tạo hệ thống đồng bộ Firebase (tạo Queue, Task background)
 * @return esp_err_t ESP_OK nếu thành công
 */
esp_err_t firebase_sync_init(void);

/**
 * @brief Gửi một task lên Firebase (tạo mới hoặc cập nhật)
 * Thêm request vào Queue nền, không block thread gọi.
 * 
 * @param task Con trỏ đến task cần đồng bộ
 * @return esp_err_t ESP_OK nếu enqueue thành công
 */
esp_err_t firebase_sync_upload_task(const task_record_t *task);

/**
 * @brief Đẩy toàn bộ CSDL cục bộ từ SPIFFS lên Firebase
 * Hàm này duyệt qua index, đọc từng file task và đẩy vào Queue.
 * 
 * @return esp_err_t ESP_OK nếu đã enqueue toàn bộ
 */
esp_err_t firebase_sync_upload_all(void);

/**
 * @brief Xoá 1 task khỏi Firebase (Hard delete)
 * Thêm request vào Queue nền, không block thread gọi.
 * 
 * @param task_id ID của task cần xoá
 * @return esp_err_t ESP_OK nếu enqueue thành công
 */
esp_err_t firebase_sync_delete_task(uint32_t task_id);

/**
 * @brief Tải toàn bộ cây /tasks/ từ Firebase về nạp lại vào CSDL SPIFFS
 * Hàm này CHẠY ĐỒNG BỘ (block) do chỉ dùng lúc khởi động hệ thống.
 * 
 * @return esp_err_t ESP_OK nếu thành công và tải xong dữ liệu
 */
esp_err_t firebase_sync_download_all(void);

#ifdef __cplusplus
}
#endif

#endif /* FIREBASE_SYNC_H */
