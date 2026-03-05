# ESP-Agent: Trợ lý nhắc việc thông minh

> Firmware ESP-IDF cho ESP32-C3 Super Mini — quản lý công việc qua Telegram Bot + OpenAI LLM

## 📋 Tính năng

### 7 Action cốt lõi

| # | Action | Mô tả | Ví dụ |
|---|--------|--------|-------|
| 🟢 | **CREATE_TASK** | Tạo task mới | "Nhắc tôi nộp báo cáo quý 1 vào 31/3" |
| 🔵 | **QUERY_BY_TIME_RANGE** | Hỏi lịch theo thời gian | "Ngày mai có gì?" |
| 🟣 | **QUERY_BY_TYPE** | Hỏi theo loại task | "Có báo cáo nào đến hạn không?" |
| 🟡 | **QUERY_SEMANTIC** | Tìm kiếm theo nghĩa | "Thời hạn báo cáo quý 1?" |
| 🟠 | **COMPLETE_TASK** | Đánh dấu hoàn thành | "Tôi đã hoàn thành báo cáo quý 1" |
| 🔴 | **DELETE_TASK** | Xóa/hủy task | "Hủy lịch họp ngày mai" |
| 🟤 | **UPDATE_TASK** | Sửa task | "Dời lịch họp sang thứ 7" |

### Tính năng nổi bật

- **Ngôn ngữ tự nhiên tiếng Việt** — nhắn tin bình thường, AI hiểu và xử lý
- **Semantic search** — tìm kiếm theo nghĩa bằng OpenAI embedding vectors 384 chiều
- **Nhắc nhở tự động** — gửi Telegram khi đến hạn
- **Task lặp lại** — hỗ trợ daily/weekly/monthly, tự tạo bản tiếp theo khi hoàn thành
- **Tối ưu API calls** — mỗi tin nhắn chỉ gọi tối đa 2 API calls (1 classification + 1 embedding)
- **Response chi tiết** — mỗi response đều hiển thị đầy đủ: tên, thời hạn, lặp lại, ngày tạo

---

## 🔧 Phần cứng

### Thiết bị cần thiết

| Thiết bị | Mô tả |
|----------|--------|
| ESP32-C3 Super Mini | MCU chính (RISC-V, 400KB RAM, 4MB Flash) |
| Cáp USB-C | Kết nối và flash firmware |

### Sơ đồ kết nối

ESP32-C3 Super Mini chỉ cần cấp nguồn qua USB-C. Không cần kết nối thêm phần cứng.

```
   USB-C
     │
     ▼
┌─────────────┐
│  ESP32-C3   │
│  Super Mini │
│             │
│  WiFi 📡   │  ← Kết nối WiFi 2.4GHz
│             │
│  Flash 4MB  │  ← Lưu tasks + embeddings
│  RAM 400KB  │
└─────────────┘
```

---

## ⚙️ Cài đặt & Flash Firmware

### Yêu cầu

- **ESP-IDF** v5.x (khuyến nghị v5.1+)
- **Python** 3.8+
- **USB Driver** cho ESP32-C3

### Bước 1: Cài đặt ESP-IDF

```bash
# Tải và cài đặt ESP-IDF
mkdir -p ~/esp
cd ~/esp
git clone -b v5.1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32c3
source export.sh
```

### Bước 2: Cấu hình Project

Mở file `main/config.h` và điền thông tin:

```c
/* WiFi */
#define WIFI_SSID               "tên_wifi_của_bạn"
#define WIFI_PASSWORD            "mật_khẩu_wifi"

/* Telegram Bot (tạo tại https://t.me/BotFather) */
#define TELEGRAM_BOT_TOKEN       "xxxx:yyyy"
#define TELEGRAM_CHAT_ID         "123456789"

/* OpenAI API (lấy tại https://platform.openai.com/api-keys) */
#define OPENAI_API_KEY           "sk-xxxx"
```

**Cách lấy Telegram Chat ID:**
1. Nhắn bất kỳ tin nhắn nào cho bot [@userinfobot](https://t.me/userinfobot)
2. Bot sẽ trả về Chat ID của bạn

### Bước 3: Build & Flash

```bash
# Di chuyển vào thư mục project
cd /path/to/ESP-Agent

# Set target ESP32-C3
idf.py set-target esp32c3

# Build firmware
idf.py build

# Flash (thay /dev/ttyUSB0 bằng port thực tế)
idf.py -p /dev/ttyUSB0 flash

# Mở Serial Monitor
idf.py -p /dev/ttyUSB0 monitor
```

> **macOS:** Port thường là `/dev/cu.usbmodem*` hoặc `/dev/cu.SLAB_USBtoUART`
> **Windows:** Port thường là `COM3`, `COM4`, ...

### Bước 4: Kiểm tra hoạt động

Sau khi flash thành công, Serial Monitor sẽ hiển thị:

```
=============================================
  ESP-Agent: Trợ lý nhắc việc thông minh
  Target: ESP32-C3 Super Mini
  Version: 1.0.0
=============================================
[1/5] NVS Flash OK
[2/5] WiFi OK
[2/5] Đồng bộ thời gian SNTP...
[3/5] Database OK
[4/5] Telegram Bot OK
[5/5] Reminder Scheduler OK
=============================================
  HỆ THỐNG SẴN SÀNG!
=============================================
```

Mở Telegram, tìm bot của bạn và gửi thử: **"Nhắc tôi nộp báo cáo quý 1 vào 31/3"**

---

## 📁 Cấu trúc Project

```
ESP-Agent/
├── CMakeLists.txt              # Project CMake
├── partitions.csv              # Partition table (NVS + SPIFFS)
├── sdkconfig.defaults          # ESP-IDF defaults
├── README.md                   # File này
│
├── main/
│   ├── CMakeLists.txt          # Component CMake
│   ├── main.c                  # Entry point
│   ├── config.h                # ⚙️ CẤU HÌNH NGƯỜI DÙNG
│   │
│   ├── wifi/                   # WiFi STA + SNTP
│   ├── telegram/               # Telegram Bot API
│   ├── openai/                 # OpenAI Chat + Embedding
│   ├── database/               # Task CRUD + Vector Search
│   ├── actions/                # 7 Action Handlers
│   ├── scheduler/              # Reminder Timer
│   └── utils/                  # Time, JSON, Response Format
│
└── spiffs_data/                # Initial SPIFFS data
```

---

## 🗄️ Database

- **Storage:** SPIFFS 2MB trên flash
- **Format:** JSON file riêng cho mỗi task + binary file cho embedding vector
- **Index:** File index tổng hợp cho truy vấn nhanh
- **Capacity:** Tối đa 300 tasks
- **Embedding:** 384 chiều (OpenAI `text-embedding-3-small`)

---

## 🔌 API sử dụng

| API | Mục đích | Model |
|-----|----------|-------|
| OpenAI Chat Completion | Phân loại tin nhắn (1 lần/msg) | `gpt-4o-mini` |
| OpenAI Embedding | Tạo vector tìm kiếm (0-1 lần/msg) | `text-embedding-3-small` |
| Telegram Bot API | Nhận/gửi tin nhắn | getUpdates, sendMessage |

---

## 📝 License

MIT License

---

## 🙏 Credits

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) — Espressif IoT Development Framework
- [OpenAI API](https://platform.openai.com/) — LLM và Embedding
- [Telegram Bot API](https://core.telegram.org/bots/api) — Messaging
