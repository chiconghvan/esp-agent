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
#include "cJSON.h"
#include "telegram_bot.h"
#include "query_engine.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"

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
    uint32_t explicit_task_ids[MAX_TASK_COUNT];
    int explicit_count = 0;

    /* 1. Trích xuất ID đích danh từ JSON */
    cJSON *task_ids_json = cJSON_GetObjectItem(data, "task_ids");
    if (task_ids_json != NULL && cJSON_IsArray(task_ids_json)) {
        cJSON *item;
        cJSON_ArrayForEach(item, task_ids_json) {
            if (cJSON_IsNumber(item) && explicit_count < MAX_TASK_COUNT) {
                explicit_task_ids[explicit_count++] = (uint32_t)item->valuedouble;
            }
        }
    }

    /* 2. Trích xuất Filter (nếu không có ID) */
    cJSON *filters_arr = cJSON_GetObjectItem(data, "filters");
    if (explicit_count == 0 && filters_arr != NULL && cJSON_IsArray(filters_arr)) {
        query_filter_t filters[MAX_FILTERS];
        int filter_cnt = query_engine_parse_filters(filters_arr, filters, MAX_FILTERS);
        query_engine_execute(filters, filter_cnt, explicit_task_ids, MAX_TASK_COUNT, &explicit_count);
    }

    char query_buf[256];
    strncpy(query_buf, search_query, sizeof(query_buf) - 1);
    query_buf[sizeof(query_buf) - 1] = '\0';
    cJSON_Delete(data);

    /* Biến lưu kết quả cuối cùng để hiển thị LCD */
    uint32_t last_completed_id = 0;
    char last_completed_title[128] = "";
    bool is_any_recurring = false;
    int total_completed = 0;

    if (explicit_count > 0) {
        /* TH1: Hoàn thành theo danh sách ID (trực tiếp hoặc qua Filter) */
        int written = snprintf(response, response_size, "✅ Đã xong %d việc:\n", explicit_count);
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
                    if (task.reminder > 0) task.reminder = task.due_time - (old_due - task.reminder);
                    task.completed_at = 0;
                    strcpy(task.status, "pending");
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
                        if (recurring) {
                            char d_buf[64];
                            time_utils_format_vietnamese(task.due_time, d_buf, sizeof(d_buf));
                            APPEND_SNPRINTF(response, response_size, written, " - [#%d] %s (Hạn mới: %s)\n", (int)task.id, task.title, d_buf);
                        } else {
                            APPEND_SNPRINTF(response, response_size, written, " - [#%d] %s\n", (int)task.id, task.title);
                        }
                    }
                }
            }
        }
        if (old_tasks && old_cnt > 0) action_undo_save(UNDO_COMPLETE, old_tasks, old_cnt);
        if (old_tasks) free(old_tasks);
    } 
    else if (strlen(query_buf) > 0) {
        /* TH2: Semantic Search */
        float query_embedding[EMBEDDING_DIM];
        if (openai_create_embedding(query_buf, query_embedding, EMBEDDING_DIM) == ESP_OK) {
            search_result_t results[SEARCH_TOP_K];
            int found = 0;
            vector_search_find_similar(query_embedding, query_buf, "pending", results, SEARCH_TOP_K, &found);

            if (found > 1) {
                /* Nhiều kết quả -> Hỏi lại (Logic này giữ nguyên như cũ) */
                snprintf(response, response_size, "❓ Tìm thấy nhiều việc cho \"%s\". Cái nào ạ?\n", query_buf);
                // ... (phần format list kết quả và set pending action giống file cũ)
                // Do phần này phức tạp và mang tính tương tác, tạm thời tôi copy lại logic cũ cho đoạn này
                // Để tiết kiệm không gian, tôi sẽ tinh chỉnh logic ở bước tiếp theo nếu cần
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
                int cur_len = strlen(response);
                format_task_list_short(matches, actual, "Kết quả", response + cur_len, response_size - cur_len);
                APPEND_SNPRINTF(response, response_size, cur_len, "\n👉 Nhập ID để xong.");
                cJSON *p_json = cJSON_CreateObject();
                cJSON_AddStringToObject(p_json, "search_query", "");
                dispatcher_set_pending_action(ACTION_COMPLETE_TASK, cJSON_PrintUnformatted(p_json));
                cJSON_Delete(p_json);
                if (ctx_ids) { dispatcher_set_context_tasks(ctx_ids, actual); free(ctx_ids); }
                free(matches);
                return ESP_OK;
            } 
            else if (found == 1) {
                task_record_t task;
                if (task_database_read(results[0].task_id, &task) == ESP_OK) {
                    task_record_t old = task;
                    bool recurring = (strcmp(task.repeat, "none") != 0);
                    if (recurring) {
                        is_any_recurring = true;
                        task.due_time = time_utils_next_due(task.due_time, task.repeat, task.repeat_interval);
                        task.completed_at = 0;
                        strcpy(task.status, "pending");
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
                            snprintf(response, response_size, "✅ Xong việc lặp lại: %s. Hạn mới: %s", task.title, d_buf);
                        } else {
                            format_task_completed(&task, response, response_size);
                        }
                    }
                }
            }
        }
    }

    /* CHỐT HẠ: LUÔN HIỂN THỊ LCD NẾU CÓ THAY ĐỔI */
    if (total_completed > 0) {
        if (total_completed == 1) {
            display_show_result(is_any_recurring ? "Lap lai" : "Hoan thanh", last_completed_id, last_completed_title);
        } else {
            char msg[32]; snprintf(msg, sizeof(msg), "Xong %d task", total_completed);
            display_show_result(msg, 0, "");
        }
        
        // KHÔNG gọi display_show_idle() ở đây vì nó sẽ ghi đè popup ngay lập tức.
        // Popup sẽ tự động biến mất và quay về Idle sau vài giây nhờ Display Task.
        
        dispatcher_set_context_tasks(NULL, 0);
        return ESP_OK;
    }

    if (total_completed == 0 && strlen(response) == 0) {
        format_not_found(query_buf, response, response_size);
    }
    return ESP_OK;
}
