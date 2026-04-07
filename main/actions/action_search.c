/**
 * ===========================================================================
 * @file action_search.c
 * @brief Handler cho SEARCH_SEMANTIC
 *
 * Tìm kiếm task theo nội dung/ngữ nghĩa sử dụng embedding vectors.
 * ===========================================================================
 */

#include "action_dispatcher.h"
#include "task_database.h"
#include "vector_search.h"
#include "openai_client.h"
#include "json_parser.h"
#include "response_formatter.h"
#include "config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "action_search";

esp_err_t action_search_semantic(const char *data_json, char *response, size_t response_size)
{
    if (data_json == NULL) {
        format_not_found("(query rỗng)", response, response_size);
        return ESP_FAIL;
    }

    cJSON *data = json_parse_string(data_json);
    if (data == NULL) {
        format_error("Lỗi phân tích dữ liệu", response, response_size);
        return ESP_FAIL;
    }

    const char *search_query = json_get_string(data, "search_query", "");
    const char *status_filter = json_get_string(data, "status_filter", NULL);
    const char *type_filter = json_get_string(data, "type_filter", NULL);

    char query_buf[256];
    strncpy(query_buf, search_query, sizeof(query_buf) - 1);
    query_buf[sizeof(query_buf) - 1] = '\0';

    char status_buf[16] = {0};
    if (status_filter && strlen(status_filter) > 0 && strcmp(status_filter, "null") != 0) {
        strncpy(status_buf, status_filter, sizeof(status_buf) - 1);
    }

    char type_buf[16] = {0};
    if (type_filter && strlen(type_filter) > 0 && strcmp(type_filter, "null") != 0) {
        strncpy(type_buf, type_filter, sizeof(type_buf) - 1);
    }

    cJSON_Delete(data);

    /* Nếu chỉ có type_filter mà không có search_query → query theo type thuần */
    if (strlen(query_buf) == 0 && strlen(type_buf) == 0) {
        format_not_found("(query rỗng)", response, response_size);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Semantic search: query='%s' status=%s type=%s",
             query_buf, status_buf[0] ? status_buf : "any",
             type_buf[0] ? type_buf : "any");

    /* Nếu chỉ có type mà không có search_query → liệt kê theo type */
    if (strlen(query_buf) == 0 && strlen(type_buf) > 0) {
        const task_index_t *index = task_database_get_index();
        const task_index_entry_t **matches = calloc(SEARCH_TOP_K, sizeof(task_index_entry_t *));
        if (matches == NULL) {
            format_error("Không đủ bộ nhớ", response, response_size);
            return ESP_ERR_NO_MEM;
        }

        int task_count = 0;
        for (int i = 0; i < index->count && task_count < SEARCH_TOP_K; i++) {
            if (strcmp(index->entries[i].type, type_buf) == 0) {
                if (strcmp(index->entries[i].status, "cancelled") == 0) continue;
                if (status_buf[0] && strcmp(index->entries[i].status, status_buf) != 0) continue;
                matches[task_count++] = &index->entries[i];
            }
        }

        if (task_count == 0) {
            char label[64];
            snprintf(label, sizeof(label), "loại \"%s\"", type_buf);
            format_not_found(label, response, response_size);
        } else {
            char label[64];
            snprintf(label, sizeof(label), "loại \"%s\"", type_buf);
            format_task_list(matches, task_count, label, response, response_size);
        }
        free(matches);
        return ESP_OK;
    }

    /* Tạo embedding cho search query */
    float query_embedding[EMBEDDING_DIM];
    esp_err_t err = openai_create_embedding(query_buf, query_embedding, EMBEDDING_DIM);
    if (err != ESP_OK) {
        format_error("Không thể tạo embedding cho tìm kiếm", response, response_size);
        return err;
    }

    /* Tìm kiếm vector */
    search_result_t search_results[SEARCH_TOP_K];
    int found_count = 0;

    err = vector_search_find_similar(query_embedding, query_buf,
                                      status_buf[0] ? status_buf : NULL,
                                      type_buf[0] ? type_buf : NULL,
                                      search_results, SEARCH_TOP_K, &found_count);

    if (err != ESP_OK || found_count == 0) {
        format_not_found(query_buf, response, response_size);
        return ESP_OK;
    }

    /* Lấy index để lấy raw pointer */
    const task_index_t *index = task_database_get_index();

    const task_index_entry_t **matches = calloc(SEARCH_TOP_K, sizeof(task_index_entry_t *));
    if (matches == NULL) {
        format_error("Không đủ bộ nhớ", response, response_size);
        return ESP_ERR_NO_MEM;
    }

    int task_count = 0;
    for (int i = 0; i < found_count; i++) {
        uint32_t tid = search_results[i].task_id;
        /* Tìm pointer trong index */
        for (int k = 0; k < index->count; k++) {
            if (index->entries[k].id == tid) {
                matches[task_count++] = &index->entries[k];
                break;
            }
        }
    }

    /* Format response */
    char label[300];
    if (type_buf[0]) {
        snprintf(label, sizeof(label), "tìm kiếm \"%s\" (loại: %s)", query_buf, type_buf);
    } else {
        snprintf(label, sizeof(label), "tìm kiếm \"%s\"", query_buf);
    }
    format_task_list(matches, task_count, label, response, response_size);
    free(matches);

    return ESP_OK;
}
