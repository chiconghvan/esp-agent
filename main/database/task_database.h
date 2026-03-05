/**
 * ===========================================================================
 * @file task_database.h
 * @brief Quản lý cơ sở dữ liệu task trên SPIFFS
 *
 * Module này cung cấp: CRUD operations cho tasks, quản lý index,
 * truy vấn theo time range, type, status. Lưu trữ trên SPIFFS
 * dưới dạng file JSON riêng biệt cho mỗi task.
 * ===========================================================================
 */

#ifndef TASK_DATABASE_H
#define TASK_DATABASE_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "config.h"

/**
 * @brief Cấu trúc dữ liệu cho một task
 */
typedef struct {
    uint32_t id;                           /**< ID task (auto-increment) */
    char title[TASK_TITLE_MAX_LEN];        /**< Tiêu đề task */
    char type[16];                         /**< Loại: meeting/report/reminder/deadline/event/other */
    char status[16];                       /**< Trạng thái: pending/done/cancelled */
    time_t created_at;                     /**< Thời điểm tạo */
    time_t start_time;                     /**< Thời gian bắt đầu (0 = không có) */
    time_t due_time;                       /**< Hạn chót (0 = không có) */
    time_t completed_at;                   /**< Thời điểm hoàn thành (0 = chưa) */
    time_t reminder;                       /**< Thời điểm nhắc (0 = không nhắc) */
    char repeat[16];                       /**< Lặp lại: none/daily/weekly/monthly/yearly */
    int repeat_interval;                   /**< Khoảng cách lặp */
    char notes[TASK_NOTES_MAX_LEN];        /**< Ghi chú bổ sung */
} task_record_t;

/**
 * @brief Cấu trúc entry trong index (nhẹ, dùng cho filter nhanh)
 */
typedef struct {
    uint32_t id;            /**< ID task */
    char status[16];        /**< Trạng thái */
    char type[16];          /**< Loại task */
    time_t due_time;        /**< Hạn chót */
    time_t start_time;      /**< Thời gian bắt đầu */
    time_t reminder;        /**< Thời điểm nhắc */
} task_index_entry_t;

/**
 * @brief Cấu trúc index tổng hợp
 */
typedef struct {
    uint32_t next_id;                              /**< ID tiếp theo */
    int count;                                     /**< Số task hiện tại */
    task_index_entry_t entries[MAX_TASK_COUNT];     /**< Mảng entries */
} task_index_t;

/**
 * @brief Khởi tạo database (mount SPIFFS, tạo thư mục, load index)
 *
 * @return esp_err_t ESP_OK nếu thành công
 */
esp_err_t task_database_init(void);

/**
 * @brief Tạo task mới và lưu vào database
 *
 * Tự động gán ID, set created_at, lưu file, cập nhật index.
 *
 * @param task Task data (id và created_at sẽ được gán tự động)
 * @return esp_err_t ESP_OK nếu thành công
 */
esp_err_t task_database_create(task_record_t *task);

/**
 * @brief Đọc task theo ID
 *
 * @param task_id ID task cần đọc
 * @param task Con trỏ để lưu task data
 * @return esp_err_t ESP_OK nếu tìm thấy
 */
esp_err_t task_database_read(uint32_t task_id, task_record_t *task);

/**
 * @brief Cập nhật task có sẵn
 *
 * @param task Task data đã cập nhật (giữ nguyên id)
 * @return esp_err_t ESP_OK nếu thành công
 */
esp_err_t task_database_update(const task_record_t *task);

/**
 * @brief Xóa mềm task (set status = cancelled)
 *
 * @param task_id ID task cần xóa
 * @return esp_err_t ESP_OK nếu thành công
 */
esp_err_t task_database_soft_delete(uint32_t task_id);

/**
 * @brief Xóa cứng (hard delete) — xóa file + index entry
 *
 * @param task_id ID task cần xóa
 * @return esp_err_t ESP_OK nếu thành công
 */
esp_err_t task_database_hard_delete(uint32_t task_id);

/**
 * @brief Truy vấn tasks theo khoảng thời gian
 *
 * Tìm tasks có due_time hoặc start_time nằm trong range.
 *
 * @param start Thời điểm bắt đầu range
 * @param end Thời điểm kết thúc range
 * @param type Lọc theo type (NULL = không lọc)
 * @param status Lọc theo status (NULL = không lọc)
 * @param results Mảng để lưu kết quả
 * @param max_results Kích thước mảng results
 * @param found_count Con trỏ để lưu số lượng tìm thấy
 * @return esp_err_t ESP_OK
 */
esp_err_t task_database_query_by_time(time_t start, time_t end,
                                       const char *type, const char *status,
                                       task_record_t *results, int max_results,
                                       int *found_count);

/**
 * @brief Truy vấn tasks theo type và/hoặc status
 *
 * @param type Lọc theo type (NULL = không lọc)
 * @param status Lọc theo status (NULL = không lọc)
 * @param results Mảng để lưu kết quả
 * @param max_results Kích thước mảng results
 * @param found_count Con trỏ để lưu số lượng tìm thấy
 * @return esp_err_t ESP_OK
 */
esp_err_t task_database_query_by_type(const char *type, const char *status,
                                       task_record_t *results, int max_results,
                                       int *found_count);

/**
 * @brief Truy vấn tasks có reminder đến hạn
 *
 * Tìm tasks có status=pending, reminder != 0, reminder <= now.
 *
 * @param results Mảng để lưu kết quả
 * @param max_results Kích thước mảng results
 * @param found_count Con trỏ để lưu số lượng tìm thấy
 * @return esp_err_t ESP_OK
 */
esp_err_t task_database_query_due_reminders(task_record_t *results, int max_results,
                                             int *found_count);

/**
 * @brief Lấy con trỏ đến index hiện tại (read-only)
 *
 * @return const task_index_t* Con trỏ đến index
 */
const task_index_t *task_database_get_index(void);

/**
 * @brief Lưu index xuống SPIFFS (gọi sau khi có thay đổi)
 *
 * @return esp_err_t ESP_OK nếu thành công
 */
esp_err_t task_database_save_index(void);

#endif /* TASK_DATABASE_H */
