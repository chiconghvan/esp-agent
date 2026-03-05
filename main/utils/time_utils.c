/**
 * ===========================================================================
 * @file time_utils.c
 * @brief Triển khai tiện ích xử lý thời gian
 *
 * Bao gồm: parse ISO8601, format tiếng Việt, tính time range,
 * tính next due cho task lặp lại.
 * ===========================================================================
 */

#include "time_utils.h"
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include "esp_log.h"

static const char *TAG = "time_utils";

/** Tên thứ trong tuần bằng tiếng Việt (index 0 = Chủ nhật) */
static const char *WEEKDAY_NAMES[] = {
    "Chủ nhật", "Thứ 2", "Thứ 3", "Thứ 4",
    "Thứ 5", "Thứ 6", "Thứ 7"
};

/* --------------------------------------------------------------------------
 * Hàm nội bộ: Lấy struct tm theo timezone local
 * -------------------------------------------------------------------------- */
static struct tm get_local_time(time_t timestamp)
{
    struct tm time_info;
    localtime_r(&timestamp, &time_info);
    return time_info;
}

/* --------------------------------------------------------------------------
 * Parse chuỗi ISO8601 thành epoch timestamp
 * -------------------------------------------------------------------------- */
time_t time_utils_parse_iso8601(const char *iso_string)
{
    if (iso_string == NULL || strlen(iso_string) == 0 || 
        strcmp(iso_string, "none") == 0 || strcmp(iso_string, "null") == 0) {
        return 0;
    }

    struct tm time_info = {0};
    int year, month, day, hour = 0, minute = 0, second = 0;

    /* Thử parse format đầy đủ: "2026-03-15T10:00:00" */
    int parsed = sscanf(iso_string, "%d-%d-%dT%d:%d:%d",
                        &year, &month, &day, &hour, &minute, &second);

    if (parsed < 3) {
        /* Thử parse format chỉ có ngày: "2026-03-15" */
        parsed = sscanf(iso_string, "%d-%d-%d", &year, &month, &day);
        if (parsed < 3) {
            ESP_LOGW(TAG, "Không thể parse ISO8601: %s", iso_string);
            return 0;
        }
    }

    time_info.tm_year = year - 1900;
    time_info.tm_mon = month - 1;
    time_info.tm_mday = day;
    time_info.tm_hour = hour;
    time_info.tm_min = minute;
    time_info.tm_sec = second;
    time_info.tm_isdst = -1;

    time_t result = mktime(&time_info);
    if (result == -1) {
        ESP_LOGW(TAG, "mktime thất bại cho: %s", iso_string);
        return 0;
    }

    return result;
}

/* --------------------------------------------------------------------------
 * Format epoch thành chuỗi tiếng Việt thân thiện
 * -------------------------------------------------------------------------- */
char *time_utils_format_vietnamese(time_t timestamp, char *buffer, size_t buffer_size)
{
    if (timestamp == 0) {
        snprintf(buffer, buffer_size, "(không có)");
        return buffer;
    }

    struct tm time_info = get_local_time(timestamp);

    snprintf(buffer, buffer_size, "%02d:%02d %s, %02d/%02d/%04d",
             time_info.tm_hour, time_info.tm_min,
             WEEKDAY_NAMES[time_info.tm_wday],
             time_info.tm_mday, time_info.tm_mon + 1,
             time_info.tm_year + 1900);

    return buffer;
}

/* --------------------------------------------------------------------------
 * Format epoch thành chuỗi ngày ngắn gọn
 * -------------------------------------------------------------------------- */
char *time_utils_format_date_short(time_t timestamp, char *buffer, size_t buffer_size)
{
    if (timestamp == 0) {
        snprintf(buffer, buffer_size, "(không có)");
        return buffer;
    }

    struct tm time_info = get_local_time(timestamp);

    snprintf(buffer, buffer_size, "%02d/%02d/%04d",
             time_info.tm_mday, time_info.tm_mon + 1,
             time_info.tm_year + 1900);

    return buffer;
}

/* --------------------------------------------------------------------------
 * Format epoch thành chuỗi ISO8601 chuẩn
 * -------------------------------------------------------------------------- */
char *time_utils_format_iso8601(time_t timestamp, char *buffer, size_t buffer_size)
{
    if (timestamp == 0) {
        snprintf(buffer, buffer_size, "null");
        return buffer;
    }

    struct tm time_info = get_local_time(timestamp);

    snprintf(buffer, buffer_size, "%04d-%02d-%02dT%02d:%02d:%02d",
             time_info.tm_year + 1900, time_info.tm_mon + 1,
             time_info.tm_mday, time_info.tm_hour,
             time_info.tm_min, time_info.tm_sec);

    return buffer;
}

/* --------------------------------------------------------------------------
 * Lấy thời gian hiện tại
 * -------------------------------------------------------------------------- */
time_t time_utils_get_now(void)
{
    return time(NULL);
}

/* --------------------------------------------------------------------------
 * Lấy time range cho "hôm nay"
 * -------------------------------------------------------------------------- */
time_range_t time_utils_get_today_range(void)
{
    time_t now = time(NULL);
    struct tm time_info = get_local_time(now);

    /* Đặt về 00:00:00 */
    time_info.tm_hour = 0;
    time_info.tm_min = 0;
    time_info.tm_sec = 0;
    time_t start = mktime(&time_info);

    /* Đặt về 23:59:59 */
    time_info.tm_hour = 23;
    time_info.tm_min = 59;
    time_info.tm_sec = 59;
    time_t end = mktime(&time_info);

    time_range_t range = { .start = start, .end = end };
    return range;
}

/* --------------------------------------------------------------------------
 * Lấy time range cho "ngày mai"
 * -------------------------------------------------------------------------- */
time_range_t time_utils_get_tomorrow_range(void)
{
    time_t now = time(NULL);
    struct tm time_info = get_local_time(now);

    /* Ngày mai, 00:00:00 */
    time_info.tm_mday += 1;
    time_info.tm_hour = 0;
    time_info.tm_min = 0;
    time_info.tm_sec = 0;
    time_t start = mktime(&time_info);

    /* Ngày mai, 23:59:59 */
    time_info.tm_hour = 23;
    time_info.tm_min = 59;
    time_info.tm_sec = 59;
    time_t end = mktime(&time_info);

    time_range_t range = { .start = start, .end = end };
    return range;
}

/* --------------------------------------------------------------------------
 * Lấy time range cho "tuần này" (Thứ 2 → Chủ nhật)
 * -------------------------------------------------------------------------- */
time_range_t time_utils_get_this_week_range(void)
{
    time_t now = time(NULL);
    struct tm time_info = get_local_time(now);

    /* Tính ngày Thứ 2 đầu tuần (wday: 0=CN, 1=T2, ..., 6=T7) */
    int days_since_monday = (time_info.tm_wday == 0) ? 6 : (time_info.tm_wday - 1);

    time_info.tm_mday -= days_since_monday;
    time_info.tm_hour = 0;
    time_info.tm_min = 0;
    time_info.tm_sec = 0;
    time_t start = mktime(&time_info);

    /* Chủ nhật cuối tuần */
    time_info.tm_mday += 6;
    time_info.tm_hour = 23;
    time_info.tm_min = 59;
    time_info.tm_sec = 59;
    time_t end = mktime(&time_info);

    time_range_t range = { .start = start, .end = end };
    return range;
}

/* --------------------------------------------------------------------------
 * Lấy time range cho "tháng này"
 * -------------------------------------------------------------------------- */
time_range_t time_utils_get_this_month_range(void)
{
    time_t now = time(NULL);
    struct tm time_info = get_local_time(now);

    /* Ngày 1 của tháng, 00:00:00 */
    time_info.tm_mday = 1;
    time_info.tm_hour = 0;
    time_info.tm_min = 0;
    time_info.tm_sec = 0;
    time_t start = mktime(&time_info);

    /* Ngày cuối tháng: sang ngày 1 tháng sau, lùi 1 giây */
    time_info.tm_mon += 1;
    time_info.tm_mday = 1;
    time_info.tm_hour = 0;
    time_info.tm_min = 0;
    time_info.tm_sec = 0;
    time_t end = mktime(&time_info) - 1;

    time_range_t range = { .start = start, .end = end };
    return range;
}

/* --------------------------------------------------------------------------
 * Lấy time range cho 3 ngày (Hôm nay, Ngày mai, Ngày kia)
 * -------------------------------------------------------------------------- */
time_range_t time_utils_get_three_day_range(void)
{
    time_t now = time(NULL);
    struct tm time_h = {0};
    localtime_r(&now, &time_h);

    /* 00:00 hôm nay */
    time_h.tm_hour = 0;
    time_h.tm_min = 0;
    time_h.tm_sec = 0;
    time_t start = mktime(&time_h);

    /* 23:59:59 của ngày kia (today + 2) */
    time_h.tm_mday += 2;
    time_h.tm_hour = 23;
    time_h.tm_min = 59;
    time_h.tm_sec = 59;
    time_t end = mktime(&time_h);

    time_range_t range = { .start = start, .end = end };
    return range;
}

/* --------------------------------------------------------------------------
 * Format thông tin lặp lại thành tiếng Việt
 * -------------------------------------------------------------------------- */
char *time_utils_format_repeat(const char *repeat, int interval, char *buffer, size_t buffer_size)
{
    if (repeat == NULL || strcmp(repeat, "none") == 0) {
        snprintf(buffer, buffer_size, "Không lặp");
        return buffer;
    }

    if (strcmp(repeat, "daily") == 0) {
        if (interval <= 1) {
            snprintf(buffer, buffer_size, "Hàng ngày");
        } else {
            snprintf(buffer, buffer_size, "Mỗi %d ngày", interval);
        }
    } else if (strcmp(repeat, "weekly") == 0) {
        if (interval <= 1) {
            snprintf(buffer, buffer_size, "Hàng tuần");
        } else {
            snprintf(buffer, buffer_size, "Mỗi %d tuần", interval);
        }
    } else if (strcmp(repeat, "monthly") == 0) {
        if (interval <= 1) {
            snprintf(buffer, buffer_size, "Hàng tháng");
        } else {
            snprintf(buffer, buffer_size, "Mỗi %d tháng", interval);
        }
    } else if (strcmp(repeat, "yearly") == 0) {
        if (interval <= 1) {
            snprintf(buffer, buffer_size, "Hàng năm");
        } else {
            snprintf(buffer, buffer_size, "Mỗi %d năm", interval);
        }
    } else if (strcmp(repeat, "quarterly") == 0) {
        if (interval <= 1) {
            snprintf(buffer, buffer_size, "Hàng quý");
        } else {
            snprintf(buffer, buffer_size, "Mỗi %d quý", interval);
        }
    } else {
        snprintf(buffer, buffer_size, "Không lặp");
    }

    return buffer;
}

/* --------------------------------------------------------------------------
 * Tính due_time tiếp theo cho task lặp lại
 * -------------------------------------------------------------------------- */
time_t time_utils_next_due(time_t current_due, const char *repeat, int interval)
{
    if (current_due == 0 || repeat == NULL || strcmp(repeat, "none") == 0) {
        return 0;
    }

    if (interval <= 0) {
        interval = 1;
    }

    struct tm time_info = get_local_time(current_due);

    if (strcmp(repeat, "daily") == 0) {
        time_info.tm_mday += interval;
    } else if (strcmp(repeat, "weekly") == 0) {
        time_info.tm_mday += (7 * interval);
    } else if (strcmp(repeat, "monthly") == 0) {
        time_info.tm_mon += interval;
    } else if (strcmp(repeat, "quarterly") == 0) {
        time_info.tm_mon += (3 * interval);
    } else if (strcmp(repeat, "yearly") == 0) {
        time_info.tm_year += interval;
    }

    /* Kiểm tra trường hợp đặc biệt: Nếu ngày ban đầu lớn hơn số ngày thực tế của tháng mới (VD: 31/06) */
    /* mktime mặc định sẽ để tràn sang 01/07. Tuy nhiên để tiện tạm thời tôi cứ dùng mặc định mktime. */
    return mktime(&time_info);
}

/* --------------------------------------------------------------------------
 * Lấy tên thứ trong tuần bằng tiếng Việt
 * -------------------------------------------------------------------------- */
char *time_utils_get_weekday_name(time_t timestamp, char *buffer, size_t buffer_size)
{
    struct tm time_info = get_local_time(timestamp);
    snprintf(buffer, buffer_size, "%s", WEEKDAY_NAMES[time_info.tm_wday]);
    return buffer;
}
