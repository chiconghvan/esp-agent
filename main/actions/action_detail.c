/**
 * ===========================================================================
 * @file action_detail.c
 * @brief Handler cho GET_TASK_DETAIL
 *
 * Lấy chi tiết 1 hoặc nhiều task theo ID hoặc semantic search.
 * Cover cả use-case hỏi thời gian (GET_TASK_TIME).
 * ===========================================================================
 */

#include "safe_append.h"
#include "action_dispatcher.h"
#include "task_database.h"
#include "vector_search.h"
#include "openai_client.h"
#include "json_parser.h"
#include "time_utils.h"
#include "response_formatter.h"
#include "config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "action_detail";

esp_err_t action_get_detail(const char *data_json, char *response, size_t response_size)
{
    if (data_json == NULL) {
        format_error("Thiếu thông tin truy vấn", response, response_size);
        return ESP_FAIL;
    }

    cJSON *data = json_parse_string(data_json);
    if (data == NULL) {
        format_error("Lỗi phân tích dữ liệu", response, response_size);
        return ESP_FAIL;
    }

    /* Parse task_ids */
    cJSON *ids_arr = cJSON_GetObjectItem(data, "task_ids");
    const char *search_query = json_get_string(data, "search_query", NULL);

    /* Copy search_query trước khi delete cJSON */
    char query_buf[256] = {0};
    if (search_query && strlen(search_query) > 0) {
        strncpy(query_buf, search_query, sizeof(query_buf) - 1);
    }

    /* Lấy task IDs */
    uint32_t task_ids[5];
    int id_count = 0;

    if (ids_arr != NULL && cJSON_IsArray(ids_arr)) {
        cJSON *item;
        cJSON_ArrayForEach(item, ids_arr) {
            if (id_count >= 5) break;
            if (cJSON_IsNumber(item)) {
                task_ids[id_count++] = (uint32_t)item->valuedouble;
            }
        }
    }

    cJSON_Delete(data);

    /* Nếu không có ID → dùng semantic search */
    if (id_count == 0 && strlen(query_buf) > 0) {
        ESP_LOGI(TAG, "Semantic search cho detail: %s", query_buf);

        float query_embedding[EMBEDDING_DIM];
        esp_err_t err = openai_create_embedding(query_buf, query_embedding, EMBEDDING_DIM);
        if (err != ESP_OK) {
            format_error("Không thể tìm kiếm", response, response_size);
            return err;
        }

        search_result_t search_results[3];
        int found = 0;
        err = vector_search_find_similar(query_embedding, query_buf, NULL, search_results, 3, &found);
        if (err != ESP_OK || found == 0) {
            format_not_found(query_buf, response, response_size);
            return ESP_OK;
        }

        /* Lấy task ID có điểm cao nhất */
        for (int i = 0; i < found && id_count < 5; i++) {
            task_ids[id_count++] = search_results[i].task_id;
        }
    }

    if (id_count == 0) {
        format_not_found("(không xác định task)", response, response_size);
        return ESP_OK;
    }

    /* Đọc và format chi tiết */
    int offset = 0;

    for (int i = 0; i < id_count; i++) {
        task_record_t task;
        if (task_database_read(task_ids[i], &task) != ESP_OK) {
            APPEND_SNPRINTF(response, response_size, offset,
                "\xE2\x9A\xA0\xEF\xB8\x8F Không tìm thấy task #%lu\n\n",
                (unsigned long)task_ids[i]);
            continue;
        }

        if (i > 0) APPEND_SNPRINTF(response, response_size, offset, "\n");
        
        char detail_buf[800];
        format_task_detail_full(&task, detail_buf, sizeof(detail_buf));
        APPEND_SNPRINTF(response, response_size, offset, "%s", detail_buf);
    }

    return ESP_OK;
}
