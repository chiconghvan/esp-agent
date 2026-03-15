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
#include <strings.h>
#include <math.h>
#include <dirent.h>
#include "display_manager.h"
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
 * Tính Lexical Score (tỷ lệ từ khóa khớp)
 * -------------------------------------------------------------------------- */
static float calculate_lexical_score(const char *target_text, const char *query_text)
{
    if (!target_text || !query_text || strlen(query_text) == 0) return 0.0f;
    
    char query_copy[256];
    strncpy(query_copy, query_text, sizeof(query_copy) - 1);
    query_copy[sizeof(query_copy) - 1] = '\0';
    
    int total_words = 0;
    int matched_words = 0;
    
    char *saveptr;
    char *token = strtok_r(query_copy, " \t\n\r,.", &saveptr);
    while (token != NULL) {
        /* Bỏ qua các từ khóa quá ngắn (<= 1 ký tự) nếu muốn, nhưng ở đây cứ check hết */
        total_words++;
        if (strcasestr(target_text, token) != NULL) {
            matched_words++;
        }
        token = strtok_r(NULL, " \t\n\r,.", &saveptr);
    }
    
    if (total_words == 0) return 0.0f;
    return (float)matched_words / (float)total_words;
}

/* --------------------------------------------------------------------------
 * Tìm kiếm semantic: top-K tasks gần nhất
 * -------------------------------------------------------------------------- */
esp_err_t vector_search_find_similar(const float *query_embedding, const char *query_text,
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
        float semantic_score = vector_search_cosine_similarity(
            query_embedding, stored_embedding, EMBEDDING_DIM);

        float similarity = semantic_score;

        /* Hybrid Search: Alpha Weighting (Lexical + Semantic) */
        if (query_text != NULL) {
            task_record_t task_data;
            if (task_database_read(task_id, &task_data) == ESP_OK) {
                /* Tạo text tổng hợp từ title và notes để search string */
                char target_text[TASK_TITLE_MAX_LEN + TASK_NOTES_MAX_LEN + 2];
                snprintf(target_text, sizeof(target_text), "%s %s", task_data.title, task_data.notes);
                
                float lexical_score = calculate_lexical_score(target_text, query_text);
                
                /* Alpha Cân bằng động (Adaptive Alpha) */
                float alpha = 0.5f; 
                if (lexical_score >= 0.8f) {
                    alpha = 0.2f; /* Trúng Keyword gần tuyệt đối -> Semantic chỉ chiếm 20% trọng số */
                } else if (lexical_score >= 0.4f) {
                    alpha = 0.4f; /* Trúng một nửa -> Semantic chiếm 40% trọng số */
                }
                
                similarity = (alpha * semantic_score) + ((1.0f - alpha) * lexical_score);
                
                ESP_LOGI(TAG, "Comparing with #%lu [%s]: sem=%.3f, lex=%.3f, alpha=%.2f -> final=%.3f", 
                         (unsigned long)task_id, task_data.title, semantic_score, lexical_score, alpha, similarity);
            }
        }

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
        task_record_t res_task;
        char title_buf[32] = "unknown";
        if (task_database_read(results[i].task_id, &res_task) == ESP_OK) {
            strncpy(title_buf, res_task.title, sizeof(title_buf)-1);
        }
        ESP_LOGI(TAG, "  [%d] task_id=%lu, sim=%.4f, title=[%s]",
                 i + 1, (unsigned long)results[i].task_id, results[i].similarity, title_buf);
    }
    return ESP_OK;
}

#include "openai_client.h"

/* --------------------------------------------------------------------------
 * Audit và Rebuild: Tự động tạo lại các embedding bị thiếu
 * -------------------------------------------------------------------------- */
esp_err_t vector_search_audit_and_rebuild(void)
{
    const task_index_t *index = task_database_get_index();
    if (index->count == 0) return ESP_OK;

    /* Đếm tổng số task thiếu để làm mẫu số cho progress bar */
    int total_missing = 0;
    for (int i = 0; i < index->count; i++) {
        char filepath[64];
        get_embedding_filepath(index->entries[i].id, filepath, sizeof(filepath));
        struct stat st;
        if (stat(filepath, &st) != 0) total_missing++;
    }

    if (total_missing == 0) {
        display_boot_progress(90, "AI Embeddings: OK");
        ESP_LOGI(TAG, "Audit xong: Tất cả embeddings đã đầy đủ.");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Bắt đầu audit embedding cho %d tasks (thiếu %d)...", index->count, total_missing);
    int rebuilt_count = 0;
    char oled_msg[32];

    for (int i = 0; i < index->count; i++) {
        uint32_t task_id = index->entries[i].id;
        char filepath[64];
        get_embedding_filepath(task_id, filepath, sizeof(filepath));

        struct stat st;
        if (stat(filepath, &st) != 0) {
            /* Đọc dữ liệu task để lấy title */
            task_record_t task;
            if (task_database_read(task_id, &task) == ESP_OK) {
                snprintf(oled_msg, sizeof(oled_msg), "Rebuilt Embeding %d/%d", rebuilt_count + 1, total_missing);
                display_boot_progress(85, oled_msg);
                
                ESP_LOGI(TAG, "Đang tạo lại embedding cho task #%" PRIu32 ": %s", task_id, task.title);
                
                float embedding[EMBEDDING_DIM];
                esp_err_t err = openai_create_embedding(task.title, embedding, EMBEDDING_DIM);
                
                if (err == ESP_OK) {
                    if (vector_search_save(task_id, embedding) == ESP_OK) {
                        rebuilt_count++;
                    }
                } else {
                    ESP_LOGE(TAG, "Không thể tạo embedding cho task #%" PRIu32 " (Error: %s)", 
                             task_id, esp_err_to_name(err));
                    if (err != ESP_ERR_TIMEOUT) break; 
                }
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        }
    }

    display_boot_progress(90, "AI Rebuilt Done.");
    ESP_LOGI(TAG, "Audit xong: Đã tạo lại %d/%d embedding.", rebuilt_count, total_missing);
    return ESP_OK;
}
