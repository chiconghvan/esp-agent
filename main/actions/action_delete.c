/**
 * ===========================================================================
 * @file action_delete.c
 * @brief Handler cho action DELETE_TASK
 *
 * Hỗ trợ 2 chế độ:
 *   - soft: set status = cancelled (hủy, bỏ)
 *   - hard: xóa file + index + embedding (xóa hẳn, delete)
 * ===========================================================================
 */

#include "safe_append.h"
#include "action_dispatcher.h"
#include "task_database.h"
#include "action_undo.h"
#include "vector_search.h"
#include "openai_client.h"
#include "json_parser.h"
#include "response_formatter.h"
#include "config.h"
#include "time_utils.h"
#include "display_manager.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "cJSON.h"
#include "telegram_bot.h"

static const char *TAG = "action_delete";

/* Bộ nhớ đệm lưu ID các task đang chờ xác nhận xóa vĩnh viễn */
static uint32_t s_pending_deletes[20];
static int s_pending_count = 0;

esp_err_t action_delete_task(const char *data_json, char *response, size_t response_size)
{
    if (data_json == NULL) {
        format_not_found("để xóa", response, response_size);
        return ESP_FAIL;
    }

    cJSON *data = json_parse_string(data_json);
    if (data == NULL) {
        format_error("Lỗi xử lý dữ liệu", response, response_size);
        return ESP_FAIL;
    }

    const char *search_query = json_get_string(data, "search_query", "");
    const char *delete_mode = json_get_string(data, "delete_mode", "soft");

    uint32_t explicit_task_ids[20];
    int explicit_count = 0;
    cJSON *task_ids_json = cJSON_GetObjectItem(data, "task_ids");
    if (task_ids_json != NULL && cJSON_IsArray(task_ids_json)) {
        cJSON *item;
        cJSON_ArrayForEach(item, task_ids_json) {
            if (cJSON_IsNumber(item) && explicit_count < 20) {
                explicit_task_ids[explicit_count++] = (uint32_t)item->valuedouble;
            }
        }
    }

    char query_buf[256];
    strncpy(query_buf, search_query, sizeof(query_buf) - 1);
    query_buf[sizeof(query_buf) - 1] = '\0';

    bool is_hard = (strcmp(delete_mode, "hard") == 0);
    cJSON_Delete(data);

    if (is_hard) {
        /* Chế độ CHẶN: Lưu vào danh sách chờ và yêu cầu /confirm */
        s_pending_count = 0; // Reset danh sách cũ

        if (explicit_count > 0) {
            int written = snprintf(response, response_size, 
                "⚠️ **XÁC NHẬN XÓA VĨNH VIỄN**\n"
                "Danh sách %d task chờ xóa:\n", explicit_count);
            
            for (int i = 0; i < explicit_count; i++) {
                task_record_t t;
                if (task_database_read(explicit_task_ids[i], &t) == ESP_OK) {
                    s_pending_deletes[s_pending_count++] = explicit_task_ids[i];
                    APPEND_SNPRINTF(response, response_size, written,
                        " - [#%" PRIu32 "] %s\n", t.id, t.title);
                }
            }
            snprintf(response + written, response_size - written, 
                "\n👉 Gõ hoặc bấm `/confirm` để xóa sạch các task trên.");
            return ESP_OK;
        } else {
            /* Tìm kiếm semantic */
            float query_embedding[EMBEDDING_DIM];
            if (openai_create_embedding(query_buf, query_embedding, EMBEDDING_DIM) != ESP_OK) {
                format_error("Lỗi AI", response, response_size);
                return ESP_FAIL;
            }
            search_result_t res[1];
            int f = 0;
            vector_search_find_similar(query_embedding, query_buf, NULL, res, 1, &f);
            if (f == 0) {
                format_not_found(query_buf, response, response_size);
                return ESP_OK;
            }
            task_record_t t;
            task_database_read(res[0].task_id, &t);
            s_pending_deletes[s_pending_count++] = t.id;
            
            snprintf(response, response_size,
                "⚠️ **XÁC NHẬN XÓA VĨNH VIỄN**\n"
                "Task: [#%" PRIu32 "] %s\n\n"
                "👉 Gõ hoặc bấm `/confirm` để xác nhận xóa.",
                t.id, t.title);
            return ESP_OK;
        }
    }

    /* Logic Xóa mềm (Hủy) - Thực hiện ngay */
    if (explicit_count > 0) {
        int deleted_count = 0;
        int written = snprintf(response, response_size, "🚫 Đã hủy %d công việc:\n", explicit_count);

        /* Array to hold old tasks for Undo */
        task_record_t *old_tasks = malloc(sizeof(task_record_t) * explicit_count);
        int old_tasks_count = 0;

        for (int i = 0; i < explicit_count; i++) {
            task_record_t task;
            if (task_database_read(explicit_task_ids[i], &task) == ESP_OK) {
                if (old_tasks) {
                    old_tasks[old_tasks_count++] = task;
                }

                if (task_database_soft_delete(explicit_task_ids[i]) == ESP_OK) {
                    APPEND_SNPRINTF(response, response_size, written,
                        " - [#%" PRIu32 "] %s\n", task.id, task.title);
                    deleted_count++;
                }
            }
        }
        
        if (old_tasks && old_tasks_count > 0) {
            action_undo_save(UNDO_DELETE, old_tasks, old_tasks_count);
        }
        if (old_tasks) free(old_tasks);

        if (deleted_count == 0) format_not_found("theo ID", response, response_size);
        return ESP_OK;
    } else {
        if (strlen(query_buf) == 0) {
            format_not_found("(không có mô tả)", response, response_size);
            return ESP_FAIL;
        }
        float query_embedding[EMBEDDING_DIM];
        openai_create_embedding(query_buf, query_embedding, EMBEDDING_DIM);
        search_result_t search_results[SEARCH_TOP_K];
        int found_count = 0;
        vector_search_find_similar(query_embedding, query_buf, NULL, search_results, SEARCH_TOP_K, &found_count);
        
        if (found_count == 0) {
            format_not_found(query_buf, response, response_size);
            return ESP_OK;
        }

        /* Nếu tìm thấy nhiều kết quả phù hợp -> Hỏi lại user */
        if (found_count > 1) {
            const task_index_t *index = task_database_get_index();
            const task_index_entry_t **matches = calloc(found_count, sizeof(task_index_entry_t *));
            if (!matches) return ESP_ERR_NO_MEM;

            uint32_t *ctx_ids = malloc(sizeof(uint32_t) * found_count);
            int actual_matches = 0;
            for (int i = 0; i < found_count; i++) {
                uint32_t tid = search_results[i].task_id;
                for (int j = 0; j < index->count; j++) {
                    if (index->entries[j].id == tid) {
                        matches[actual_matches] = &index->entries[j];
                        if (ctx_ids) ctx_ids[actual_matches] = tid;
                        actual_matches++;
                        break;
                    }
                }
            }

            snprintf(response, response_size, "❓ <b>Tìm thấy nhiều kết quả cho \"%s\". Bạn muốn hủy cái nào?</b>\n\n", query_buf);
            int current_len = strlen(response);
            format_task_list_short(matches, actual_matches, "Kết quả tìm thấy", response + current_len, response_size - current_len);
            
            // Cập nhật lại độ dài thực tế
            current_len = strlen(response);

            APPEND_SNPRINTF(response, response_size, current_len, "\n\n👉 <i>Vui lòng nhập ID task (VD: 1, 2) để hủy.</i>");

            // Lưu pending action
            cJSON *pending_json = cJSON_CreateObject();
            cJSON_AddStringToObject(pending_json, "search_query", "");
            cJSON_AddStringToObject(pending_json, "delete_mode", "soft");
            dispatcher_set_pending_action(ACTION_DELETE_TASK, cJSON_PrintUnformatted(pending_json));
            cJSON_Delete(pending_json);
            
            if (ctx_ids) {
                dispatcher_set_context_tasks(ctx_ids, actual_matches);
                free(ctx_ids);
            }
            free(matches);
            return ESP_OK;
        }

        uint32_t target_id = search_results[0].task_id;
        task_record_t task;
        task_database_read(target_id, &task);
        
        /* Lưu trạng thái cũ để Undo */
        task_record_t old_task = task;

        task_database_soft_delete(target_id);
        
        /* Lưu undo log */
        action_undo_save(UNDO_DELETE, &old_task, 1);

        snprintf(response, response_size, "🚫 Đã hủy task #%" PRIu32 ": %s", target_id, task.title);
        display_show_result("Huy task", target_id, task.title);
        return ESP_OK;
    }
}

/* Hàm phụ để dispatcher gọi thực thi lệnh /confirm */
esp_err_t action_delete_confirm_hard(char *response, size_t response_size)
{
    if (s_pending_count == 0) {
        snprintf(response, response_size, "ℹ️ Không có lệnh xóa nào đang chờ xác nhận.");
        return ESP_OK;
    }

    int success_count = 0;
    int written = snprintf(response, response_size, "🗑️ **ĐÃ XÓA VĨNH VIỄN**\n───────────────\n");

    for (int i = 0; i < s_pending_count; i++) {
        uint32_t id = s_pending_deletes[i];
        task_record_t t;
        if (task_database_read(id, &t) == ESP_OK) {
            char title_tmp[64];
            strncpy(title_tmp, t.title, 63); title_tmp[63] = '\0';

            if (task_database_hard_delete(id) == ESP_OK) {
                vector_search_delete(id);
                APPEND_SNPRINTF(response, response_size, written, "✅ #%" PRIu32 ": %s\n", id, title_tmp);
                success_count++;
            }
        }
    }

    if (success_count > 0) {
        snprintf(response + written, response_size - written, "───────────────\n🚀 Thành công %d task.", success_count);
        display_show_result("Xoa xong", success_count, "tasks");
        
        /* Delete Undo Queue since we did a hard delete */
        action_undo_clear();
    } else {
        snprintf(response, response_size, "❌ Không thể thực hiện xóa. Có lỗi xảy ra.");
    }

    s_pending_count = 0; // Xóa sạch hàng đợi sau khi dùng
    ESP_LOGI(TAG, "Đã hoàn thành xác nhận xóa vĩnh viễn.");
    return ESP_OK;
}
