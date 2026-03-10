/**
 * ===========================================================================
 * @file action_complete.c
 * @brief Handler cho action COMPLETE_TASK
 *
 * Tìm task bằng semantic search → đánh dấu hoàn thành.
 * Nếu task có repeat → tự động tạo bản tiếp theo.
 * Gọi API: Embedding (1 lần cho search query).
 * ===========================================================================
 */

#include "safe_append.h"
#include "action_dispatcher.h"
#include "task_database.h"
#include "action_undo.h"
#include "vector_search.h"
#include "openai_client.h"
#include "json_parser.h"
#include "time_utils.h"
#include "display_manager.h"
#include "response_formatter.h"
#include "config.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "action_complete";

esp_err_t action_complete_task(const char *data_json, char *response, size_t response_size)
{
    if (data_json == NULL) {
        format_not_found("để đánh dấu hoàn thành", response, response_size);
        return ESP_FAIL;
    }

    cJSON *data = json_parse_string(data_json);
    if (data == NULL) {
        format_error("Lỗi xử lý dữ liệu", response, response_size);
        return ESP_FAIL;
    }

    const char *search_query = json_get_string(data, "search_query", "");

    uint32_t explicit_task_ids[10];
    int explicit_count = 0;
    cJSON *task_ids_json = cJSON_GetObjectItem(data, "task_ids");
    if (task_ids_json != NULL && cJSON_IsArray(task_ids_json)) {
        cJSON *item;
        cJSON_ArrayForEach(item, task_ids_json) {
            if (cJSON_IsNumber(item) && explicit_count < 10) {
                explicit_task_ids[explicit_count++] = (uint32_t)item->valuedouble;
            }
        }
    }

    /* Copy query trước khi delete cJSON */
    char query_buf[256];
    strncpy(query_buf, search_query, sizeof(query_buf) - 1);
    query_buf[sizeof(query_buf) - 1] = '\0';

    cJSON_Delete(data);

    if (explicit_count > 0) {
        /* Đánh dấu hoàn thành hàng loạt */
        int completed_count = 0;
        int written = snprintf(response, response_size, "\xE2\x9C\x85 Đã hoàn thành %d công việc:\n", explicit_count);

        /* Array to hold old tasks for Undo */
        task_record_t *old_tasks = malloc(sizeof(task_record_t) * explicit_count);
        int old_tasks_count = 0;

        for (int i = 0; i < explicit_count; i++) {
            task_record_t task;
            if (task_database_read(explicit_task_ids[i], &task) == ESP_OK) {
                if (strcmp(task.status, "done") != 0) {
                    if (old_tasks) {
                        old_tasks[old_tasks_count++] = task;
                    }
                    strncpy(task.status, "done", sizeof(task.status) - 1);
                    task.completed_at = time_utils_get_now();
                    
                    if (task_database_update(&task) == ESP_OK) {
                        /* Xử lý lặp lại nếu có */
                        if (strcmp(task.repeat, "none") != 0 && task.due_time > 0) {
                            task_record_t next_task = task;
                            memset(next_task.status, 0, sizeof(next_task.status));
                            strncpy(next_task.status, "pending", sizeof(next_task.status) - 1);
                            next_task.completed_at = 0;
                            next_task.due_time = time_utils_next_due(task.due_time, task.repeat, task.repeat_interval);

                            if (next_task.reminder > 0) {
                                time_t reminder_offset = task.due_time - task.reminder;
                                next_task.reminder = next_task.due_time - reminder_offset;
                            }

                            if (task_database_create(&next_task) == ESP_OK) {
                                float emb[EMBEDDING_DIM];
                                if (openai_create_embedding(next_task.title, emb, EMBEDDING_DIM) == ESP_OK) {
                                    vector_search_save(next_task.id, emb);
                                }
                            }
                        }
                        
                        if (written < response_size) {
                            APPEND_SNPRINTF(response, response_size, written, 
                                " - [#%" PRIu32 "] %s\n", task.id, task.title);
                        }
                        completed_count++;
                    }
                }
            }
        }

        if (old_tasks && old_tasks_count > 0) {
            action_undo_save(UNDO_COMPLETE, old_tasks, old_tasks_count);
        }
        if (old_tasks) free(old_tasks);

        if (completed_count == 0) {
            format_not_found("theo ID yêu cầu, hoặc đã hoàn thành rồi", response, response_size);
        } else {
            if (written < response_size) {
                snprintf(response + written, response_size - written, "\n\xF0\x9F\x8E\x89 Tốt lắm!");
            }
        }
        dispatcher_set_context_tasks(NULL, 0);
        return ESP_OK;

    } else {
        /* Tìm kiếm semantic search */
        if (strlen(query_buf) == 0) {
            format_not_found("(không có mô tả hoặc ID)", response, response_size);
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "Complete task semantic search: %s", query_buf);

        /* Tạo embedding cho search query */
        float query_embedding[EMBEDDING_DIM];
        esp_err_t err = openai_create_embedding(query_buf, query_embedding, EMBEDDING_DIM);
        if (err != ESP_OK) {
            format_error("Không thể tạo embedding cho tìm kiếm", response, response_size);
            return err;
        }

        /* Tìm task matching cao nhất */
        search_result_t search_results[SEARCH_TOP_K];
        int found_count = 0;

        err = vector_search_find_similar(query_embedding, search_results,
                                          SEARCH_TOP_K, &found_count);

        if (err != ESP_OK || found_count == 0) {
            format_not_found(query_buf, response, response_size);
            return ESP_OK;
        }

        uint32_t target_task_id = search_results[0].task_id;
        float best_similarity = search_results[0].similarity;

        ESP_LOGI(TAG, "Best match: task #%" PRIu32 " (similarity=%.3f)", target_task_id, best_similarity);

        /* Đọc task chi tiết */
        task_record_t task;
        err = task_database_read(target_task_id, &task);
        if (err != ESP_OK) {
            format_not_found("theo ID yêu cầu", response, response_size);
            return ESP_OK; 
        }

        /* Kiểm tra task đã hoàn thành chưa */
        if (strcmp(task.status, "done") == 0) {
            snprintf(response, response_size,
                "\xE2\x84\xB9\xEF\xB8\x8F Công việc \"%s\" đã được hoàn thành trước đó.",
                task.title);
            return ESP_OK;
        }

        /* Lưu trạng thái cũ để Undo */
        task_record_t old_task = task;

        /* Đánh dấu hoàn thành */
        strncpy(task.status, "done", sizeof(task.status) - 1);
        task.completed_at = time_utils_get_now();

        err = task_database_update(&task);
        if (err != ESP_OK) {
            format_error("Không thể cập nhật task", response, response_size);
            return err;
        }

        /* Nếu có repeat → tạo task mới cho lần tiếp theo */
        if (strcmp(task.repeat, "none") != 0 && task.due_time > 0) {
            task_record_t next_task = task;
            memset(next_task.status, 0, sizeof(next_task.status));
            strncpy(next_task.status, "pending", sizeof(next_task.status) - 1);
            next_task.completed_at = 0;
            next_task.due_time = time_utils_next_due(
                task.due_time, task.repeat, task.repeat_interval);

            if (next_task.reminder > 0) {
                /* Tính reminder mới tương ứng */
                time_t reminder_offset = task.due_time - task.reminder;
                next_task.reminder = next_task.due_time - reminder_offset;
            }

            esp_err_t create_err = task_database_create(&next_task);
            if (create_err == ESP_OK) {
                /* Tạo embedding cho task mới (dùng lại title) */
                float emb[EMBEDDING_DIM];
                if (openai_create_embedding(next_task.title, emb, EMBEDDING_DIM) == ESP_OK) {
                    vector_search_save(next_task.id, emb);
                }

                char next_due_buf[64];
                time_utils_format_vietnamese(next_task.due_time, next_due_buf, sizeof(next_due_buf));
                ESP_LOGI(TAG, "Đã tạo task lặp lại #%" PRIu32 ", due: %s",
                         next_task.id, next_due_buf);
            }
        }

        /* Lưu undo log */
        action_undo_save(UNDO_COMPLETE, &old_task, 1);

        /* Format response */
        format_task_completed(&task, response, response_size);

        /* Cập nhật màn hình LCD */
        display_show_result("Hoan thanh", task.id, task.title);

        return ESP_OK;
    }
}
