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

#include "action_dispatcher.h"
#include "task_database.h"
#include "json_parser.h"
#include "time_utils.h"
#include "response_formatter.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "action_query";
#define MAX_RESULTS 50

/* --------------------------------------------------------------------------
 * Parse filter operator string → enum
 * -------------------------------------------------------------------------- */
static filter_op_t parse_op(const char *op_str)
{
    if (op_str == NULL) return FILTER_OP_EQUALS;
    if (strcmp(op_str, "equals") == 0)       return FILTER_OP_EQUALS;
    if (strcmp(op_str, "not_equals") == 0)   return FILTER_OP_NOT_EQUALS;
    if (strcmp(op_str, "before") == 0)       return FILTER_OP_BEFORE;
    if (strcmp(op_str, "after") == 0)        return FILTER_OP_AFTER;
    if (strcmp(op_str, "between") == 0)      return FILTER_OP_BETWEEN;
    if (strcmp(op_str, "contains") == 0)     return FILTER_OP_CONTAINS;
    if (strcmp(op_str, "is_null") == 0)      return FILTER_OP_IS_NULL;
    if (strcmp(op_str, "is_not_null") == 0)  return FILTER_OP_IS_NOT_NULL;
    return FILTER_OP_EQUALS;
}

/* --------------------------------------------------------------------------
 * Lấy giá trị time_t từ field name trong task_index_entry
 * -------------------------------------------------------------------------- */
static time_t get_index_time_field(const task_index_entry_t *entry, const char *field)
{
    if (strcmp(field, "due_time") == 0)   return entry->due_time;
    if (strcmp(field, "start_time") == 0) return entry->start_time;
    if (strcmp(field, "reminder") == 0)   return entry->reminder;
    return 0;
}

/* --------------------------------------------------------------------------
 * Lấy giá trị string từ field name trong task_index_entry
 * -------------------------------------------------------------------------- */
static const char *get_index_str_field(const task_index_entry_t *entry, const char *field)
{
    if (strcmp(field, "status") == 0) return entry->status;
    if (strcmp(field, "type") == 0)   return entry->type;
    return NULL;
}

/* --------------------------------------------------------------------------
 * Kiểm tra 1 entry có khớp 1 filter không
 * -------------------------------------------------------------------------- */
static bool match_filter(const task_index_entry_t *entry,
                          const task_record_t *full_record,
                          const query_filter_t *filter)
{
    /* Time fields: due_time, start_time, reminder, created_at */
    if (strcmp(filter->field, "due_time") == 0 ||
        strcmp(filter->field, "start_time") == 0 ||
        strcmp(filter->field, "reminder") == 0) {

        time_t val = get_index_time_field(entry, filter->field);

        switch (filter->op) {
            case FILTER_OP_EQUALS:      return val == filter->time_value;
            case FILTER_OP_NOT_EQUALS:  return val != filter->time_value;
            case FILTER_OP_BEFORE:      return val > 0 && val < filter->time_value;
            case FILTER_OP_AFTER:       return val > 0 && val > filter->time_value;
            case FILTER_OP_BETWEEN:
                return val > 0 && val >= filter->time_value && val <= filter->time_value_end;
            case FILTER_OP_IS_NULL:     return val == 0;
            case FILTER_OP_IS_NOT_NULL: return val != 0;
            default: return true;
        }
    }

    /* String fields: status, type */
    if (strcmp(filter->field, "status") == 0 ||
        strcmp(filter->field, "type") == 0) {

        const char *val = get_index_str_field(entry, filter->field);
        if (val == NULL) return filter->op == FILTER_OP_IS_NULL;

        /* Xử lý đặc biệt cho status = pending/incomplete để bao gồm cả overdue */
        if (strcmp(filter->field, "status") == 0) {
            bool is_target_pending = (strcmp(filter->str_value, "pending") == 0 || strcmp(filter->str_value, "incomplete") == 0);
            if (is_target_pending && filter->op == FILTER_OP_EQUALS) {
                return (strcmp(val, "pending") == 0 || strcmp(val, "overdue") == 0);
            }
        }

        switch (filter->op) {
            case FILTER_OP_EQUALS:      return strcmp(val, filter->str_value) == 0;
            case FILTER_OP_NOT_EQUALS:  return strcmp(val, filter->str_value) != 0;
            case FILTER_OP_CONTAINS:    return strstr(val, filter->str_value) != NULL;
            case FILTER_OP_IS_NULL:     return strlen(val) == 0;
            case FILTER_OP_IS_NOT_NULL: return strlen(val) > 0;
            default: return true;
        }
    }

    /* Repeat field — cần đọc full record */
    if (strcmp(filter->field, "repeat") == 0 && full_record != NULL) {
        /* "none" hoặc rỗng = không lặp lại = coi như null */
        bool is_repeating = (strlen(full_record->repeat) > 0 &&
                             strcmp(full_record->repeat, "none") != 0);
        switch (filter->op) {
            case FILTER_OP_EQUALS:      return strcmp(full_record->repeat, filter->str_value) == 0;
            case FILTER_OP_NOT_EQUALS:  return strcmp(full_record->repeat, filter->str_value) != 0;
            case FILTER_OP_IS_NULL:     return !is_repeating;
            case FILTER_OP_IS_NOT_NULL: return is_repeating;
            default: return true;
        }
    }

    /* created_at — cần đọc full record */
    if (strcmp(filter->field, "created_at") == 0 && full_record != NULL) {
        time_t val = full_record->created_at;
        switch (filter->op) {
            case FILTER_OP_BEFORE:  return val > 0 && val < filter->time_value;
            case FILTER_OP_AFTER:   return val > 0 && val > filter->time_value;
            case FILTER_OP_BETWEEN:
                return val >= filter->time_value && val <= filter->time_value_end;
            default: return true;
        }
    }

    return true; /* Unknown field → match */
}

/* --------------------------------------------------------------------------
 * Kiểm tra cần đọc full record không (filter trên repeat / created_at)
 * -------------------------------------------------------------------------- */
static bool needs_full_record(const query_filter_t *filters, int count)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(filters[i].field, "repeat") == 0 ||
            strcmp(filters[i].field, "repeat_interval") == 0 ||
            strcmp(filters[i].field, "created_at") == 0) {
            return true;
        }
    }
    return false;
}

/* --------------------------------------------------------------------------
 * Fallback: Resolve thời gian đặc biệt nếu AI trả về placeholder
 * -------------------------------------------------------------------------- */
static time_t resolve_time_value(const char *value)
{
    if (value == NULL) return 0;

    /* Thử parse ISO8601 trước */
    time_t t = time_utils_parse_iso8601(value);
    if (t > 0) return t;

    /* Fallback cho các placeholder phổ biến */
    time_t now = time_utils_get_now();
    if (strstr(value, "now") != NULL) return now;
    if (strstr(value, "today") != NULL) {
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        tm_now.tm_hour = 0; tm_now.tm_min = 0; tm_now.tm_sec = 0;
        return mktime(&tm_now);
    }

    ESP_LOGW(TAG, "Không thể parse time value: '%s'", value);
    return 0;
}

/* --------------------------------------------------------------------------
 * Parse filters từ JSON
 * -------------------------------------------------------------------------- */
static int parse_filters_from_json(cJSON *filters_arr, query_filter_t *out, int max)
{
    int count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, filters_arr) {
        if (count >= max) break;

        const char *field = json_get_string(item, "field", "");
        const char *op_str = json_get_string(item, "op", "equals");
        const char *value = json_get_string(item, "value", NULL);
        const char *value_end = json_get_string(item, "value_end", NULL);

        strncpy(out[count].field, field, sizeof(out[count].field) - 1);
        out[count].op = parse_op(op_str);

        /* Parse value */
        if (value != NULL) {
            strncpy(out[count].str_value, value, sizeof(out[count].str_value) - 1);

            /* Chỉ parse thời gian cho các trường TIME, không parse cho type/status/repeat */
            bool is_time_field = (strcmp(field, "due_time") == 0 ||
                                  strcmp(field, "start_time") == 0 ||
                                  strcmp(field, "created_at") == 0 ||
                                  strcmp(field, "reminder") == 0);
            if (is_time_field) {
                out[count].time_value = resolve_time_value(value);
            }
        }

        /* Parse value_end (cho BETWEEN) - luôn là time field */
        if (value_end != NULL) {
            out[count].time_value_end = resolve_time_value(value_end);
        }

        ESP_LOGI(TAG, "Filter[%d]: field=%s, op=%s, value=%s (t=%ld), value_end=%s (t=%ld)",
                 count, field, op_str,
                 value ? value : "(null)", (long)out[count].time_value,
                 value_end ? value_end : "(null)", (long)out[count].time_value_end);

        count++;
    }
    return count;
}

/* --------------------------------------------------------------------------
 * So sánh hàm cho qsort (sort by due_time dựa trên pointer)
 * -------------------------------------------------------------------------- */
static int cmp_due_asc_ptr(const void *a, const void *b)
{
    const task_index_entry_t *ea = *(const task_index_entry_t **)a;
    const task_index_entry_t *eb = *(const task_index_entry_t **)b;
    if (ea->due_time == eb->due_time) return 0;
    return (ea->due_time < eb->due_time) ? -1 : 1;
}

static int cmp_due_desc_ptr(const void *a, const void *b)
{
    return -cmp_due_asc_ptr(a, b);
}

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

    /* Parse filters */
    query_filter_t filters[MAX_FILTERS];
    memset(filters, 0, sizeof(filters));
    int filter_count = 0;

    cJSON *filters_arr = cJSON_GetObjectItem(data, "filters");
    if (filters_arr != NULL && cJSON_IsArray(filters_arr)) {
        filter_count = parse_filters_from_json(filters_arr, filters, MAX_FILTERS);
    }

    cJSON_Delete(data);

    ESP_LOGI(TAG, "Query: type=%s, label=%s, filters=%d, sort=%s, limit=%d",
             resp_type_buf, label_buf, filter_count, sort_buf, limit);

    /* Cập nhật task quá hạn */
    task_database_update_overdue();

    /* Duyệt index và filter */
    const task_index_t *index = task_database_get_index();
    bool need_full = needs_full_record(filters, filter_count);

    const task_index_entry_t **matches = calloc(index->count, sizeof(task_index_entry_t *));
    if (matches == NULL) {
        snprintf(response, response_size,
            "\xE2\x9A\xA0\xEF\xB8\x8F Không đủ bộ nhớ.");
        return ESP_ERR_NO_MEM;
    }

    int match_count = 0;

    for (int i = 0; i < index->count; i++) {
        const task_index_entry_t *entry = &index->entries[i];

        /* Đọc full record nếu cần kiểm tra từ ngữ */
        task_record_t full_rec;
        task_record_t *full_ptr = NULL;
        if (need_full) {
            if (task_database_read(entry->id, &full_rec) == ESP_OK) {
                full_ptr = &full_rec;
            } else {
                continue; /* Lỗi đọc file thì skip */
            }
        }

        /* Kiểm tra tất cả filters (AND logic) */
        bool match = true;
        for (int f = 0; f < filter_count; f++) {
            if (!match_filter(entry, full_ptr, &filters[f])) {
                match = false;
                break;
            }
        }

        if (match) {
            matches[match_count++] = entry;
        }
    }

    /* Sort Mảng con trỏ */
    if (match_count > 1 && sort_buf[0] != '\0') {
        if (strcmp(sort_buf, "due_time_asc") == 0) {
            qsort(matches, match_count, sizeof(task_index_entry_t *), cmp_due_asc_ptr);
        } else if (strcmp(sort_buf, "due_time_desc") == 0) {
            qsort(matches, match_count, sizeof(task_index_entry_t *), cmp_due_desc_ptr);
        }
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
    return ESP_OK;
}
