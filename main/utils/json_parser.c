/**
 * ===========================================================================
 * @file json_parser.c
 * @brief Triển khai wrapper an toàn cho cJSON
 * ===========================================================================
 */

#include "json_parser.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"

static const char *TAG = "json_parser";

/* --------------------------------------------------------------------------
 * Đọc string an toàn từ JSON
 * -------------------------------------------------------------------------- */
const char *json_get_string(const cJSON *json, const char *key, const char *default_value)
{
    if (json == NULL || key == NULL) {
        return default_value;
    }

    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (item == NULL || !cJSON_IsString(item) || item->valuestring == NULL) {
        return default_value;
    }

    return item->valuestring;
}

/* --------------------------------------------------------------------------
 * Đọc integer an toàn từ JSON
 * -------------------------------------------------------------------------- */
int64_t json_get_int(const cJSON *json, const char *key, int64_t default_value)
{
    if (json == NULL || key == NULL) {
        return default_value;
    }

    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (item == NULL || !cJSON_IsNumber(item)) {
        return default_value;
    }

    return (int64_t)item->valuedouble;
}

/* --------------------------------------------------------------------------
 * Đọc double an toàn từ JSON
 * -------------------------------------------------------------------------- */
double json_get_double(const cJSON *json, const char *key, double default_value)
{
    if (json == NULL || key == NULL) {
        return default_value;
    }

    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (item == NULL || !cJSON_IsNumber(item)) {
        return default_value;
    }

    return item->valuedouble;
}

/* --------------------------------------------------------------------------
 * Đọc boolean an toàn từ JSON
 * -------------------------------------------------------------------------- */
bool json_get_bool(const cJSON *json, const char *key, bool default_value)
{
    if (json == NULL || key == NULL) {
        return default_value;
    }

    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (item == NULL) {
        return default_value;
    }

    if (cJSON_IsTrue(item)) return true;
    if (cJSON_IsFalse(item)) return false;

    return default_value;
}

/* --------------------------------------------------------------------------
 * Lấy object con an toàn
 * -------------------------------------------------------------------------- */
cJSON *json_get_object(const cJSON *json, const char *key)
{
    if (json == NULL || key == NULL) {
        return NULL;
    }

    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (item == NULL || !cJSON_IsObject(item)) {
        return NULL;
    }

    return item;
}

/* --------------------------------------------------------------------------
 * Lấy array an toàn
 * -------------------------------------------------------------------------- */
cJSON *json_get_array(const cJSON *json, const char *key)
{
    if (json == NULL || key == NULL) {
        return NULL;
    }

    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (item == NULL || !cJSON_IsArray(item)) {
        return NULL;
    }

    return item;
}

/* --------------------------------------------------------------------------
 * Parse chuỗi JSON
 * -------------------------------------------------------------------------- */
cJSON *json_parse_string(const char *json_string)
{
    if (json_string == NULL || strlen(json_string) == 0) {
        ESP_LOGW(TAG, "JSON string rỗng");
        return NULL;
    }

    cJSON *parsed = cJSON_Parse(json_string);
    if (parsed == NULL) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            ESP_LOGE(TAG, "Lỗi parse JSON tại: %.20s", error_ptr);
        }
        return NULL;
    }

    return parsed;
}

/* --------------------------------------------------------------------------
 * Trích xuất JSON từ trong text (tìm cặp { } ngoài cùng)
 * -------------------------------------------------------------------------- */
cJSON *json_extract_from_text(const char *text)
{
    if (text == NULL) {
        return NULL;
    }

    /* Tìm ký tự '{' đầu tiên */
    const char *start = strchr(text, '{');
    if (start == NULL) {
        ESP_LOGW(TAG, "Không tìm thấy '{' trong text");
        return NULL;
    }

    /* Tìm ký tự '}' cuối cùng */
    const char *end = strrchr(text, '}');
    if (end == NULL || end <= start) {
        ESP_LOGW(TAG, "Không tìm thấy '}' hợp lệ trong text");
        return NULL;
    }

    /* Trích xuất chuỗi JSON */
    size_t json_len = (end - start) + 1;
    char *json_str = malloc(json_len + 1);
    if (json_str == NULL) {
        ESP_LOGE(TAG, "Không đủ RAM để trích xuất JSON");
        return NULL;
    }

    memcpy(json_str, start, json_len);
    json_str[json_len] = '\0';

    /* Parse JSON */
    cJSON *result = cJSON_Parse(json_str);
    free(json_str);

    if (result == NULL) {
        ESP_LOGW(TAG, "JSON trích xuất không hợp lệ");
    }

    return result;
}
