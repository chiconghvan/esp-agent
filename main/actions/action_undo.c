/**
 * ===========================================================================
 * @file action_undo.c
 * @brief Triển khai chức năng Undo dựa trên file system của SPIFFS
 * ===========================================================================
 */

#include "action_undo.h"
#include "action_dispatcher.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "vector_search.h"
#include "openai_client.h"
#include "display_manager.h"
#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>

static const char *TAG = "action_undo";
#define UNDO_FILE_PATH "/spiffs/undo.bin"

esp_err_t action_undo_save(action_undo_type_t type, const task_record_t *tasks, int count)
{
    if (tasks == NULL && count > 0) return ESP_ERR_INVALID_ARG;
    if (count <= 0) {
        action_undo_clear(); // Khong co gi de undo, xoa file
        return ESP_OK;
    }

    FILE *f = fopen(UNDO_FILE_PATH, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Không thể mở file %s để ghi", UNDO_FILE_PATH);
        return ESP_FAIL;
    }

    // Ghi header
    undo_header_t header = {
        .action_type = (uint8_t)type,
        .num_tasks = count
    };
    
    if (fwrite(&header, sizeof(undo_header_t), 1, f) != 1) {
        ESP_LOGE(TAG, "Lỗi ghi header undo.bin");
        fclose(f);
        return ESP_FAIL;
    }

    // Ghi array tasks
    if (fwrite(tasks, sizeof(task_record_t), count, f) != (size_t)count) {
        ESP_LOGE(TAG, "Lỗi ghi dữ liệu tasks undo.bin");
        fclose(f);
        return ESP_FAIL;
    }

    fclose(f);
    ESP_LOGI(TAG, "Đã lưu log Undo cho hành động %d, số task: %d", type, count);
    return ESP_OK;
}

bool action_undo_is_available(void)
{
    struct stat st;
    if (stat(UNDO_FILE_PATH, &st) == 0) {
        return (st.st_size >= sizeof(undo_header_t));
    }
    return false;
}

void action_undo_clear(void)
{
    struct stat st;
    if (stat(UNDO_FILE_PATH, &st) == 0) {
        unlink(UNDO_FILE_PATH);
        ESP_LOGI(TAG, "Đã xoá lịch sử Undo");
    }
}

static esp_err_t restore_task_and_update_embedding(const task_record_t *old_task)
{
    esp_err_t err = task_database_update(old_task);
    if (err == ESP_OK) {
        // Cập nhật lại embedding nếu khôi phục thành công
        float new_emb[EMBEDDING_DIM];
        if (openai_create_embedding(old_task->title, new_emb, EMBEDDING_DIM) == ESP_OK) {
            vector_search_save(old_task->id, new_emb);
        }
    }
    return err;
}

esp_err_t action_undo_execute(char *response, size_t buffer_size)
{
    if (!action_undo_is_available()) {
        snprintf(response, buffer_size, "⚠️ Không có thao tác nào để hoàn tác (hoặc đã quá hạn).");
        return ESP_FAIL;
    }

    FILE *f = fopen(UNDO_FILE_PATH, "rb");
    if (f == NULL) {
        snprintf(response, buffer_size, "⚠️ Lỗi khi mở file hoàn tác.");
        return ESP_FAIL;
    }

    undo_header_t header;
    if (fread(&header, sizeof(undo_header_t), 1, f) != 1) {
        fclose(f);
        snprintf(response, buffer_size, "⚠️ Lỗi đọc header hoàn tác.");
        action_undo_clear();
        return ESP_FAIL;
    }

    if (header.num_tasks <= 0 || header.num_tasks > MAX_TASK_COUNT) {
        fclose(f);
        snprintf(response, buffer_size, "⚠️ Dữ liệu hoàn tác không hợp lệ.");
        action_undo_clear();
        return ESP_FAIL;
    }

    task_record_t *tasks = malloc(sizeof(task_record_t) * header.num_tasks);
    if (tasks == NULL) {
        fclose(f);
        snprintf(response, buffer_size, "⚠️ Lỗi cấp phát bộ nhớ khi hoàn tác.");
        return ESP_ERR_NO_MEM;
    }

    if (fread(tasks, sizeof(task_record_t), header.num_tasks, f) != (size_t)header.num_tasks) {
        fclose(f);
        free(tasks);
        snprintf(response, buffer_size, "⚠️ Lỗi đọc nội dung hoàn tác.");
        action_undo_clear();
        return ESP_FAIL;
    }
    fclose(f);

    int success_count = 0;
    uint32_t last_id = 0;
    // Bắt đầu khôi phục tuỳ theo ngữ cảnh
    for (int i = 0; i < header.num_tasks; i++) {
        esp_err_t err = ESP_FAIL;
        
        switch (header.action_type) {
            case UNDO_CREATE:
                // User vừa CREATE -> Undo là DELETE hard
                err = task_database_hard_delete(tasks[i].id);
                break;
                
            case UNDO_UPDATE:
            case UNDO_COMPLETE:
                // User vừa UPDATE hoặc COMPLETE -> Undo là ghi lại nội dung cũ
                err = restore_task_and_update_embedding(&tasks[i]);
                break;
                
            case UNDO_DELETE:
                // User vừa DELETE mềm -> Undo là tạo lại task / update lại status cũ
                // Trong TH này task.status=cancelled, mình lưu task CŨ có status pending
                err = restore_task_and_update_embedding(&tasks[i]);
                break;
        }

        if (err == ESP_OK) {
            success_count++;
            last_id = tasks[i].id;
        }
    }

    free(tasks);
    
    // Sau khi undo thành công, xoá file để không duplicate undo
    action_undo_clear();

    if (success_count > 0) {
        snprintf(response, buffer_size, "↩️ Đã hoàn tác thành công %d thao tác.", success_count);
        display_show_result("Hoan tac", last_id, "Thanh cong");
        return ESP_OK;
    } else {
        snprintf(response, buffer_size, "⚠️ Không thể hoàn tác các luồng công việc này.");
        return ESP_FAIL;
    }
}
