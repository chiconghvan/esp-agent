# 🤖 ESP-Agent — AI Agent Context Guide

> **Mục đích:** File này cung cấp toàn bộ kiến thức cần thiết cho AI agent hiểu codebase ngay lập tức.  
> **Phiên bản:** v4.0.0 — Cập nhật: 2026-03-15  
> **Platform:** ESP-IDF v5.3 • ESP32-C3 Super Mini • 4MB Flash • ~320KB RAM  

---

## 1. Kiến trúc Project (Architecture)

### 1.1 Tổng quan kiến trúc

```
┌──────────────────────────────────────────────────┐
│               Telegram Bot API                   │
│          (Long Polling, Send Message)            │
└─────────────────────┬────────────────────────────┘
                      │
┌─────────────────────▼────────────────────────────┐
│                  main.c                          │
│   (app_main → init → telegram_polling_loop)      │
└──┬──────┬───────┬───────┬───────┬────────┬───────┘
   │      │       │       │       │        │
   ▼      ▼       ▼       ▼       ▼        ▼
┌─────┐┌──────┐┌──────┐┌──────┐┌───────┐┌───────┐
│WiFi ││Tele- ││OpenAI││Data- ││Display││Fire-  │
│Mgr  ││gram  ││Client││base  ││Mgr    ││base   │
│     ││Bot   ││      ││+ Vec ││(OLED) ││Sync   │
└─────┘└──────┘└──────┘└──────┘└───────┘└───────┘
                  │         │
           ┌──────▼─────────▼──────┐
           │  Action Dispatcher    │
           │  B1: Intent Classify  │
           │  B2: Query Parser     │
           ├───────────────────────┤
           │  action_create.c      │
           │  action_update.c      │
           │  action_complete.c    │
           │  action_delete.c      │
           │  action_query.c       │
           │  action_detail.c      │
           │  action_search.c      │
           │  action_summary.c     │
           │  action_undo.c        │
           └───────────────────────┘
```

### 1.2 Cấu trúc thư mục

```
ESP-Agent/
├── CMakeLists.txt              # Root CMake (project name, IDF version)
├── partitions.csv              # Bảng phân vùng Flash: NVS 24KB + App 1.9MB + SPIFFS 2MB
├── sdkconfig.defaults          # Cấu hình mặc định: target esp32c3, stack 16KB, WDT 60s
├── prompt.txt                  # Prompt gốc cho AI tham khảo thiết kế (không ảnh hưởng code)
├── README.md                   # Giới thiệu project
├── agent.md                    # File này — hướng dẫn cho AI agent
├── spiffs_data/                # Dữ liệu mẫu SPIFFS (nếu có)
│
└── main/
    ├── CMakeLists.txt          # Liệt kê toàn bộ source files + include dirs + requires
    ├── main.c                  # Entry point: app_main(), khởi tạo hệ thống, polling loop
    ├── config.h                # ⚠ FILE BÍ MẬT — chứa API keys, WiFi creds (gitignored)
    ├── config.h.example        # File mẫu cấu hình (committed to git)
    │
    ├── wifi/
    │   ├── wifi_manager.h      # API: wifi_manager_init(), start_sntp(), is_connected()
    │   └── wifi_manager.c      # Kết nối STA, retry, SNTP, Captive Portal (APSTA mode)
    │
    ├── telegram/
    │   ├── telegram_bot.h      # API: init(), get_update(), send_message(), send_inline_keyboard()
    │   └── telegram_bot.c      # Long polling getUpdates, sendMessage qua HTTPS
    │
    ├── openai/
    │   ├── openai_client.h     # API: chat_completion(), create_embedding()
    │   └── openai_client.c     # Gọi OpenAI Chat + Embedding API qua HTTPS
    │
    ├── database/
    │   ├── task_database.h     # API: CRUD tasks, query_by_time/type, index management
    │   ├── task_database.c     # Lưu trữ JSON files trên SPIFFS, in-memory index
    │   ├── vector_search.h     # API: save/load/delete embedding, find_similar (hybrid search)
    │   └── vector_search.c     # Cosine similarity + Adaptive Alpha Weighting (lexical+semantic)
    │
    ├── actions/
    │   ├── action_dispatcher.h # Enum action_type_t, filter_op_t, query_filter_t, query_request_t
    │   ├── action_dispatcher.c # ★ TRUNG TÂM ★ B1 prompt (intent), B2 prompt (parse), dispatch
    │   ├── action_create.c     # CREATE_TASK: parse JSON → tạo task + embedding
    │   ├── action_update.c     # UPDATE_TASK: semantic search → cập nhật fields
    │   ├── action_complete.c   # COMPLETE_TASK: đánh dấu done, tạo repeat task nếu có
    │   ├── action_delete.c     # DELETE_TASK: soft delete / hard delete + /confirm flow
    │   ├── action_query.c      # QUERY_TASKS: filter engine duyệt index, match filters
    │   ├── action_detail.c     # GET_TASK_DETAIL: semantic search → trả chi tiết 1 task
    │   ├── action_search.c     # SEARCH_SEMANTIC: embedding search → trả top-K
    │   ├── action_summary.c    # TASK_SUMMARY: thống kê pending/done/overdue trong period
    │   ├── action_undo.h       # Enum undo_action_type_t, cấu trúc undo state
    │   └── action_undo.c       # Lưu/khôi phục trạng thái cũ (1 bước undo)
    │
    ├── display/
    │   ├── display_manager.h   # API: init(), boot_progress(), show_idle/result/alert()
    │   ├── display_manager.c   # State machine BOOT→IDLE↔RESULT/ALERT, carousel animation
    │   ├── ssd1306.h           # Driver API cho SSD1306 OLED (I2C)
    │   ├── ssd1306.c           # Thao tác pixel, text, invert, scroll
    │   ├── font6x8.h           # Font bitmap 6x8 ASCII
    │   ├── vn_utils.h          # API: strip dấu tiếng Việt UTF-8 → ASCII
    │   └── vn_utils.c          # Bộ chuyển đổi ký tự Unicode → ASCII tương đương
    │
    ├── cloud/
    │   ├── firebase_sync.h     # API: init(), upload_task(), upload_all(), download_all()
    │   └── firebase_sync.c     # FreeRTOS background task, Queue-based upload/delete
    │
    ├── scheduler/
    │   ├── reminder_scheduler.h # API: start()
    │   └── reminder_scheduler.c # FreeRTOS task kiểm tra reminder mỗi 60s
    │
    └── utils/
        ├── time_utils.h        # API: get_now(), format_vietnamese/iso8601(), next_due(), weekday
        ├── time_utils.c        # Xử lý thời gian, parse ISO8601, tính repeat cycle
        ├── json_parser.h       # API: parse_string(), get_string/int/double/bool/array()
        ├── json_parser.c       # Wrapper cJSON an toàn, tránh null pointer
        ├── response_formatter.h # API: format_task_list/detail/completed/error/not_found()
        ├── response_formatter.c # Formatter chuỗi response UTF-8 + emoji cho Telegram
        ├── token_tracker.h     # API: init(), add_usage(), print_summary()
        ├── token_tracker.c     # Theo dõi token OpenAI usage, lưu NVS theo tháng
        └── safe_append.h       # Macro APPEND_SNPRINTF an toàn cho buffer append
```

### 1.3 Luồng xử lý chính (Main Workflow)

```
Telegram Message
    │
    ▼
telegram_polling_loop() [main.c]
    │
    ├── Lệnh đặc biệt: /confirm, /alltask, /t <id>, /undo, /json
    │
    └── Tin nhắn thường → action_dispatcher_handle()
        │
        ├── B1: Intent Classification (1 lần gọi AI)
        │   → Trả: { "intent": "...", "confidence": 0.0-1.0 }
        │   → 9 Intent: CREATE_TASK, UPDATE_TASK, COMPLETE_TASK,
        │              DELETE_TASK, QUERY_TASKS, GET_TASK_DETAIL,
        │              SEARCH_SEMANTIC, TASK_SUMMARY, CHITCHAT
        │
        ├── B2: Query Parser (1 lần gọi AI, prompt khác nhau theo intent)
        │   → Trả: JSON chuyên biệt cho từng intent
        │
        └── Dispatch → action_xxx() handler tương ứng
            │
            ├── Truy vấn CSDL local (SPIFFS) hoặc Vector Search
            ├── Format response (response_formatter)
            ├── Cập nhật OLED display
            └── Gửi response về Telegram
```

### 1.4 Cấu trúc dữ liệu quan trọng

**task_record_t** (Task chính):
| Field            | Type      | Mô tả                                              |
|------------------|-----------|-----------------------------------------------------|
| `id`             | uint32_t  | Auto-increment ID                                   |
| `title`          | char[128] | Tiêu đề task (UTF-8)                                |
| `type`           | char[16]  | meeting / report / reminder / event / anniversary / other |
| `status`         | char[16]  | pending / done / cancelled / overdue                |
| `created_at`     | time_t    | Thời điểm tạo                                      |
| `start_time`     | time_t    | Bắt đầu (0 = không có)                             |
| `due_time`       | time_t    | Hạn chót (0 = không có)                             |
| `completed_at`   | time_t    | Hoàn thành (0 = chưa)                              |
| `reminder`       | time_t    | Nhắc nhở (0 = không)                               |
| `repeat`         | char[16]  | none / daily / weekly / monthly / quarterly / yearly |
| `repeat_interval` | int      | Khoảng cách lặp (VD: 2 = mỗi 2 tuần)              |
| `notes`          | char[256] | Ghi chú bổ sung                                    |

**Lưu trữ SPIFFS:**
- Tasks: `/spiffs/tasks/task_XXXX.json` (mỗi task 1 file JSON)
- Index: `/spiffs/tasks/index.json` (in-memory index, load khi boot)
- Embeddings: `/spiffs/embeddings/emb_XXXX.bin` (binary float[384])

### 1.5 Hybrid Search (Adaptive Alpha Weighting)

```
Final_Score = (α × Semantic_Score) + ((1 - α) × Lexical_Score)

Trong đó:
- Semantic_Score = cosine_similarity(query_embedding, task_embedding)
- Lexical_Score  = matched_words / total_words (tokenize bằng strtok_r)
- α (Adaptive Alpha):
    Lexical ≥ 0.8  →  α = 0.2  (Ưu tiên keyword 80%)
    Lexical ≥ 0.4  →  α = 0.4  (Ưu tiên keyword 60%)
    Mặc định       →  α = 0.5  (Cân bằng 50/50)
- Ngưỡng: SIMILARITY_THRESHOLD = 0.70f
```

---

## 2. Code Convention (Quy ước code)

### 2.1 Naming Convention

| Loại               | Quy tắc                    | Ví dụ                                  |
|--------------------|-----------------------------|----------------------------------------|
| File               | `snake_case.c / .h`        | `wifi_manager.c`, `task_database.h`    |
| Function           | `module_verb_noun()`        | `task_database_create()`, `wifi_manager_init()` |
| Static function    | `verb_noun()` (không prefix)| `parse_op()`, `match_filter()`         |
| Struct             | `module_noun_t`             | `task_record_t`, `search_result_t`     |
| Enum               | `module_noun_t` (ALL_CAPS members) | `action_type_t` → `ACTION_CREATE_TASK` |
| Macro / Define     | `ALL_CAPS_SNAKE`            | `EMBEDDING_DIM`, `MAX_TASK_COUNT`      |
| Global variable    | Tránh dùng; nếu có: `g_` prefix | (hiếm khi dùng trong project này)     |
| Static variable    | `s_` prefix                 | `s_context_task_ids`, `s_last_action_json` |
| TAG (ESP_LOG)      | `static const char *TAG`    | `"action_query"`, `"vector_search"`    |

### 2.2 File Structure

Mỗi `.c` file tuân thủ cấu trúc:

```c
/**
 * ===========================================================================
 * @file filename.c
 * @brief Mô tả ngắn gọn module
 *
 * Mô tả chi tiết hơn (nếu cần).
 * ===========================================================================
 */

#include "own_header.h"       // Header của chính module
#include "project_headers.h"  // Headers nội bộ project
#include <system_headers.h>   // Headers hệ thống
#include "esp_log.h"          // ESP-IDF headers

static const char *TAG = "module_name";

/* ========================== Private Functions ============================ */

static void helper_function(void) { ... }

/* =========================== Public Functions ============================ */

esp_err_t module_public_function(void) { ... }
```

### 2.3 Comment Style

```c
/* ==========================================================================
 * SECTION HEADER (dùng cho nhóm functions lớn)
 * ========================================================================== */

/* --------------------------------------------------------------------------
 * Sub-section (dùng cho từng function)
 * -------------------------------------------------------------------------- */

/** @brief Doxygen doc cho public function */

/* Inline comment giải thích logic */
```

### 2.4 Error Handling

- **Luôn dùng `esp_err_t`** làm return type cho public functions.
- **Kiểm tra NULL** ở đầu mỗi function: `if (param == NULL) return ESP_ERR_INVALID_ARG;`
- **Log lỗi** bằng `ESP_LOGE(TAG, "...")` trước khi return error.
- **Không dùng `assert()`** — thiết bị embedded không được crash.
- **`cJSON_Delete()`** phải luôn được gọi sau khi parse xong.
- **`free()`** phải luôn đi kèm với `malloc()`.

### 2.5 Ngôn ngữ

- **Code**: Tiếng Anh (function names, variable names).
- **Comment**: Tiếng Việt (giải thích logic, mô tả module).
- **Log messages (ESP_LOG)**: Tiếng Việt (vì user đọc terminal).
- **Telegram responses**: Tiếng Việt + Emoji UTF-8.
- **Prompt AI**: Tiếng Việt (vì input/output là tiếng Việt).

### 2.6 Memory Management

- **Stack allocation** cho buffers < 2KB: `float embedding[384];`
- **Heap allocation** cho buffers lớn: `char *prompt = malloc(3072);` → phải `free()`.
- **Không dùng VLA** (Variable Length Array) — ESP32 stack nhỏ.
- **Main task stack**: 16384 bytes (config trong sdkconfig.defaults).
- **Tổng free heap khả dụng**: ~110-130KB khi hệ thống chạy ổn định.

---

## 3. Quy tắc quan trọng (Critical Rules)

### 3.1 ⛔ KHÔNG ĐƯỢC SỬA

| File / Thư mục        | Lý do                                            |
|------------------------|--------------------------------------------------|
| `main/config.h`       | Chứa API keys/secrets riêng → gitignored. Chỉ sửa `config.h.example` |
| `sdkconfig`            | Auto-generated bởi ESP-IDF menuconfig            |
| `sdkconfig.old`        | Backup tự động                                   |
| `build/`               | Thư mục build output                             |
| `prompt.txt`           | Tài liệu thiết kế gốc, chỉ tham khảo            |

### 3.2 ⚠️ CẨN THẬN KHI SỬA

| File                           | Lưu ý                                                    |
|--------------------------------|-----------------------------------------------------------|
| `action_dispatcher.c`         | Chứa toàn bộ AI Prompt (B1, B2). Sửa prompt ảnh hưởng toàn bộ hành vi AI. Buffer prompt hiện tại = 3072 bytes. |
| `partitions.csv`              | Thay đổi layout flash → phải erase flash toàn bộ         |
| `main/CMakeLists.txt`         | Thêm file `.c` mới phải đăng ký ở đây                    |
| `vector_search.c`             | Thuật toán Hybrid Search. Thay đổi alpha/threshold ảnh hưởng chất lượng search |
| `config.h.example`            | Template cho người dùng mới. Giữ version đồng bộ với `config.h` |
| `wifi_manager.c`              | Có Captive Portal logic (APSTA mode). Rất phức tạp       |

### 3.3 ✅ QUY TẮC BẮT BUỘC

1. **Version Sync**: Khi cập nhật version, PHẢI sửa đồng bộ 3 nơi:
   - `main/config.h` → `FIRMWARE_VERSION`
   - `main/config.h.example` → `FIRMWARE_VERSION`
   - `README.md` → Tiêu đề + Footer

2. **Thêm file source mới**: PHẢI đăng ký trong `main/CMakeLists.txt`:
   - Thêm vào `COMPONENT_SRCS` (đường dẫn tương đối từ `main/`)
   - Thêm thư mục vào `COMPONENT_INCLUDE_DIRS` nếu là thư mục mới

3. **Sửa Prompt AI**: Sau khi sửa prompt trong `action_dispatcher.c`:
   - Kiểm tra tổng độ dài chuỗi prompt (phải < buffer size = 3072)
   - Test ít nhất 3 câu lệnh khác nhau qua Telegram

4. **Thêm Action mới**: Cần sửa ở 5 nơi:
   - Thêm enum trong `action_dispatcher.h` → `action_type_t`
   - Thêm prompt B2 trong `action_dispatcher.c`
   - Thêm parser trong `parse_intent()`
   - Thêm case trong `step_b2_parse()` và `step_b3_execute()`
   - Tạo file `action_xxx.c` + khai báo prototype trong `.h`

5. **Database Schema**: `task_record_t` thay đổi → PHẢI xem xét:
   - `task_database.c`: Hàm read/write JSON
   - `task_index_entry_t`: Index entry (lightweight version)
   - `firebase_sync.c`: Upload/download format
   - Tất cả action handlers sử dụng struct này

6. **Embedding Vector**: Kích thước cố định 384 chiều (`EMBEDDING_DIM`). Thay đổi giá trị này → phải xóa toàn bộ file embedding cũ trên SPIFFS.

### 3.4 🔑 Secrets & Sensitive Data

Các giá trị sau nằm trong `main/config.h` (gitignored):
- `WIFI_SSID`, `WIFI_PASSWORD`
- `TELEGRAM_BOT_TOKEN`, `TELEGRAM_CHAT_ID`
- `OPENAI_API_KEY`
- `FIREBASE_HOST`, `FIREBASE_AUTH`

**KHÔNG BAO GIỜ hardcode secrets vào các file khác hoặc commit lên git.**

### 3.5 📏 Giới hạn phần cứng

| Tài nguyên       | Giá trị          | Ghi chú                                    |
|-------------------|------------------|---------------------------------------------|
| Flash             | 4MB              | App ~1.2MB, SPIFFS ~2MB                    |
| RAM (Heap)        | ~320KB tổng      | Free khi chạy: ~110-130KB                  |
| Stack main task   | 16KB             | Cẩn thận với buffer lớn trên stack          |
| SPIFFS            | 2MB              | Chứa ~200-300 tasks + embeddings            |
| Max tasks         | 300              | `MAX_TASK_COUNT` trong config.h             |
| Embedding size    | 1.5KB/task       | 384 × 4 bytes = 1536 bytes                 |
| Watchdog timeout  | 60 giây          | HTTP request phải hoàn thành trong thời gian này |

---

## 4. Build & Test Commands

### 4.1 Thiết lập môi trường (Chỉ lần đầu)

```bash
# Source ESP-IDF environment (đã alias trong shell profile)
. ~/esp/esp-idf/export.sh
```

### 4.2 Build

```bash
# Build project
idf.py build

# Build output: build/esp-agent.bin (~1.2MB)
```

### 4.3 Flash & Monitor

```bash
# Flash + Monitor (phổ biến nhất, dùng lệnh này khi dev)
idf.py build flash monitor

# Chỉ flash (không monitor)
idf.py flash

# Chỉ monitor (xem log serial)
idf.py monitor

# Thoát monitor: Ctrl + ]
```

### 4.4 Clean Build

```bash
# Xóa build cache (khi gặp lỗi lạ)
idf.py fullclean

# Build lại từ đầu
idf.py build
```

### 4.5 Erase Flash (Reset toàn bộ)

```bash
# Xóa toàn bộ flash (mất hết NVS + SPIFFS data)
idf.py erase-flash
```

### 4.6 Serial Port

```bash
# macOS: Port thường là /dev/cu.usbmodem*
# Nếu port bận, kill process đang giữ:
pkill -f "idf.py monitor"

# Chỉ định port cụ thể:
idf.py -p /dev/cu.usbmodem1301 flash monitor
```

### 4.7 Git Workflow

```bash
# Commit + Tag + Push (quy trình release)
git add .
git commit -m "Release vX.Y.Z: Mô tả thay đổi"
git tag -a vX.Y.Z -m "Release vX.Y.Z: Mô tả"
git push origin main --tags
```

### 4.8 Checklist trước khi Build

- [ ] `idf.py build` không có **error** (warning `-Wunused-variable` chấp nhận được)
- [ ] Không có warning `-Werror=format-truncation` (buffer quá nhỏ cho snprintf)
- [ ] Free heap sau boot > 100KB (kiểm tra log `Free heap: XXXXX bytes`)
- [ ] Kiểm tra log `HỆ THỐNG SẴN SÀNG!` xuất hiện sau boot
- [ ] Test 1-2 lệnh Telegram cơ bản (tạo task, query)

---

## Phụ lục: Quick Reference

### ESP-IDF Components sử dụng

```
nvs_flash, esp_wifi, esp_event, esp_http_client, esp-tls,
spiffs, json (cJSON), esp_timer, esp_netif, lwip, mbedtls,
driver (I2C/GPIO), esp_http_server (Captive Portal), app_update (OTA)
```

### Luồng khởi động (Boot Sequence)

```
app_main()
  ├── [1/5] NVS Flash init
  ├── [2/5] WiFi init (STA hoặc Captive Portal nếu chưa có config)
  ├── [3/5] Database init (SPIFFS mount, load index)
  │         └── Firebase download (nếu local trống)
  ├── [4/5] Telegram Bot init
  ├── [5/5] Reminder Scheduler start
  ├── SNTP time sync
  ├── Firebase upload all (background)
  ├── Display init + show idle
  └── Telegram polling loop (infinite)
```
