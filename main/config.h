/**
 * ===========================================================================
 * @file config.h
 * @brief Cấu hình người dùng cho ESP-Agent
 *
 * File này chứa tất cả các thông số cấu hình mà người dùng cần thay đổi
 * trước khi build và flash firmware. Bao gồm: WiFi, Telegram Bot,
 * OpenAI API, timezone, database, và các tham số hệ thống.
 *
 * @note QUAN TRỌNG: Hãy điền đầy đủ thông tin trước khi build!
 * ===========================================================================
 */

#ifndef CONFIG_H
#define CONFIG_H

#include "api.h"

/* ===========================================================================
 * Project Version
 * =========================================================================== */

/** Phiên bản Firmware (dùng cho GitHub & Rollback) */
#define FIRMWARE_VERSION         "4.4.1"

/* ===========================================================================
 * WiFi Configuration (Mặc định nếu NVS trống, hoặc cấu hình qua Captive Portal)
 * =========================================================================== */

/** SSID của mạng WiFi mặc định (nếu muốn debug) */
#define WIFI_SSID               ""

/** Mật khẩu WiFi (tuỳ chọn) */
#define WIFI_PASSWORD            ""

/** Số lần thử kết nối lại WiFi tối đa */
#define WIFI_MAX_RETRY           10

/** Thời gian chờ giữa các lần retry (ms) */
#define WIFI_RETRY_DELAY_MS      5000

/* ===========================================================================
 * Telegram Bot Configuration
 * Tạo bot tại: https://t.me/BotFather
 * Lấy Chat ID tại: https://t.me/userinfobot
 * =========================================================================== */

/** Token của Telegram Bot (lấy từ @BotFather) - Moved to api.h */
// #define TELEGRAM_BOT_TOKEN       "..."

/** Chat ID của người dùng (lấy từ @userinfobot) - Moved to api.h */
// #define TELEGRAM_CHAT_ID         "..."

/** Thời gian timeout cho long polling (giây) */
#define TELEGRAM_POLL_TIMEOUT_SEC    30

/** URL gốc của Telegram Bot API */
#define TELEGRAM_API_URL         "https://api.telegram.org/bot"

/* ===========================================================================
 * OpenAI API Configuration
 * Lấy API key tại: https://platform.openai.com/api-keys
 * =========================================================================== */

/** API Key của OpenAI - Moved to api.h */
// #define OPENAI_API_KEY           "..."

/** Model dùng cho classification (phân loại tin nhắn) */
#define OPENAI_MODEL             "gpt-4o-mini"

/** Model dùng cho embedding (tạo vector) */
#define OPENAI_EMBEDDING_MODEL   "text-embedding-3-small"

/** Số chiều embedding vector (384 tiết kiệm RAM, vẫn đủ chính xác) */
#define OPENAI_EMBEDDING_DIMENSIONS  384

/** Số token tối đa cho response từ LLM */
#define OPENAI_MAX_TOKENS        512

/** URL gốc của OpenAI API */
#define OPENAI_API_URL           "https://api.openai.com/v1"

/* ===========================================================================
 * Time Configuration
 * =========================================================================== */

/** Offset múi giờ so với UTC (Việt Nam = +7) */
#define TIMEZONE_OFFSET_HOURS    7

/** Chuỗi timezone POSIX cho Việt Nam */
#define TIMEZONE_POSIX           "ICT-7"

/** Server SNTP để đồng bộ thời gian */
#define SNTP_SERVER              "time.google.com"

/* ===========================================================================
 * Task Database Configuration
 * =========================================================================== */

/** Số lượng task tối đa trong hệ thống */
#define MAX_TASK_COUNT           300

/** Độ dài tối đa của tiêu đề task (bytes, bao gồm UTF-8) */
#define TASK_TITLE_MAX_LEN       128

/** Độ dài tối đa của ghi chú task (bytes) */
#define TASK_NOTES_MAX_LEN       256

/** Đường dẫn gốc SPIFFS mount point */
#define SPIFFS_BASE_PATH         "/spiffs"

/** Thư mục chứa file task JSON */
#define TASK_DIR_PATH            "/spiffs/tasks"

/** Thư mục chứa file embedding binary */
#define EMBEDDING_DIR_PATH       "/spiffs/embeddings"

/** Tên file index tổng hợp */
#define TASK_INDEX_FILE          "/spiffs/tasks/index.json"

/* ===========================================================================
 * Semantic Search Configuration
 * =========================================================================== */

/** Số chiều vector embedding */
#define EMBEDDING_DIM            384

/** Ngưỡng similarity tối thiểu để coi là match */
#define SIMILARITY_THRESHOLD     0.70f

/** Số kết quả top-K trả về khi tìm kiếm */
#define SEARCH_TOP_K             3

/* ===========================================================================
 * Reminder Configuration
 * =========================================================================== */

/** Chu kỳ kiểm tra reminder (giây) */
#define REMINDER_CHECK_INTERVAL_SEC  60

/* ===========================================================================
 * Hardware Configuration
 * =========================================================================== */
/** Chân đèn LED xanh trên board (ESP32-C3 Super Mini là GPIO 8, Active Low) */
#define SYSTEM_LED_GPIO          8

/* ===========================================================================
 * Display ST7565R SPI Configuration (LCD mới)
 * =========================================================================== */
#define DISPLAY_SPI_SCK_GPIO    0
#define DISPLAY_SPI_MOSI_GPIO   1
#define DISPLAY_SPI_CS_GPIO     10  // Switched to GPIO 10 for absolute safety
#define DISPLAY_SPI_DC_GPIO     2
#define DISPLAY_SPI_RST_GPIO    3



/* ===========================================================================
 * System Buffer Configuration
 * =========================================================================== */

/** Kích thước buffer cho HTTP response (bytes) — cần >= 8KB cho embedding response */
#define HTTP_BUFFER_SIZE         10240

/** Kích thước buffer cho JSON parsing (bytes) */
#define JSON_BUFFER_SIZE         4096

/** Kích thước buffer cho response formatting (bytes) */
#define RESPONSE_BUFFER_SIZE     1024

/** Kích thước buffer cho Telegram message (bytes) */
#define TELEGRAM_MSG_BUFFER_SIZE 4096

/** Kích thước buffer cho OpenAI request body (bytes) */
#define OPENAI_REQUEST_BUFFER_SIZE 10240

/* ===========================================================================
 * Firebase Cloud Sync Configuration
 * =========================================================================== */
/** Firebase Host - Moved to api.h */
// #define FIREBASE_HOST           "..."
/** Firebase Auth Key - Moved to api.h */
// #define FIREBASE_AUTH           "..."


#endif /* CONFIG_H */
