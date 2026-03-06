/**
 * ===========================================================================
 * @file action_create.c
 * @brief Handler cho action CREATE_TASK
 *
 * Parse dữ liệu task từ LLM → tạo embedding → lưu task → trả response.
 * Gọi API: Classification (đã gọi) + Embedding (1 lần cho title).
 * ===========================================================================
 */

#include "action_dispatcher.h"
#include "task_database.h"
#include "action_undo.h"
#include "vector_search.h"
#include "openai_client.h"
#include "json_parser.h"
#include "time_utils.h"
#include "response_formatter.h"
#include "display_manager.h"
#include "config.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "action_create";

esp_err_t action_create_task(const char *data_json, char *response, size_t response_size)
{
    if (data_json == NULL) {
        snprintf(response, response_size,
            "\xE2\x9A\xA0\xEF\xB8\x8F Thiếu thông tin để tạo công việc.");
        return ESP_FAIL;
    }

    cJSON *data = json_parse_string(data_json);
    if (data == NULL) {
        snprintf(response, response_size,
            "\xE2\x9A\xA0\xEF\xB8\x8F Lỗi xử lý dữ liệu.");
        return ESP_FAIL;
    }

    /* Parse thông tin task từ LLM response */
    task_record_t task = {0};

    const char *title = json_get_string(data, "title", "");
    strncpy(task.title, title, sizeof(task.title) - 1);

    const char *type = json_get_string(data, "type", "other");
    strncpy(task.type, type, sizeof(task.type) - 1);

    strncpy(task.status, "pending", sizeof(task.status) - 1);

    /* Parse thời gian */
    const char *start_time_str = json_get_string(data, "start_time", NULL);
    if (start_time_str != NULL) {
        task.start_time = time_utils_parse_iso8601(start_time_str);
    }

    const char *due_time_str = json_get_string(data, "due_time", NULL);
    if (due_time_str != NULL) {
        task.due_time = time_utils_parse_iso8601(due_time_str);
    }

    const char *reminder_str = json_get_string(data, "reminder", NULL);
    if (reminder_str != NULL) {
        task.reminder = time_utils_parse_iso8601(reminder_str);
    }

    /* Parse repeat */
    const char *repeat = json_get_string(data, "repeat", "none");
    strncpy(task.repeat, repeat, sizeof(task.repeat) - 1);

    task.repeat_interval = (int)json_get_int(data, "repeat_interval", 0);
    if (task.repeat_interval <= 0 && strcmp(task.repeat, "none") != 0) {
        task.repeat_interval = 1;
    }

    /* Parse notes */
    const char *notes = json_get_string(data, "notes", "");
    strncpy(task.notes, notes, sizeof(task.notes) - 1);

    cJSON_Delete(data);

    /* Kiểm tra title không rỗng */
    if (strlen(task.title) == 0) {
        snprintf(response, response_size,
            "\xE2\x9A\xA0\xEF\xB8\x8F Không xác định được tên công việc.");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Tạo task: %s (type=%s)", task.title, task.type);

    /* Tạo task trong database */
    esp_err_t err = task_database_create(&task);
    if (err != ESP_OK) {
        snprintf(response, response_size,
            "\xE2\x9A\xA0\xEF\xB8\x8F Lỗi lưu công việc vào database.");
        return err;
    }

    /* Lưu Undo Log (với action CREATE) */
    action_undo_save(UNDO_CREATE, &task, 1);

    /* Tạo embedding cho title */
    float embedding[EMBEDDING_DIM];
    err = openai_create_embedding(task.title, embedding, EMBEDDING_DIM);
    if (err == ESP_OK) {
        vector_search_save(task.id, embedding);
        ESP_LOGI(TAG, "Đã lưu embedding cho task #%" PRIu32, task.id);
    } else {
        ESP_LOGW(TAG, "Không thể tạo embedding (task vẫn được lưu)");
    }

    /* Format response */
    format_task_created(&task, response, response_size);

    /* Cập nhật màn hình LCD */
    display_show_result("Tao task", task.id, task.title);

    ESP_LOGI(TAG, "Task #%" PRIu32 " đã tạo thành công", task.id);
    return ESP_OK;
}
