/**
 * ===========================================================================
 * @file token_tracker.h
 * @brief Theo dõi số lượng token đã sử dụng và ước lượng chi phí API
 *
 * Lưu trữ thống kê theo tháng trong SPIFFS.
 * Tính chi phí ước lượng theo VND.
 * ===========================================================================
 */

#ifndef TOKEN_TRACKER_H
#define TOKEN_TRACKER_H

#include "esp_err.h"
#include <stdint.h>

/**
 * @brief Loại API call
 */
typedef enum {
    TOKEN_TYPE_CHAT = 0,      /* Chat Completion (gpt-4o-mini) */
    TOKEN_TYPE_EMBEDDING      /* Embedding (text-embedding-3-small) */
} token_type_t;

/**
 * @brief Thống kê token cho một tháng
 */
typedef struct {
    uint32_t chat_prompt_tokens;     /* Tổng prompt tokens (chat) */
    uint32_t chat_completion_tokens; /* Tổng completion tokens (chat) */
    uint32_t embedding_tokens;       /* Tổng embedding tokens */
    uint32_t chat_calls;             /* Số lần gọi chat */
    uint32_t embedding_calls;        /* Số lần gọi embedding */
} token_stats_t;

/**
 * @brief Khởi tạo token tracker, load dữ liệu từ SPIFFS
 */
esp_err_t token_tracker_init(void);

/**
 * @brief Ghi nhận token usage cho một lần gọi API
 *
 * @param type Loại API
 * @param prompt_tokens Số prompt tokens (hoặc input tokens cho embedding)
 * @param completion_tokens Số completion tokens (0 cho embedding)
 */
void token_tracker_add(token_type_t type, uint32_t prompt_tokens, uint32_t completion_tokens);

/**
 * @brief Lấy thống kê tháng hiện tại
 */
token_stats_t token_tracker_get_monthly(void);

/**
 * @brief Lấy thống kê tổng cộng (all-time)
 */
token_stats_t token_tracker_get_total(void);

/**
 * @brief Ước lượng chi phí VND từ thống kê
 *
 * @param stats Thống kê token
 * @return Chi phí ước lượng (VND)
 */
double token_tracker_estimate_cost_vnd(const token_stats_t *stats);

/**
 * @brief Format thống kê thành chuỗi gửi Telegram
 *
 * @param buffer Buffer output
 * @param buffer_size Kích thước buffer
 */
void token_tracker_format_status(char *buffer, size_t buffer_size);

#endif /* TOKEN_TRACKER_H */
