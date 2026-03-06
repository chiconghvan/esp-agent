/**
 * ===========================================================================
 * @file action_undo.h
 * @brief Quản lý lịch sử thao tác để hỗ trợ tính năng Undo (Hoàn tác)
 * ===========================================================================
 */

#ifndef ACTION_UNDO_H
#define ACTION_UNDO_H

#include "esp_err.h"
#include "task_database.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Các loại thao tác có thể được Undo
 */
typedef enum {
    UNDO_CREATE,    /**< Đã tạo mới task (Undo = xoá đi) */
    UNDO_UPDATE,    /**< Đã cập nhật task (Undo = khôi phục thông tin cũ) */
    UNDO_COMPLETE,  /**< Đã đánh dấu hoàn thành (Undo = khôi phục trạng thái cũ) */
    UNDO_DELETE     /**< Đã xoá mềm task (Undo = khôi phục lại) */
} action_undo_type_t;

/**
 * @brief Headers của file lưu trữ undo
 */
typedef struct {
    uint8_t action_type;  /**< enum action_undo_type_t */
    int num_tasks;        /**< Số lượng task bị ảnh hưởng */
} undo_header_t;

/**
 * @brief Lưu trạng thái cũ của các task bị thay đổi để hỗ trợ Undo
 * 
 * Hàm này ghi vào file `/spiffs/undo.bin`.
 * 
 * @param type Loại hành động vừa thực hiện
 * @param tasks Mảng các task_record_t (chứa trạng thái CŨ trước khi sửa)
 * @param count Số lượng task trong mảng
 * @return esp_err_t ESP_OK nếu thành công
 */
esp_err_t action_undo_save(action_undo_type_t type, const task_record_t *tasks, int count);

/**
 * @brief Kiểm tra xem có khả năng undo không (file undo.bin có tồn tại và hợp lệ)
 * @return true nếu có thể undo
 */
bool action_undo_is_available(void);

/**
 * @brief Thực thi việc Undo dựa trên lịch sử gần nhất
 * 
 * Đọc file `/spiffs/undo.bin` và khôi phục các task.
 * 
 * @param response Buffer chứa câu trả lời cho User
 * @param buffer_size Kích thước buffer
 * @return esp_err_t ESP_OK nếu thành công
 */
esp_err_t action_undo_execute(char *response, size_t buffer_size);

/**
 * @brief Xoá lịch sử undo (không cho phép undo nữa - ví dụ sau Hard delete)
 */
void action_undo_clear(void);

#endif /* ACTION_UNDO_H */
