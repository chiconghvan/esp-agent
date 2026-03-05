/**
 * ===========================================================================
 * @file vn_utils.h
 * @brief Tiện ích chuyển đổi tiếng Việt có dấu → không dấu (ASCII)
 * ===========================================================================
 */

#ifndef VN_UTILS_H
#define VN_UTILS_H

#include <stddef.h>

/**
 * @brief Chuyển chuỗi UTF-8 tiếng Việt thành ASCII không dấu
 * @param dst Buffer đích
 * @param src Chuỗi nguồn UTF-8
 * @param dst_size Kích thước buffer đích
 */
void vn_strip_diacritics(char *dst, const char *src, size_t dst_size);

#endif /* VN_UTILS_H */
