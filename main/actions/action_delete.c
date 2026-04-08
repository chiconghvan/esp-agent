/**
 * ===========================================================================
 * @file action_delete.c
 * @brief Handler cho action DELETE_TASK
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
#include "query_engine.h"

/* static const char *TAG = "action_delete"; */

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
    const char *type_filter = NULL;
    const char *status_filter_ai = NULL;

    uint32_t explicit_task_ids[MAX_TASK_COUNT];
    int explicit_count = 0;

    /* 1. ID đích danh */
    cJSON *task_ids_json = cJSON_GetObjectItem(data, "task_ids");
    if (task_ids_json != NULL && cJSON_IsArray(task_ids_json)) {
        cJSON *item;
        cJSON_ArrayForEach(item, task_ids_json) {
            if (cJSON_IsNumber(item) && explicit_count < MAX_TASK_COUNT) {
                explicit_task_ids[explicit_count++] = (uint32_t)item->valuedouble;
            }
        }
    }

    /* 2. Filters */
    cJSON *filters_arr = cJSON_GetObjectItem(data, "filters");
    if (filters_arr && cJSON_IsArray(filters_arr)) {
        cJSON *f_item;
        cJSON_ArrayForEach(f_item, filters_arr) {
            const char *field = json_get_string(f_item, "field", "");
            if (strcmp(field, "type") == 0) type_filter = json_get_string(f_item, "value", NULL);
            if (strcmp(field, "status") == 0) status_filter_ai = json_get_string(f_item, "value", NULL);
        }
    }

    bool has_ids = (explicit_count > 0);
    bool has_query = (search_query && strlen(search_query) > 0);
    bool is_hard = (strcmp(delete_mode, "hard") == 0);

    /* 3. Phân luồng xử lý */
    if (has_ids) {
        if (is_hard) {
            s_pending_count = 0;
            int written = snprintf(response, response_size, "⚠️ **XÁC NHẬN XÓA VĨNH VIỄN**\nDanh sách %d task chờ xóa:\n", explicit_count);
            for (int i = 0; i < explicit_count; i++) {
                task_record_t t;
                if (task_database_read(explicit_task_ids[i], &t) == ESP_OK) {
                    s_pending_deletes[s_pending_count++] = explicit_task_ids[i];
                    APPEND_SNPRINTF(response, response_size, written, " - [#%" PRIu32 "] %s\n", t.id, t.title);
                }
            }
            snprintf(response + written, response_size - written, "\n👉 Gõ `/confirm` để xóa sạch.");
            cJSON_Delete(data); return ESP_OK;
        } else {
            int written = snprintf(response, response_size, "🚫 Đã hủy %d công việc:\n", explicit_count);
            task_record_t *old_tasks = malloc(sizeof(task_record_t) * explicit_count);
            int old_cnt = 0;
            for (int i = 0; i < explicit_count; i++) {
                task_record_t task;
                if (task_database_read(explicit_task_ids[i], &task) == ESP_OK) {
                    if (old_tasks) old_tasks[old_cnt++] = task;
                    if (task_database_soft_delete(explicit_task_ids[i]) == ESP_OK) {
                        APPEND_SNPRINTF(response, response_size, written, " - [#%d] %s\n", (int)task.id, task.title);
                    }
                }
            }
            if (old_tasks && old_cnt > 0) action_undo_save(UNDO_DELETE, old_tasks, old_cnt);
            if (old_tasks) free(old_tasks);
            cJSON_Delete(data); return ESP_OK;
        }
    }
    else if (has_query) {
        float query_embedding[EMBEDDING_DIM];
        if (openai_create_embedding(search_query, query_embedding, EMBEDDING_DIM) == ESP_OK) {
            search_result_t results[SEARCH_TOP_K]; int found = 0;
            vector_search_find_similar(query_embedding, search_query, status_filter_ai, type_filter, results, SEARCH_TOP_K, &found);

            if (found > 1) {
                int offset = snprintf(response, response_size, "❓ Có nhiều kết quả cho \"%s\". Bạn muốn xóa cái nào?\n\n", search_query);
                const task_index_t *idx = task_database_get_index();
                const task_index_entry_t **matches = calloc(found, sizeof(task_index_entry_t *));
                uint32_t *ctx_ids = malloc(sizeof(uint32_t) * found);
                int act = 0;
                for (int i=0; i<found; i++) {
                    for(int j=0; j<idx->count; j++) { if(idx->entries[j].id == results[i].task_id) { matches[act] = &idx->entries[j]; if(ctx_ids) ctx_ids[act] = results[i].task_id; act++; break; } }
                }
                
                if (act > 0) {
                    format_task_list_short(matches, act, "Kết quả tìm thấy", response + offset, response_size - offset);
                    offset = strlen(response);
                    APPEND_SNPRINTF(response, response_size, offset, "\n👉 Nhập ID (VD: 1, 2) để xác nhận.");
                    
                    cJSON_AddItemToObject(data, "task_ids", cJSON_CreateArray());
                    cJSON_SetValuestring(cJSON_GetObjectItem(data, "search_query"), "");
                    dispatcher_set_pending_action(ACTION_DELETE_TASK, cJSON_PrintUnformatted(data));
                    if (ctx_ids) { dispatcher_set_context_tasks(ctx_ids, act); free(ctx_ids); }
                }
                free(matches); cJSON_Delete(data); return ESP_OK;
            }
            else if (found == 1) {
                uint32_t tid = results[0].task_id;
                task_record_t t; task_database_read(tid, &t);
                if (is_hard) {
                    s_pending_count = 1; s_pending_deletes[0] = tid;
                    snprintf(response, response_size, "⚠️ **XÁC NHẬN XÓA VĨNH VIỄN**\nTask: [#%d] %s\n\n👉 Gõ `/confirm` để xác nhận.", (int)tid, t.title);
                } else {
                    task_record_t old = t;
                    task_database_soft_delete(tid);
                    action_undo_save(UNDO_DELETE, &old, 1);
                    snprintf(response, response_size, "🚫 Đã hủy task #%d: %s", (int)tid, t.title);
                    display_show_result("Huy task", tid, t.title);
                }
            }
        }
    }

    if (strlen(response) == 0) format_not_found(search_query, response, response_size);
    cJSON_Delete(data);
    return ESP_OK;
}

esp_err_t action_delete_confirm_hard(char *response, size_t response_size)
{
    if (s_pending_count == 0) { snprintf(response, response_size, "ℹ️ Không có lệnh xóa nào chờ xác nhận."); return ESP_OK; }
    int success_count = 0;
    int written = snprintf(response, response_size, "🗑️ **ĐÃ XÓA VĨNH VIỄN**\n───────────────\n");
    task_record_t *deleted_tasks = malloc(sizeof(task_record_t) * s_pending_count);
    int del_cnt = 0;
    for (int i = 0; i < s_pending_count; i++) {
        uint32_t id = s_pending_deletes[i]; task_record_t t;
        if (task_database_read(id, &t) == ESP_OK) {
            if (deleted_tasks) deleted_tasks[del_cnt++] = t;
            if (task_database_hard_delete(id) == ESP_OK) {
                vector_search_delete(id);
                APPEND_SNPRINTF(response, response_size, written, "✅ #%d: %s\n", (int)id, t.title);
                success_count++;
            }
        }
    }
    if (success_count > 0) {
        snprintf(response + written, response_size - written, "───────────────\n🚀 Thành công %d task.", success_count);
        display_show_result("Xoa xong", success_count, "tasks");
        if (deleted_tasks && del_cnt > 0) action_undo_save(UNDO_DELETE, deleted_tasks, del_cnt);
    }
    if (deleted_tasks) free(deleted_tasks);
    s_pending_count = 0; return ESP_OK;
}
