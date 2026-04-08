/**
 * ===========================================================================
 * @file action_complete.c
 * @brief Handler cho action COMPLETE_TASK
 * 
 * Sửa lỗi: Hiển thị task list khi có nhiều kết quả và lưu pending context.
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
#include "cJSON.h"
#include "telegram_bot.h"
#include "query_engine.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"

/* static const char *TAG = "action_complete"; */

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
    const char *status_filter_ai = NULL;
    const char *type_filter = NULL;

    uint32_t explicit_task_ids[MAX_TASK_COUNT];
    int explicit_count = 0;

    /* 1. Trích xuất ID đích danh */
    cJSON *task_ids_json = cJSON_GetObjectItem(data, "task_ids");
    if (task_ids_json != NULL && cJSON_IsArray(task_ids_json)) {
        cJSON *item;
        cJSON_ArrayForEach(item, task_ids_json) {
            if (cJSON_IsNumber(item) && explicit_count < MAX_TASK_COUNT) {
                explicit_task_ids[explicit_count++] = (uint32_t)item->valuedouble;
            }
        }
    }

    /* 2. Trích xuất filters */
    cJSON *filters_arr = cJSON_GetObjectItem(data, "filters");
    if (filters_arr && cJSON_IsArray(filters_arr)) {
        cJSON *f_item;
        cJSON_ArrayForEach(f_item, filters_arr) {
            const char *field = json_get_string(f_item, "field", "");
            if (strcmp(field, "type") == 0) type_filter = json_get_string(f_item, "value", NULL);
            if (strcmp(field, "status") == 0) status_filter_ai = json_get_string(f_item, "value", NULL);
        }
    }

    /* 3. Phân luồng xử lý */
    bool has_ids = (explicit_count > 0);
    bool has_query = (search_query && strlen(search_query) > 0);

    uint32_t last_completed_id = 0;
    char last_completed_title[128] = "";
    bool is_any_recurring = false;
    int total_completed = 0;

    if (has_ids) {
        /* LUỒNG A: Đã có ID */
        int written = snprintf(response, response_size, "✅ Đã hoàn thành %d việc:\n", explicit_count);
        task_record_t *old_tasks = malloc(sizeof(task_record_t) * explicit_count);
        int old_cnt = 0;

        for (int i = 0; i < explicit_count; i++) {
            task_record_t task;
            if (task_database_read(explicit_task_ids[i], &task) == ESP_OK) {
                if (strcmp(task.status, "done") == 0) continue;
                if (old_tasks) old_tasks[old_cnt++] = task;
                
                bool recurring = (strcmp(task.repeat, "none") != 0 && task.due_time > 0);
                if (recurring) {
                    is_any_recurring = true;
                    time_t old_due = task.due_time;
                    task.due_time = time_utils_next_due(old_due, task.repeat, task.repeat_interval);
                    if (task.reminder > 0) {
                        task.reminder = task.due_time - (old_due - task.reminder);
                    } else {
                        task.reminder = task.due_time;
                    }
                    task.completed_at = 0; strcpy(task.status, "pending");
                } else {
                    strcpy(task.status, "done");
                    task.completed_at = time_utils_get_now();
                }

                if (task_database_update(&task) == ESP_OK) {
                    task_database_log_history(task.title, time_utils_get_now());
                    last_completed_id = task.id;
                    strncpy(last_completed_title, task.title, sizeof(last_completed_title)-1);
                    total_completed++;
                    if (written < response_size) {
                        APPEND_SNPRINTF(response, response_size, written, " - [#%d] %s\n", (int)task.id, task.title);
                    }
                }
            }
        }
        if (old_tasks && old_cnt > 0) action_undo_save(UNDO_COMPLETE, old_tasks, old_cnt);
        if (old_tasks) free(old_tasks);
    } 
    else if (has_query) {
        /* LUỒNG B: Semantic Search */
        float query_embedding[EMBEDDING_DIM];
        if (openai_create_embedding(search_query, query_embedding, EMBEDDING_DIM) == ESP_OK) {
            search_result_t results[SEARCH_TOP_K];
            int found = 0;
            vector_search_find_similar(query_embedding, search_query, status_filter_ai ? status_filter_ai : "pending", type_filter, results, SEARCH_TOP_K, &found);

            if (found > 1) {
                /* Nhiều kết quả -> Liệt kê và hỏi lại */
                int offset = snprintf(response, response_size, "❓ Tìm thấy %d kết quả cho \"%s\". Bạn muốn hoàn thành cái nào?\n\n", found, search_query);
                
                const task_index_t *index = task_database_get_index();
                const task_index_entry_t **matches = calloc(found, sizeof(task_index_entry_t *));
                uint32_t *ctx_ids = malloc(sizeof(uint32_t) * found);
                int actual = 0;
                for (int i=0; i<found; i++) {
                    for(int j=0; j<index->count; j++) {
                        if(index->entries[j].id == results[i].task_id) {
                            matches[actual] = &index->entries[j];
                            if(ctx_ids) ctx_ids[actual] = results[i].task_id;
                            actual++; break;
                        }
                    }
                }
                
                if (actual > 0) {
                    format_task_list_short(matches, actual, "Kết quả tìm thấy", response + offset, response_size - offset);
                    offset = strlen(response); // Cập nhật offset sau khi liệt kê
                    APPEND_SNPRINTF(response, response_size, offset, "\n👉 Nhập ID (VD: 1, 2) để xác nhận.");
                    
                    /* Quan trọng: Trình điều phối sẽ dựa vào pending action để lắng nghe ID tiếp theo */
                    cJSON *p_json = cJSON_CreateObject();
                    cJSON_AddStringToObject(p_json, "search_query", "");
                    dispatcher_set_pending_action(ACTION_COMPLETE_TASK, cJSON_PrintUnformatted(p_json));
                    cJSON_Delete(p_json);
                    
                    if (ctx_ids) {
                        dispatcher_set_context_tasks(ctx_ids, actual);
                        free(ctx_ids);
                    }
                }
                free(matches);
                cJSON_Delete(data);
                return ESP_OK;
            } 
            else if (found == 1) {
                task_record_t task;
                if (task_database_read(results[0].task_id, &task) == ESP_OK) {
                    task_record_t old = task;
                    bool recurring = (strcmp(task.repeat, "none") != 0);
                    if (recurring) {
                        is_any_recurring = true;
                        time_t old_due = task.due_time;
                        task.due_time = time_utils_next_due(old_due, task.repeat, task.repeat_interval);
                        if (task.reminder > 0) task.reminder = task.due_time - (old_due - task.reminder);
                        else task.reminder = task.due_time;
                        task.completed_at = 0; strcpy(task.status, "pending");
                    } else {
                        strcpy(task.status, "done");
                        task.completed_at = time_utils_get_now();
                    }
                    if (task_database_update(&task) == ESP_OK) {
                        task_database_log_history(task.title, time_utils_get_now());
                        action_undo_save(UNDO_COMPLETE, &old, 1);
                        last_completed_id = task.id;
                        strncpy(last_completed_title, task.title, sizeof(last_completed_title)-1);
                        total_completed = 1;
                        if (recurring) {
                            char d_buf[64]; time_utils_format_vietnamese(task.due_time, d_buf, sizeof(d_buf));
                            snprintf(response, response_size, "✅ Đã hoàn thành và gia hạn việc lặp lại: %s. Hạn mới: %s", task.title, d_buf);
                        } else {
                            format_task_completed(&task, response, response_size);
                        }
                    }
                }
            }
        }
    }

    if (total_completed > 0) {
        if (total_completed == 1) {
            display_show_result(is_any_recurring ? "Lap lai" : "Hoan thanh", last_completed_id, last_completed_title);
        } else {
            char msg[32]; snprintf(msg, sizeof(msg), "Xong %d task", total_completed);
            display_show_result(msg, 0, "");
        }
        dispatcher_set_context_tasks(NULL, 0);
    } else if (strlen(response) == 0) {
        format_not_found(search_query, response, response_size);
    }

    cJSON_Delete(data);
    return ESP_OK;
}
