/**
 * ===========================================================================
 * @file action_query.c
 * @brief Unified Query Engine — xử lý QUERY_TASKS
 *
 * Nhận mảng filters[] từ B2, duyệt index local, trả kết quả.
 * Hỗ trợ: list, count, sort, limit.
 * Không gọi thêm AI.
 * ===========================================================================
 */

#include "openai_client.h"
#include "cJSON.h"
#include "query_engine.h"
#include "task_database.h"
#include "json_parser.h"
#include "response_formatter.h"
#include <string.h>
#include <stdlib.h>
#define MAX_RESULTS 50

/* Sorting functions (pointers) */
static int cmp_due_asc_ptr(const void *a, const void *b)
{
    const task_index_entry_t *ea = *(const task_index_entry_t **)a;
    const task_index_entry_t *eb = *(const task_index_entry_t **)b;
    if (ea->due_time == eb->due_time) return 0;
    return (ea->due_time < eb->due_time) ? -1 : 1;
}
static int cmp_due_desc_ptr(const void *a, const void *b) { return -cmp_due_asc_ptr(a, b); }

/* ==========================================================================
 * Handler chính
 * ========================================================================== */
esp_err_t action_query_tasks(const char *data_json, char *response, size_t response_size)
{
    if (data_json == NULL) {
        snprintf(response, response_size,
            "\xE2\x9A\xA0\xEF\xB8\x8F Không thể phân tích câu hỏi.");
        return ESP_FAIL;
    }

    cJSON *data = json_parse_string(data_json);
    if (data == NULL) {
        snprintf(response, response_size,
            "\xE2\x9A\xA0\xEF\xB8\x8F Lỗi phân tích dữ liệu.");
        return ESP_FAIL;
    }

    /* Parse query request */
    const char *response_type = json_get_string(data, "response_type", "list");
    const char *label = json_get_string(data, "label", "truy vấn");
    const char *sort_str = json_get_string(data, "sort", NULL);
    int limit = (int)json_get_double(data, "limit", 0);

    char label_buf[256];
    strncpy(label_buf, label, sizeof(label_buf) - 1);
    label_buf[sizeof(label_buf) - 1] = '\0';

    char resp_type_buf[16];
    strncpy(resp_type_buf, response_type, sizeof(resp_type_buf) - 1);
    resp_type_buf[sizeof(resp_type_buf) - 1] = '\0';

    char sort_buf[32] = {0};
    if (sort_str) {
        strncpy(sort_buf, sort_str, sizeof(sort_buf) - 1);
    }

    /* 1. Parse filters */
    query_filter_t filters[MAX_FILTERS];
    int filter_count = 0;
    cJSON *filters_arr = cJSON_GetObjectItem(data, "filters");
    if (filters_arr) filter_count = query_engine_parse_filters(filters_arr, filters, MAX_FILTERS);

    /* 2. Execute query */
    uint32_t *target_ids = malloc(sizeof(uint32_t) * MAX_TASK_COUNT);
    int match_count = 0;
    query_engine_execute(filters, filter_count, target_ids, MAX_TASK_COUNT, &match_count);

    /* 3. Get index pointers for sorting */
    const task_index_entry_t **matches = calloc(match_count, sizeof(task_index_entry_t *));
    const task_index_t *index = task_database_get_index();
    
    int actual_matches = 0;
    for (int i = 0; i < match_count; i++) {
        for (int j = 0; j < index->count; j++) {
            if (index->entries[j].id == target_ids[i]) {
                matches[actual_matches++] = &index->entries[j];
                break;
            }
        }
    }
    match_count = actual_matches;

    /* 4. Sort results */
    if (match_count > 1 && sort_buf[0] != '\0') {
        if (strcmp(sort_buf, "due_time_asc") == 0) qsort(matches, match_count, sizeof(task_index_entry_t *), cmp_due_asc_ptr);
        else if (strcmp(sort_buf, "due_time_desc") == 0) qsort(matches, match_count, sizeof(task_index_entry_t *), cmp_due_desc_ptr);
    }

    /* Limit */
    if (limit > 0 && match_count > limit) {
        match_count = limit;
    }

    /* Format response theo response_type */
    if (strcmp(resp_type_buf, "count") == 0) {
        format_task_count(match_count, label_buf, response, response_size);
    } else {
        if (strcmp(resp_type_buf, "short") == 0) {
            format_task_list_short(matches, match_count, label_buf, response, response_size);
        } else {
            format_task_list(matches, match_count, label_buf, response, response_size);
        }
    }

    free(matches);
    free(target_ids);
    cJSON_Delete(data);
    return ESP_OK;
}
