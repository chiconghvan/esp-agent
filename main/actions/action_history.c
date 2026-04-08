/**
 * ===========================================================================
 * @file action_history.c
 * @brief Handler cho action VIEW_HISTORY
 *
 * Đọc lịch sử từ file history.txt và hiển thị lên Telegram.
 * ===========================================================================
 */

#include "action_dispatcher.h"
#include "task_database.h"
#include "json_parser.h"
#include "response_formatter.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>

#include "time_utils.h"

static const char *TAG = "action_history";

esp_err_t action_view_history(const char *data_json, char *response, size_t response_size)
{
    int limit = 0; // 0 means show all if period is set
    time_t start = 0, end = 0;
    
    if (data_json != NULL) {
        cJSON *root = cJSON_Parse(data_json);
        if (root) {
            cJSON *limit_item = cJSON_GetObjectItem(root, "limit");
            if (limit_item && cJSON_IsNumber(limit_item)) {
                limit = (int)limit_item->valuedouble;
            } else if (limit_item && cJSON_IsNull(limit_item)) {
                limit = 0; // null means no limit
            } else {
                limit = 10; // default
            }

            const char *start_str = json_get_string(root, "period_start", NULL);
            const char *end_str = json_get_string(root, "period_end", NULL);

            if (start_str) start = time_utils_parse_iso8601(start_str);
            if (end_str) end = time_utils_parse_iso8601(end_str);

            cJSON_Delete(root);
        }
    }

    if (start == 0 && end == 0 && limit <= 0) limit = 10;

    ESP_LOGI(TAG, "Viewing history, limit=%d, start=%ld, end=%ld", limit, (long)start, (long)end);

    return task_database_read_history(response, response_size, limit, start, end);
}
