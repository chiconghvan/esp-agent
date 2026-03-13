/**
 * ===========================================================================
 * @file time_utils.h
 * @brief Tiện ích xử lý thời gian: parse ISO8601, format tiếng Việt, tính range
 *
 * Module này cung cấp các hàm chuyển đổi giữa epoch timestamp, ISO8601 string,
 * và format hiển thị tiếng Việt thân thiện. Hỗ trợ tính toán time range
 * cho các truy vấn "hôm nay", "ngày mai", "tuần này", "tháng này".
 * ===========================================================================
 */

#ifndef TIME_UTILS_H
#define TIME_UTILS_H

#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Cấu trúc lưu khoảng thời gian (start → end)
 */
typedef struct {
    time_t start;   /**< Thời điểm bắt đầu (epoch seconds) */
    time_t end;     /**< Thời điểm kết thúc (epoch seconds) */
} time_range_t;

/**
 * @brief Parse chuỗi ISO8601 thành epoch timestamp
 *
 * Hỗ trợ format: "2026-03-15T10:00:00" hoặc "2026-03-15"
 *
 * @param iso_string Chuỗi thời gian ISO8601
 * @return time_t Epoch timestamp, hoặc 0 nếu parse thất bại
 */
time_t time_utils_parse_iso8601(const char *iso_string);

/**
 * @brief Format epoch timestamp thành chuỗi tiếng Việt thân thiện
 *
 * Ví dụ: "14:30 Thứ 6, 15/03/2026"
 *
 * @param timestamp Epoch timestamp
 * @param buffer Buffer để ghi kết quả
 * @param buffer_size Kích thước buffer
 * @return char* Con trỏ đến buffer, hoặc "(không có)" nếu timestamp = 0
 */
char *time_utils_format_vietnamese(time_t timestamp, char *buffer, size_t buffer_size);

/**
 * @brief Format epoch timestamp thành chuỗi ngày ngắn gọn
 *
 * Ví dụ: "15/03/2026"
 *
 * @param timestamp Epoch timestamp
 * @param buffer Buffer để ghi kết quả
 * @param buffer_size Kích thước buffer
 * @return char* Con trỏ đến buffer
 */
char *time_utils_format_date_short(time_t timestamp, char *buffer, size_t buffer_size);

/**
 * @brief Format epoch timestamp thành chuỗi ISO8601 chuẩn
 *
 * Ví dụ: "2026-03-15T14:30:00"
 *
 * @param timestamp Epoch timestamp
 * @param buffer Buffer để ghi kết quả
 * @param buffer_size Kích thước buffer
 * @return char* Con trỏ đến buffer
 */
char *time_utils_format_iso8601(time_t timestamp, char *buffer, size_t buffer_size);

/**
 * @brief Lấy time range cho "hôm nay" (00:00 → 23:59:59)
 *
 * @return time_range_t Khoảng thời gian hôm nay
 */
time_range_t time_utils_get_today_range(void);

/**
 * @brief Lấy time range cho "ngày mai" (00:00 → 23:59:59)
 *
 * @return time_range_t Khoảng thời gian ngày mai
 */
time_range_t time_utils_get_tomorrow_range(void);

/**
 * @brief Lấy time range cho "tuần này" (Thứ 2 → Chủ nhật)
 *
 * @return time_range_t Khoảng thời gian tuần này
 */
time_range_t time_utils_get_this_week_range(void);

/**
 * @brief Lấy time range cho "tuần sau" (Thứ 2 → Chủ nhật của tuần kế tiếp)
 *
 * @return time_range_t Khoảng thời gian tuần sau
 */
time_range_t time_utils_get_next_week_range(void);

/**
 * @brief Lấy time range cho "tháng này" (ngày 1 → cuối tháng)
 *
 * @return time_range_t Khoảng thời gian tháng này
 */
time_range_t time_utils_get_this_month_range(void);

/**
 * @brief Lấy time range cho 3 ngày (Hôm nay, Ngày mai, Ngày kia)
 *
 * @return time_range_t Từ 00:00 hôm nay đến 23:59 ngày kia
 */
time_range_t time_utils_get_three_day_range(void);

/**
 * @brief Lấy thời gian hiện tại (epoch)
 *
 * @return time_t Thời gian hiện tại
 */
time_t time_utils_get_now(void);

/**
 * @brief Format thông tin lặp lại thành chuỗi tiếng Việt
 *
 * @param repeat Kiểu lặp ("none", "daily", "weekly", "monthly", "yearly")
 * @param interval Khoảng cách lặp
 * @param buffer Buffer để ghi kết quả
 * @param buffer_size Kích thước buffer
 * @return char* Con trỏ đến buffer
 */
char *time_utils_format_repeat(const char *repeat, int interval, char *buffer, size_t buffer_size);

/**
 * @brief Tính due_time tiếp theo cho task lặp lại
 *
 * @param current_due Due time hiện tại
 * @param repeat Kiểu lặp
 * @param interval Khoảng cách
 * @return time_t Due time tiếp theo
 */
time_t time_utils_next_due(time_t current_due, const char *repeat, int interval);

/**
 * @brief Lấy tên thứ trong tuần bằng tiếng Việt
 *
 * @param timestamp Epoch timestamp
 * @param buffer Buffer để ghi kết quả
 * @param buffer_size Kích thước buffer
 * @return char* "Thứ 2", "Thứ 3", ..., "Chủ nhật"
 */
char *time_utils_get_weekday_name(time_t timestamp, char *buffer, size_t buffer_size);

/**
 * @brief Cập nhật thời gian hệ thống từ chuỗi HTTP Date header
 *
 * Chuỗi có định dạng: "Fri, 06 Mar 2026 08:35:01 GMT"
 *
 * @param date_str Chuỗi thời gian từ HTTP header
 * @return esp_err_t ESP_OK nếu thành công
 */
esp_err_t time_utils_set_time_from_http_date(const char *date_str);

#endif /* TIME_UTILS_H */
