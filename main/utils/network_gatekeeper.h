/**
 * ===========================================================================
 * @file network_gatekeeper.h
 * @brief Quản lý tranh chấp tài nguyên mạng (Sử dụng Mutex)
 * 
 * Giúp tránh việc gọi nhiều API HTTPS cùng lúc gây tràn RAM (Heap) do MbedTLS
 * tốn quá nhiều bộ nhớ cho mỗi handshake.
 * ===========================================================================
 */

#ifndef NETWORK_GATEKEEPER_H
#define NETWORK_GATEKEEPER_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Khởi tạo gatekeeper
 */
void network_gatekeeper_init(void);

/**
 * @brief Thu giữ quyền sử dụng mạng (Lock)
 * Đợi vô thời hạn cho đến khi mạng trống.
 */
void network_lock(void);

/**
 * @brief Thu giữ quyền sử dụng mạng với timeout
 * @param timeout_ms Thời gian đợi tối đa
 * @return true nếu lấy được lock, false nếu hết thời gian
 */
bool network_lock_timeout(uint32_t timeout_ms);

/**
 * @brief Giải phóng quyền sử dụng mạng (Unlock)
 */
void network_unlock(void);

#endif /* NETWORK_GATEKEEPER_H */
