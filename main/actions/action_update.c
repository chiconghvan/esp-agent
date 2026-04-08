/**
 * ===========================================================================
 * @file action_update.c
 * @brief Handler cho action UPDATE_TASK
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
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "cJSON.h"
#include "query_engine.h"

/* static const char *TAG = "action_update"; */

static const char *normalize_type(const char *type)
{
    if (type == NULL) return "other";
    if (strcmp(type, "meeting") == 0 || strcmp(type, "report") == 0 ||
        strcmp(type, "reminder") == 0 || strcmp(type, "event") == 0 ||
        strcmp(type, "anniversary") == 0 || strcmp(type, "other") == 0) {
        return type;
    }
    if (strstr(type, "nh\xe1\xba" "\xaf" "c") != NULL) return "reminder";
    if (strstr(type, "b\xc3\xa1o c\xc3\xa1o") != NULL) return "report";
    if (strstr(type, "cu\xe1\xbb\x99" "c h\xe1\xbb\x8dp") != NULL) return "meeting";
    if (strstr(type, "s\xe1\xbb\xb1" " ki\xe1\xbb\x87n") != NULL) return "event";
    if (strstr(type, "k\xe1\xbb\x89 ni\xe1\xbb\x87m") != NULL) return "anniversary";
    return "other";
}

esp_err_t action_update_task(const char *data_json, char *response, size_t response_size)
{
    if (data_json == NULL) {
        format_not_found("để cập nhật", response, response_size);
        return ESP_FAIL;
    }

    cJSON *data = json_parse_string(data_json);
    if (data == NULL) {
        format_error("Lỗi xử lý dữ liệu", response, response_size);
        return ESP_FAIL;
    }

    const char *search_query = json_get_string(data, "search_query", "");
    cJSON *updates = json_get_object(data, "updates");
    cJSON *fields = (updates != NULL) ? updates : data;

    const char *status_filter_ai = NULL;
    const char *type_filter = NULL;

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

    /* 3. Phân luồng xử lý */
    if (has_ids) {
        /* Chế độ ID */
        int updated_count = 0;
        int written = snprintf(response, response_size, "📝 Đã cập nhật %d việc:\n", explicit_count);
        task_record_t *old_tasks = malloc(sizeof(task_record_t) * explicit_count);
        int old_cnt = 0;

        for (int i = 0; i < explicit_count; i++) {
            task_record_t task;
            if (task_database_read(explicit_task_ids[i], &task) == ESP_OK) {
                if (old_tasks) old_tasks[old_cnt++] = task;
                bool title_changed = false;
                char changes_buf[128] = ""; int changes_len = 0;

                const char *new_due = json_get_string(fields, "due_time", NULL);
                if (new_due) {
                    if (strcmp(new_due, "none") == 0) { task.due_time = 0; APPEND_SNPRINTF(changes_buf, 128, changes_len, "thời hạn, "); }
                    else { time_t p = time_utils_parse_iso8601(new_due); if(p>0) { task.due_time = p; APPEND_SNPRINTF(changes_buf, 128, changes_len, "thời hạn, "); } }
                }
                const char *new_title = json_get_string(fields, "title", NULL);
                if (new_title && strlen(new_title) > 0) { strncpy(task.title, new_title, 127); title_changed = true; APPEND_SNPRINTF(changes_buf, 128, changes_len, "tiêu đề, "); }
                const char *new_type = json_get_string(fields, "type", NULL);
                if (new_type) { strncpy(task.type, normalize_type(new_type), 15); APPEND_SNPRINTF(changes_buf, 128, changes_len, "loại, "); }

                if (task_database_update(&task) == ESP_OK) {
                    if (title_changed) { float emb[EMBEDDING_DIM]; if(openai_create_embedding(task.title, emb, EMBEDDING_DIM)==ESP_OK) vector_search_save(task.id, emb); }
                    if (written < response_size) { APPEND_SNPRINTF(response, response_size, written, " - [#%d] %s\n", (int)task.id, task.title); }
                    updated_count++;
                }
            }
        }
        if (old_tasks && old_cnt > 0) action_undo_save(UNDO_UPDATE, old_tasks, old_cnt);
        if (old_tasks) free(old_tasks);
    } 
    else if (has_query) {
        /* Chế độ Search */
        float query_embedding[EMBEDDING_DIM];
        if (openai_create_embedding(search_query, query_embedding, EMBEDDING_DIM) == ESP_OK) {
            search_result_t results[SEARCH_TOP_K]; int found = 0;
            vector_search_find_similar(query_embedding, search_query, status_filter_ai, type_filter, results, SEARCH_TOP_K, &found);

            if (found > 1) {
                int offset = snprintf(response, response_size, "❓ Có nhiều kết quả cho \"%s\". Bạn muốn đổi cái nào?\n\n", search_query);
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
                    APPEND_SNPRINTF(response, response_size, offset, "\n👉 Nhập ID (VD: 1, 2) để cập nhật.");
                    
                    cJSON_DeleteItemFromObject(data, "task_ids");
                    cJSON_SetValuestring(cJSON_GetObjectItem(data, "search_query"), "");
                    dispatcher_set_pending_action(ACTION_UPDATE_TASK, cJSON_PrintUnformatted(data));
                    if (ctx_ids) { dispatcher_set_context_tasks(ctx_ids, act); free(ctx_ids); }
                }
                free(matches); cJSON_Delete(data); return ESP_OK;
            } 
            else if (found == 1) {
                task_record_t task;
                if (task_database_read(results[0].task_id, &task) == ESP_OK) {
                    task_record_t old = task;
                    bool title_changed = false;
                    /* char changes_buf[128] = ""; int changes_len = 0; */
                    const char *new_due = json_get_string(fields, "due_time", NULL);
                    if (new_due) { time_t p = time_utils_parse_iso8601(new_due); if(p>0) { task.due_time = p; } }
                    const char *new_title = json_get_string(fields, "title", NULL);
                    if (new_title && strlen(new_title) > 0) { strncpy(task.title, new_title, 127); title_changed = true; }

                    if (task_database_update(&task) == ESP_OK) {
                        if (title_changed) { float emb[EMBEDDING_DIM]; if(openai_create_embedding(task.title, emb, EMBEDDING_DIM)==ESP_OK) vector_search_save(task.id, emb); }
                        action_undo_save(UNDO_UPDATE, &old, 1);
                        snprintf(response, response_size, "📝 Đã sửa task #%d: %s", (int)task.id, task.title);
                        display_show_result("Sua task", task.id, task.title);
                    }
                }
            }
        }
    } 

    if (strlen(response) == 0) format_not_found(search_query, response, response_size);
    cJSON_Delete(data);
    return ESP_OK;
}
