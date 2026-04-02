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

static const char *TAG = "action_history";

esp_err_t action_view_history(const char *data_json, char *response, size_t response_size)
{
    int limit = 10;
    
    if (data_json != NULL) {
        cJSON *root = cJSON_Parse(data_json);
        if (root) {
            limit = json_get_int(root, "limit", 10);
            cJSON_Delete(root);
        }
    }

    if (limit <= 0 || limit > 50) limit = 10;

    ESP_LOGI(TAG, "Viewing history, limit=%d", limit);

    return task_database_read_history(response, response_size, limit);
}
