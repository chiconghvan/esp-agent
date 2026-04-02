/**
 * ===========================================================================
 * @file query_engine.h
 * @brief Bộ máy truy vấn dùng chung cho Query và Mutation
 * ===========================================================================
 */

#ifndef QUERY_ENGINE_H
#define QUERY_ENGINE_H

#include "action_dispatcher.h"
#include "cJSON.h"

/**
 * @brief Parse filters từ mảng JSON của AI
 * @return Số lượng filter parse được
 */
int query_engine_parse_filters(cJSON *filters_arr, query_filter_t *out, int max);

/**
 * @brief Thực thi truy vấn trên Database và trả về danh sách IDs khớp
 * @param filters Mảng các filter
 * @param filter_count Số lượng filter
 * @param out_ids Mảng chứa IDs kết quả
 * @param max_ids Kích thước mảng IDs
 * @param found_count Số lượng IDs tìm thấy thực tế
 */
esp_err_t query_engine_execute(const query_filter_t *filters, int filter_count, 
                                uint32_t *out_ids, int max_ids, int *found_count);

#endif /* QUERY_ENGINE_H */
