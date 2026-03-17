/**
 * ===========================================================================
 * @file vector_search.h
 * @brief Tìm kiếm ngữ nghĩa bằng embedding vectors (cosine similarity)
 *
 * Module này lưu/đọc embedding vectors dưới dạng file binary trên SPIFFS,
 * và thực hiện tìm kiếm top-K tasks gần nhất bằng cosine similarity.
 * Mỗi vector 384 chiều × 4 bytes = 1536 bytes/task.
 * ===========================================================================
 */

#ifndef VECTOR_SEARCH_H
#define VECTOR_SEARCH_H

#include "esp_err.h"
#include "config.h"
#include <stdint.h>

/**
 * @brief Kết quả tìm kiếm semantic
 */
typedef struct {
    uint32_t task_id;           /**< ID task tìm được */
    float similarity;           /**< Similarity score (0.0 → 1.0) */
} search_result_t;

/**
 * @brief Lưu embedding vector cho một task
 *
 * @param task_id ID task
 * @param embedding Mảng float[EMBEDDING_DIM]
 * @return esp_err_t ESP_OK nếu thành công
 */
esp_err_t vector_search_save(uint32_t task_id, const float *embedding);

/**
 * @brief Đọc embedding vector của một task
 *
 * @param task_id ID task
 * @param embedding Mảng float[EMBEDDING_DIM] để lưu kết quả
 * @return esp_err_t ESP_OK nếu tìm thấy
 */
esp_err_t vector_search_load(uint32_t task_id, float *embedding);

/**
 * @brief Xóa embedding vector của một task
 *
 * @param task_id ID task
 * @return esp_err_t ESP_OK nếu thành công
 */
esp_err_t vector_search_delete(uint32_t task_id);

/**
 * @brief Tìm kiếm semantic: tìm top-K tasks gần nhất
 *
 * Duyệt tất cả embedding files, tính cosine similarity với query vector,
 * trả về top-K kết quả có similarity > threshold.
 *
 * @param query_embedding Vector query float[EMBEDDING_DIM]
 * @param results Mảng search_result_t để lưu kết quả (tối đa SEARCH_TOP_K)
 * @param max_results Kích thước mảng results
 * @param found_count Con trỏ để lưu số kết quả tìm được
 * @return esp_err_t ESP_OK
 */
esp_err_t vector_search_find_similar(const float *query_embedding, const char *query_text,
                                      const char *status_filter,
                                      search_result_t *results, int max_results,
                                      int *found_count);

/**
 * @brief Tính cosine similarity giữa 2 vectors
 *
 * @param vec_a Vector thứ nhất
 * @param vec_b Vector thứ hai
 * @param dim Số chiều
 * @return float Cosine similarity (0.0 → 1.0)
 */
float vector_search_cosine_similarity(const float *vec_a, const float *vec_b, int dim);

/**
 * @brief Kiểm tra và tạo lại các embedding bị thiếu
 *
 * Duyệt qua toàn bộ task trong database, nếu thiếu file embedding
 * sẽ gọi OpenAI API để tạo lại.
 *
 * @return esp_err_t ESP_OK nếu thành công hoặc không có gì để làm
 */
esp_err_t vector_search_audit_and_rebuild(void);

#endif /* VECTOR_SEARCH_H */
