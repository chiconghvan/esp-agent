/**
 * ===========================================================================
 * @file action_summary.c
 * @brief Handler cho TASK_SUMMARY
 *
 * Thống kê tổng quan: pending, done, cancelled, overdue trong khoảng thời gian.
 * ===========================================================================
 */

#include "action_dispatcher.h"
#include "task_database.h"
#include "json_parser.h"
#include "time_utils.h"
#include "response_formatter.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "action_summary";

esp_err_t action_task_summary(const char *data_json, char *response, size_t response_size)
{
    time_t period_start = 0, period_end = 0;

    /* Parse khoảng thời gian */
    if (data_json != NULL) {
        cJSON *data = json_parse_string(data_json);
        if (data != NULL) {
            const char *start_str = json_get_string(data, "period_start", NULL);
            const char *end_str = json_get_string(data, "period_end", NULL);

            if (start_str) period_start = time_utils_parse_iso8601(start_str);
            if (end_str)   period_end = time_utils_parse_iso8601(end_str);

            cJSON_Delete(data);
        }
    }

    /* Mặc định: đầu tháng → hiện tại */
    time_t now = time_utils_get_now();
    if (period_start == 0) {
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        tm_now.tm_mday = 1;
        tm_now.tm_hour = 0;
        tm_now.tm_min = 0;
        tm_now.tm_sec = 0;
        period_start = mktime(&tm_now);
    }
    if (period_end == 0) {
        period_end = now;
    }

    ESP_LOGI(TAG, "Summary: %ld → %ld", (long)period_start, (long)period_end);

    /* Duyệt index đếm */
    const task_index_t *index = task_database_get_index();
    int count_pending = 0, count_done = 0, count_cancelled = 0, count_overdue = 0;
    int count_total = 0;

    /* Cập nhật trạng thái overdue trước khi đếm */
    task_database_update_overdue();

    for (int i = 0; i < index->count; i++) {
        const task_index_entry_t *entry = &index->entries[i];

        /* Chỉ đếm task có due_time hoặc start_time nằm trong period */
        bool in_period = false;

        if (entry->due_time > 0 &&
            entry->due_time >= period_start &&
            entry->due_time <= period_end) {
            in_period = true;
        }

        if (!in_period && entry->start_time > 0 &&
            entry->start_time >= period_start &&
            entry->start_time <= period_end) {
            in_period = true;
        }

        if (!in_period) continue;

        count_total++;

        if (strcmp(entry->status, "pending") == 0 || strcmp(entry->status, "overdue") == 0) {
            count_pending++; /* Tính gộp chung vào đang chờ để lấy tổng quan */
            /* Kiểm tra quá hạn */
            if (strcmp(entry->status, "overdue") == 0 || (entry->due_time > 0 && entry->due_time < now)) {
                count_overdue++;
            }
        } else if (strcmp(entry->status, "done") == 0) {
            count_done++;
        } else if (strcmp(entry->status, "cancelled") == 0) {
            count_cancelled++;
        }
    }

    /* Format thời gian periode */
    char start_buf[32], end_buf[32];
    time_utils_format_date_short(period_start, start_buf, sizeof(start_buf));
    time_utils_format_date_short(period_end, end_buf, sizeof(end_buf));

    /* Format response */
    int offset = 0;
    offset += snprintf(response + offset, response_size - offset,
        "\xF0\x9F\x93\x88 *Tổng kết %s → %s:*\n\n"
        "\xF0\x9F\x93\x8B Tổng: %d task\n"
        "\xE2\x9C\x85 Hoàn thành: %d\n"
        "\xE2\x8F\xB3 Đang chờ: %d\n"
        "\xE2\x9D\x8C Đã hủy: %d\n",
        start_buf, end_buf,
        count_total,
        count_done,
        count_pending,
        count_cancelled);

    if (count_overdue > 0) {
        offset += snprintf(response + offset, response_size - offset,
            "\xE2\x9A\xA0\xEF\xB8\x8F Quá hạn: %d\n", count_overdue);
    }

    /* Tỷ lệ hoàn thành */
    if (count_total > 0) {
        int pct = (count_done * 100) / count_total;
        offset += snprintf(response + offset, response_size - offset,
            "\n\xF0\x9F\x8E\xAF Tiến độ: %d%%", pct);
    }

    return ESP_OK;
}
