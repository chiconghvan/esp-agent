/**
 * ===========================================================================
 * @file vector_search.c
 * @brief Triển khai tìm kiếm semantic bằng cosine similarity
 *
 * Embedding vector lưu dưới dạng binary file: /spiffs/emb_XXXX.bin
 * Định dạng file: task_id (4 bytes) + float32[384] (1536 bytes) = 1540 bytes
 * Tìm kiếm: đọc từng file, tính similarity, giữ top-K.
 * ===========================================================================
 */

#include "vector_search.h"
#include "task_database.h"
#include "config.h"
#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include "esp_log.h"

static const char *TAG = "vector_search";

/** Kích thước 1 embedding file (bytes): 4 + 384*4 = 1540 */
#define EMB_FILE_SIZE (sizeof(uint32_t) + EMBEDDING_DIM * sizeof(float))

/* --------------------------------------------------------------------------
 * Hàm nội bộ: Tạo đường dẫn file embedding
 * -------------------------------------------------------------------------- */
static void get_embedding_filepath(uint32_t task_id, char *path, size_t path_size)
{
    snprintf(path, path_size, "%s/emb_%04" PRIu32 ".bin", EMBEDDING_DIR_PATH, task_id);
}

/* --------------------------------------------------------------------------
 * Tính cosine similarity giữa 2 vectors
 * -------------------------------------------------------------------------- */
float vector_search_cosine_similarity(const float *vec_a, const float *vec_b, int dim)
{
    float dot_product = 0.0f;
    float norm_a = 0.0f;
    float norm_b = 0.0f;

    for (int i = 0; i < dim; i++) {
        dot_product += vec_a[i] * vec_b[i];
        norm_a += vec_a[i] * vec_a[i];
        norm_b += vec_b[i] * vec_b[i];
    }

    /* Tránh chia cho 0 */
    if (norm_a == 0.0f || norm_b == 0.0f) {
        return 0.0f;
    }

    return dot_product / (sqrtf(norm_a) * sqrtf(norm_b));
}

/* --------------------------------------------------------------------------
 * Lưu embedding vector
 * -------------------------------------------------------------------------- */
esp_err_t vector_search_save(uint32_t task_id, const float *embedding)
{
    if (embedding == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char filepath[64];
    get_embedding_filepath(task_id, filepath, sizeof(filepath));

    FILE *file = fopen(filepath, "wb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Không thể mở file để ghi: %s", filepath);
        return ESP_FAIL;
    }

    /* Ghi task_id (4 bytes) */
    fwrite(&task_id, sizeof(uint32_t), 1, file);

    /* Ghi embedding vector (EMBEDDING_DIM * 4 bytes) */
    fwrite(embedding, sizeof(float), EMBEDDING_DIM, file);

    fclose(file);

    ESP_LOGD(TAG, "Đã lưu embedding cho task #%" PRIu32 " (%d chiều)", task_id, EMBEDDING_DIM);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Đọc embedding vector
 * -------------------------------------------------------------------------- */
esp_err_t vector_search_load(uint32_t task_id, float *embedding)
{
    if (embedding == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char filepath[64];
    get_embedding_filepath(task_id, filepath, sizeof(filepath));

    FILE *file = fopen(filepath, "rb");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    /* Đọc task_id (bỏ qua, chỉ verify) */
    uint32_t stored_id;
    fread(&stored_id, sizeof(uint32_t), 1, file);

    /* Đọc embedding vector */
    size_t read = fread(embedding, sizeof(float), EMBEDDING_DIM, file);
    fclose(file);

    if (read != EMBEDDING_DIM) {
        ESP_LOGW(TAG, "Embedding file không đầy đủ: %zu/%d floats", read, EMBEDDING_DIM);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Xóa embedding vector
 * -------------------------------------------------------------------------- */
esp_err_t vector_search_delete(uint32_t task_id)
{
    char filepath[64];
    get_embedding_filepath(task_id, filepath, sizeof(filepath));

    if (remove(filepath) == 0) {
        ESP_LOGD(TAG, "Đã xóa embedding task #%" PRIu32, task_id);
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

/* --------------------------------------------------------------------------
 * Tìm kiếm semantic: top-K tasks gần nhất
 * -------------------------------------------------------------------------- */
esp_err_t vector_search_find_similar(const float *query_embedding,
                                      search_result_t *results, int max_results,
                                      int *found_count)
{
    if (query_embedding == NULL || results == NULL || found_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *found_count = 0;

    /* Khởi tạo kết quả với similarity = -1 (chưa có) */
    for (int i = 0; i < max_results; i++) {
        results[i].task_id = 0;
        results[i].similarity = -1.0f;
    }

    /* Buffer cho 1 embedding vector (đọc từ file) */
    float stored_embedding[EMBEDDING_DIM];

    /* Duyệt qua tất cả tasks trong index */
    const task_index_t *index = task_database_get_index();

    for (int i = 0; i < index->count; i++) {
        uint32_t task_id = index->entries[i].id;

        /* Bỏ qua task đã hủy */
        if (strcmp(index->entries[i].status, "cancelled") == 0) {
            continue;
        }

        /* Đọc embedding của task */
        esp_err_t err = vector_search_load(task_id, stored_embedding);
        if (err != ESP_OK) {
            continue;  /* Không có embedding → bỏ qua */
        }

        /* Tính cosine similarity */
        float similarity = vector_search_cosine_similarity(
            query_embedding, stored_embedding, EMBEDDING_DIM);

        /* Kiểm tra ngưỡng */
        if (similarity < SIMILARITY_THRESHOLD) {
            continue;
        }

        /* Chèn vào kết quả top-K (sorted desc by similarity) */
        for (int j = 0; j < max_results; j++) {
            if (similarity > results[j].similarity) {
                /* Dịch các kết quả phía sau xuống */
                for (int k = max_results - 1; k > j; k--) {
                    results[k] = results[k - 1];
                }
                /* Chèn kết quả mới */
                results[j].task_id = task_id;
                results[j].similarity = similarity;
                break;
            }
        }
    }

    /* Đếm kết quả hợp lệ */
    for (int i = 0; i < max_results; i++) {
        if (results[i].similarity >= SIMILARITY_THRESHOLD) {
            (*found_count)++;
        }
    }

    ESP_LOGI(TAG, "Semantic search: tìm thấy %d kết quả", *found_count);
    for (int i = 0; i < *found_count; i++) {
        ESP_LOGI(TAG, "  [%d] task_id=%lu, similarity=%.4f",
                 i + 1, (unsigned long)results[i].task_id, results[i].similarity);
    }
    return ESP_OK;
}
