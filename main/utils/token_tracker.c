/**
 * ===========================================================================
 * @file token_tracker.c
 * @brief Triển khai bộ theo dõi token và chi phí API
 *
 * Lưu trữ:
 *   /spiffs/stats/monthly_YYYYMM.json  (thống kê theo tháng)
 *   /spiffs/stats/total.json           (thống kê tổng cộng)
 *
 * Giá API (ước lượng, tháng 3/2026):
 *   gpt-4o-mini:             $0.15/1M input, $0.60/1M output
 *   text-embedding-3-small:  $0.02/1M tokens
 *   Tỷ giá: ~25,500 VND/USD
 * ===========================================================================
 */

#include "token_tracker.h"
#include "json_parser.h"
#include "time_utils.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "cJSON.h"
#include "config.h"
#include "esp_system.h"

static const char *TAG = "token_tracker";

/* Giá API (USD/token) */
#define PRICE_CHAT_INPUT_PER_TOKEN    (0.15 / 1000000.0)
#define PRICE_CHAT_OUTPUT_PER_TOKEN   (0.60 / 1000000.0)
#define PRICE_EMBEDDING_PER_TOKEN     (0.02 / 1000000.0)
#define USD_TO_VND                    25500.0

/* Thống kê tháng hiện tại và tổng cộng */
static token_stats_t s_monthly = {0};
static token_stats_t s_total = {0};
static int s_current_month = 0;  /* YYYYMM */
static bool s_initialized = false;

/* --------------------------------------------------------------------------
 * Helper: Lấy YYYYMM hiện tại
 * -------------------------------------------------------------------------- */
static int get_current_yyyymm(void)
{
    time_t now = time_utils_get_now();
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    return (tm_info.tm_year + 1900) * 100 + (tm_info.tm_mon + 1);
}

/* --------------------------------------------------------------------------
 * Helper: Đường dẫn file thống kê theo tháng
 * -------------------------------------------------------------------------- */
static void monthly_filepath(int yyyymm, char *path, size_t path_size)
{
    snprintf(path, path_size, "/spiffs/stats/m_%d.json", yyyymm);
}

/* --------------------------------------------------------------------------
 * Lưu thống kê vào file
 * -------------------------------------------------------------------------- */
static void save_stats(const char *filepath, const token_stats_t *stats)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return;

    cJSON_AddNumberToObject(root, "chat_prompt", stats->chat_prompt_tokens);
    cJSON_AddNumberToObject(root, "chat_completion", stats->chat_completion_tokens);
    cJSON_AddNumberToObject(root, "embedding", stats->embedding_tokens);
    cJSON_AddNumberToObject(root, "chat_calls", stats->chat_calls);
    cJSON_AddNumberToObject(root, "emb_calls", stats->embedding_calls);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str == NULL) return;

    FILE *f = fopen(filepath, "w");
    if (f != NULL) {
        fputs(json_str, f);
        fclose(f);
    }
    free(json_str);
}

/* --------------------------------------------------------------------------
 * Load thống kê từ file
 * -------------------------------------------------------------------------- */
static void load_stats(const char *filepath, token_stats_t *stats)
{
    memset(stats, 0, sizeof(token_stats_t));

    FILE *f = fopen(filepath, "r");
    if (f == NULL) return;

    char buf[256];
    size_t len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = '\0';

    cJSON *root = json_parse_string(buf);
    if (root == NULL) return;

    stats->chat_prompt_tokens = (uint32_t)json_get_int(root, "chat_prompt", 0);
    stats->chat_completion_tokens = (uint32_t)json_get_int(root, "chat_completion", 0);
    stats->embedding_tokens = (uint32_t)json_get_int(root, "embedding", 0);
    stats->chat_calls = (uint32_t)json_get_int(root, "chat_calls", 0);
    stats->embedding_calls = (uint32_t)json_get_int(root, "emb_calls", 0);

    cJSON_Delete(root);
}

/* --------------------------------------------------------------------------
 * Khởi tạo
 * -------------------------------------------------------------------------- */
esp_err_t token_tracker_init(void)
{
    /* Tạo thư mục stats nếu chưa có */
    /* SPIFFS không hỗ trợ mkdir, file sẽ tự được tạo */

    s_current_month = get_current_yyyymm();

    /* Load thống kê tổng */
    load_stats("/spiffs/stats/total.json", &s_total);

    /* Load thống kê tháng hiện tại */
    char path[48];
    monthly_filepath(s_current_month, path, sizeof(path));
    load_stats(path, &s_monthly);

    s_initialized = true;
    ESP_LOGI(TAG, "Token tracker init: tháng=%d, total_calls=%lu+%lu",
             s_current_month,
             (unsigned long)s_total.chat_calls,
             (unsigned long)s_total.embedding_calls);

    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Ghi nhận token usage
 * -------------------------------------------------------------------------- */
void token_tracker_add(token_type_t type, uint32_t prompt_tokens, uint32_t completion_tokens)
{
    if (!s_initialized) return;

    /* Kiểm tra chuyển tháng */
    int current = get_current_yyyymm();
    if (current != s_current_month) {
        /* Lưu tháng cũ */
        char path[48];
        monthly_filepath(s_current_month, path, sizeof(path));
        save_stats(path, &s_monthly);

        /* Reset tháng mới */
        s_current_month = current;
        memset(&s_monthly, 0, sizeof(token_stats_t));
        ESP_LOGI(TAG, "Chuyển sang tháng mới: %d", current);
    }

    if (type == TOKEN_TYPE_CHAT) {
        s_monthly.chat_prompt_tokens += prompt_tokens;
        s_monthly.chat_completion_tokens += completion_tokens;
        s_monthly.chat_calls++;

        s_total.chat_prompt_tokens += prompt_tokens;
        s_total.chat_completion_tokens += completion_tokens;
        s_total.chat_calls++;
    } else {
        s_monthly.embedding_tokens += prompt_tokens;
        s_monthly.embedding_calls++;

        s_total.embedding_tokens += prompt_tokens;
        s_total.embedding_calls++;
    }

    /* Lưu cả hai */
    char path[48];
    monthly_filepath(s_current_month, path, sizeof(path));
    save_stats(path, &s_monthly);
    save_stats("/spiffs/stats/total.json", &s_total);
}

/* --------------------------------------------------------------------------
 * Lấy thống kê
 * -------------------------------------------------------------------------- */
token_stats_t token_tracker_get_monthly(void) { return s_monthly; }
token_stats_t token_tracker_get_total(void) { return s_total; }

/* --------------------------------------------------------------------------
 * Ước lượng chi phí VND
 * -------------------------------------------------------------------------- */
double token_tracker_estimate_cost_vnd(const token_stats_t *stats)
{
    double cost_usd =
        (stats->chat_prompt_tokens * PRICE_CHAT_INPUT_PER_TOKEN) +
        (stats->chat_completion_tokens * PRICE_CHAT_OUTPUT_PER_TOKEN) +
        (stats->embedding_tokens * PRICE_EMBEDDING_PER_TOKEN);

    return cost_usd * USD_TO_VND;
}

/* --------------------------------------------------------------------------
 * Format thống kê cho Telegram
 * -------------------------------------------------------------------------- */
void token_tracker_format_status(char *buffer, size_t buffer_size)
{
    token_stats_t monthly = token_tracker_get_monthly();
    token_stats_t total = token_tracker_get_total();

    double monthly_cost = token_tracker_estimate_cost_vnd(&monthly);
    double total_cost = token_tracker_estimate_cost_vnd(&total);

    uint32_t monthly_total_tokens = monthly.chat_prompt_tokens +
                                     monthly.chat_completion_tokens +
                                     monthly.embedding_tokens;
    uint32_t total_total_tokens = total.chat_prompt_tokens +
                                   total.chat_completion_tokens +
                                   total.embedding_tokens;

    int month = s_current_month % 100;
    int year = s_current_month / 100;

    snprintf(buffer, buffer_size,
        "\xF0\x9F\x93\x8A Thống kê sử dụng API\n"
        "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
        "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
        "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\n"
        "\xF0\x9F\x93\x85 Tháng %d/%d:\n"
        "  \xF0\x9F\x92\xAC Chat: %lu lần (%lu tokens)\n"
        "  \xF0\x9F\x94\x8D Embedding: %lu lần (%lu tokens)\n"
        "  \xF0\x9F\x93\x88 Tổng: %lu tokens\n"
        "  \xF0\x9F\x92\xB0 Chi phí: ~%.0f VND\n"
        "\n"
        "\xF0\x9F\x93\x8A Tổng cộng (all-time):\n"
        "  \xF0\x9F\x92\xAC Chat: %lu lần (%lu tokens)\n"
        "  \xF0\x9F\x94\x8D Embedding: %lu lần (%lu tokens)\n"
        "  \xF0\x9F\x93\x88 Tổng: %lu tokens\n"
        "  \xF0\x9F\x92\xB0 Chi phí: ~%.0f VND\n"
        "\n"
        "\xF0\x9F\x96\xA5\xEF\xB8\x8F Free heap: %lu bytes\n"
        "\xE2\x9A\x99\xEF\xB8\x8F Model: %s",
        month, year,
        (unsigned long)monthly.chat_calls,
        (unsigned long)(monthly.chat_prompt_tokens + monthly.chat_completion_tokens),
        (unsigned long)monthly.embedding_calls,
        (unsigned long)monthly.embedding_tokens,
        (unsigned long)monthly_total_tokens,
        monthly_cost,
        (unsigned long)total.chat_calls,
        (unsigned long)(total.chat_prompt_tokens + total.chat_completion_tokens),
        (unsigned long)total.embedding_calls,
        (unsigned long)total.embedding_tokens,
        (unsigned long)total_total_tokens,
        total_cost,
        (unsigned long)esp_get_free_heap_size(),
        OPENAI_MODEL);
}
