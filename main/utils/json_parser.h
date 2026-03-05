/**
 * ===========================================================================
 * @file json_parser.h
 * @brief Wrapper an toàn cho thư viện cJSON
 *
 * Cung cấp các hàm helper để parse và build JSON một cách an toàn,
 * tránh NULL pointer dereference và memory leak.
 * ===========================================================================
 */

#ifndef JSON_PARSER_H
#define JSON_PARSER_H

#include "cJSON.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Đọc giá trị string từ JSON object một cách an toàn
 *
 * @param json JSON object
 * @param key Tên field
 * @param default_value Giá trị mặc định nếu field không tồn tại
 * @return const char* Giá trị string, hoặc default_value
 */
const char *json_get_string(const cJSON *json, const char *key, const char *default_value);

/**
 * @brief Đọc giá trị integer từ JSON object một cách an toàn
 *
 * @param json JSON object
 * @param key Tên field
 * @param default_value Giá trị mặc định nếu field không tồn tại
 * @return int64_t Giá trị integer
 */
int64_t json_get_int(const cJSON *json, const char *key, int64_t default_value);

/**
 * @brief Đọc giá trị double từ JSON object một cách an toàn
 *
 * @param json JSON object
 * @param key Tên field
 * @param default_value Giá trị mặc định nếu field không tồn tại
 * @return double Giá trị double
 */
double json_get_double(const cJSON *json, const char *key, double default_value);

/**
 * @brief Đọc giá trị boolean từ JSON object một cách an toàn
 *
 * @param json JSON object
 * @param key Tên field
 * @param default_value Giá trị mặc định nếu field không tồn tại
 * @return bool Giá trị boolean
 */
bool json_get_bool(const cJSON *json, const char *key, bool default_value);

/**
 * @brief Lấy JSON object con một cách an toàn
 *
 * @param json JSON object cha
 * @param key Tên field
 * @return cJSON* Con trỏ đến object con, hoặc NULL
 */
cJSON *json_get_object(const cJSON *json, const char *key);

/**
 * @brief Lấy JSON array một cách an toàn
 *
 * @param json JSON object cha
 * @param key Tên field
 * @return cJSON* Con trỏ đến array, hoặc NULL
 */
cJSON *json_get_array(const cJSON *json, const char *key);

/**
 * @brief Parse chuỗi JSON, trả về cJSON object
 *
 * @param json_string Chuỗi JSON
 * @return cJSON* Parsed object (cần gọi cJSON_Delete khi xong), hoặc NULL
 */
cJSON *json_parse_string(const char *json_string);

/**
 * @brief Tìm và trích xuất JSON object từ trong chuỗi text
 *
 * Hữu ích khi LLM trả về JSON lẫn trong text giải thích.
 * Tìm cặp { } ngoài cùng và parse.
 *
 * @param text Chuỗi text chứa JSON
 * @return cJSON* Parsed object, hoặc NULL
 */
cJSON *json_extract_from_text(const char *text);

#endif /* JSON_PARSER_H */
