/**
 * ===========================================================================
 * @file query_engine.c
 * @brief Bộ máy truy vấn dùng chung (Shared Filter Logic)
 * ===========================================================================
 */

#include "query_engine.h"
#include "task_database.h"
#include "time_utils.h"
#include "json_parser.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * Parse filter operator string
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
 * Resolve thời gian
 * -------------------------------------------------------------------------- */
static time_t resolve_time_value(const char *value)
{
    if (value == NULL) return 0;
    time_t t = time_utils_parse_iso8601(value);
    if (t > 0) return t;
    time_t now = time_utils_get_now();
    if (strstr(value, "now") != NULL) return now;
    return 0;
}

/* --------------------------------------------------------------------------
 * Parse filters từ JSON
 * -------------------------------------------------------------------------- */
int query_engine_parse_filters(cJSON *filters_arr, query_filter_t *out, int max)
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

        if (value != NULL) {
            strncpy(out[count].str_value, value, sizeof(out[count].str_value) - 1);
            bool is_time_field = (strcmp(field, "due_time") == 0 ||
                                  strcmp(field, "start_time") == 0 ||
                                  strcmp(field, "created_at") == 0 ||
                                  strcmp(field, "reminder") == 0);
            if (is_time_field) out[count].time_value = resolve_time_value(value);
        }
        if (value_end != NULL) {
            out[count].time_value_end = resolve_time_value(value_end);
        }
        count++;
    }
    return count;
}

/* --------------------------------------------------------------------------
 * Check 1 filter
 * -------------------------------------------------------------------------- */
static bool match_filter(const task_index_entry_t *entry,
                          const task_record_t *full_record,
                          const query_filter_t *filter)
{
    /* Time fields */
    if (strcmp(filter->field, "due_time") == 0 ||
        strcmp(filter->field, "start_time") == 0 ||
        strcmp(filter->field, "reminder") == 0) {
        
        time_t val = 0;
        if (strcmp(filter->field, "due_time") == 0)   val = entry->due_time;
        else if (strcmp(filter->field, "start_time") == 0) val = entry->start_time;
        else val = entry->reminder;

        switch (filter->op) {
            case FILTER_OP_EQUALS:      return val == filter->time_value;
            case FILTER_OP_NOT_EQUALS:  return val != filter->time_value;
            case FILTER_OP_BEFORE:      return val > 0 && val < filter->time_value;
            case FILTER_OP_AFTER:       return val > 0 && val > filter->time_value;
            case FILTER_OP_BETWEEN:     return val > 0 && val >= filter->time_value && val <= filter->time_value_end;
            case FILTER_OP_IS_NULL:     return val == 0;
            case FILTER_OP_IS_NOT_NULL: return val != 0;
            default: return true;
        }
    }

    /* String fields: status, type */
    if (strcmp(filter->field, "status") == 0 ||
        strcmp(filter->field, "type") == 0) {
        
        const char *val = (strcmp(filter->field, "status") == 0) ? entry->status : entry->type;
        if (val == NULL) return filter->op == FILTER_OP_IS_NULL;

        /* Support "pending" including "overdue" */
        if (strcmp(filter->field, "status") == 0 && strcmp(filter->str_value, "pending") == 0 && filter->op == FILTER_OP_EQUALS) {
            return (strcmp(val, "pending") == 0 || strcmp(val, "overdue") == 0);
        }

        switch (filter->op) {
            case FILTER_OP_EQUALS:      return strcmp(val, filter->str_value) == 0;
            case FILTER_OP_NOT_EQUALS:  return strcmp(val, filter->str_value) != 0;
            case FILTER_OP_CONTAINS:    return strstr(val, filter->str_value) != NULL;
            case FILTER_OP_IS_NOT_NULL: return strlen(val) > 0;
            default: return true;
        }
    }

    /* Repeat field — requires reading full record */
    if (strcmp(filter->field, "repeat") == 0 && full_record != NULL) {
        switch (filter->op) {
            case FILTER_OP_EQUALS:      return strcmp(full_record->repeat, filter->str_value) == 0;
            case FILTER_OP_IS_NULL:     return (strlen(full_record->repeat) == 0 || strcmp(full_record->repeat, "none") == 0);
            default: return true;
        }
    }

    return true;
}

/* --------------------------------------------------------------------------
 * Main logic execution
 * -------------------------------------------------------------------------- */
esp_err_t query_engine_execute(const query_filter_t *filters, int filter_count, 
                                uint32_t *out_ids, int max_ids, int *found_count)
{
    const task_index_t *index = task_database_get_index();
    *found_count = 0;

    bool needs_full = false;
    for (int i = 0; i < filter_count; i++) {
        if (strcmp(filters[i].field, "repeat") == 0 || strcmp(filters[i].field, "created_at") == 0) {
            needs_full = true; break;
        }
    }

    for (int i = 0; i < index->count && *found_count < max_ids; i++) {
        const task_index_entry_t *entry = &index->entries[i];
        task_record_t full_rec;
        task_record_t *full_ptr = NULL;
        if (needs_full) {
            if (task_database_read(entry->id, &full_rec) == ESP_OK) full_ptr = &full_rec;
            else continue;
        }

        bool match = true;
        for (int f = 0; f < filter_count; f++) {
            if (!match_filter(entry, full_ptr, &filters[f])) {
                match = false; break;
            }
        }

        if (match) {
            out_ids[(*found_count)++] = entry->id;
        }
    }
    return ESP_OK;
}
