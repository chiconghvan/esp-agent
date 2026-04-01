# ESP-Agent: Trợ lý nhắc việc thông minh v4.4.1

> Firmware tối ưu cho **ESP32-C3 Super Mini** (RISC-V) — Tích hợp **Telegram Bot**, **OpenAI LLM**, màn hình **LCD ST7565R (SPI)** và **Firebase Cloud Sync**.

---

## 📋 Tính năng nổi bật (v4.4.x)

- **Màn hình LCD ST7565R SPI (Mới)**:
    - **Tốc độ phản hồi cao**: Chuyển đổi từ I2C SSD1306 sang giao thức SPI nhanh và ổn định hơn.
    - **Hiển thị linh hoạt**: Hỗ trợ đầy đủ thư viện `u8g2`, tối ưu cho các module như GMG12864-06D.
    - **Page Indicator**: Chỉ báo trang phong cách 3x3 pixel hiện đại, trực quan.
- **AI Intent Parsing & Semantic Search**:
    - **GPT-4o-mini Integration**: Phân loại ý định người dùng (Intent) và bóc tách thông tin (Entities) cực kỳ chính xác.
    - **Hybrid Search**: Tìm kiếm kết hợp giữa từ khóa (Lexical) và ý nghĩa (Semantic Vector) với ngưỡng tương đồng (Similarity) tùy chỉnh.
- **Hệ thống Quản lý Bộ nhớ "Extreme"**:
    - **Network Gatekeeper (Mutex)**: Tuần tự hóa các yêu cầu HTTPS để tránh lỗi `ESP_ERR_NO_MEM` trên RAM hạn chế (400KB) của ESP32-C3.
    - **Font Subsetting**: Chỉ nạp các ký tự Tiếng Việt cần thiết từ font Iosevka để tiết kiệm dung lượng Flash.
- **Trải nghiệm khởi động (Boot Experience)**:
    - **Real-time Progress Bar**: Thanh tiến trình đồng bộ chính xác với quá trình khởi tạo (WiFi -> Time -> Cloud -> AI).
    - **Vietnamese Status**: Hiển thị trạng thái chi tiết bằng tiếng Việt trong suốt quá trình boot.
- **Đồng bộ hóa & Nhắc nhở**:
    - **Firebase Cloud Sync**: Tự động đồng bộ task, cấu hình và dung lượng API sử dụng.
    - **Intelligent Reminder**: Kiểm tra và thông báo task sắp đến hạn theo thời gian thực.

---

## 🔧 Phân bổ phần cứng (Hardware Allocation)

### 1. Thành phần chính
- **MCU**: ESP32-C3 Super Mini.
- **Display**: LCD ST7565R Monochrome (SPI) — Độ phân giải 128x64.
- **LED**: System Status LED (GPIO 8).

### 2. Sơ đồ nối dây (Wiring Diagram)

Dưới đây là sơ đồ kết nối chuẩn đã được kiểm kê và ổn định trên board ESP32-C3 Super Mini:

| LCD ST7565R Pin | ESP32-C3 Pin | GPIO | Chức năng |
| :--- | :--- | :--- | :--- |
| **VCC** | 3.3V | - | Nguồn cấp (3.3V) |
| **GND** | GND | - | Đất |
| **SCK / SCL** | **GPIO 0** | 0 | SPI Clock |
| **SDA / MOSI** | **GPIO 1** | 1 | SPI Master Out Slave In |
| **CS / Chip Select** | **GPIO 10** | 10 | Chip Select (Kênh an toàn) |
| **RS / DC** | **GPIO 2** | 2 | Register Select / Data Command |
| **RST / Reset** | **GPIO 3** | 3 | Hardware Reset |
| **LEDA (Backlight)** | 3.3V | - | Đèn nền (Nối 3.3V hoặc qua trở) |

> [!IMPORTANT]
> **System LED**: GPIO 8 (Active Low) là đèn LED mặc định trên board ESP32-C3 Super Mini, dùng để chỉ báo trạng thái hệ thống.

---

## ⚙️ Cài đặt & Flash Firmware

### Bước 1: Cấu hình thông tin nhạy cảm
1. Sao chép `main/api.h.sample` thành `main/api.h`.
2. Điền các khóa API (Telegram Token, Chat ID, OpenAI Key, Firebase Host/Auth).
3. Đảm bảo `api.h` đã được thêm vào `.gitignore` để bảo mật.

### Bước 2: Build & Flash
Sử dụng **ESP-IDF v5.1** hoặc mới hơn để có kết quả tốt nhất.

```bash
# Cài đặt môi trường (nếu chưa)
. $HOME/export.sh

# Thực hiện build và nạp
idf.py build
idf.py flash monitor
```

---

## 🚀 Hướng dẫn sử dụng

### 1. Tương tác qua Telegram
- **Thêm việc**: *"Nhắc tôi mang Laptop đi sửa lúc 14h ngày mai"*
- **Xóa/Sửa**: Gửi tên task hoặc ID cụ thể. Hệ thống sẽ gợi ý nếu ý định không rõ ràng.
- **Tìm kiếm**: *"Tuần sau có việc gì quan trọng không?"*
- **Thống kê**: *"Hôm nay tôi đã dùng bao nhiêu tiền OpenAI?"*

### 2. Tương tác trên thiết bị (Display UI)
- **Màn hình Carousel**: Tự động hiển thị các task sắp đến hạn (theo chu kỳ 5s).
- **Phím cuộn (BOOT button)**: 
    - Nhấn 1 lần: Chuyển sang trang tiếp theo.
    - Nhấn đúp: Chuyển đổi giữa chế độ xem **Hôm nay** và **Sắp tới**.
    - Giữ 5 giây: Reset cấu hình WiFi (Phát Captive Portal).

---

## 📂 Cấu trúc thư mục

```text
├── components/          # HAL cho U8g2 và các thư viện bên thứ 3
├── main/
│   ├── actions/         # Logic xử lý Intent và thực thi lệnh AI
│   ├── cloud/           # Firebase Sync Client
│   ├── database/        # Quản lý SPIFFS & Vector Search
│   ├── display/         # UI Engine, Layout & Font Management
│   ├── openai/          # OpenAI API Client (Embedding & Chat)
│   ├── telegram/        # Telegram Bot Client & Polling
│   ├── utils/           # Time, String, Memory helpers
│   ├── config.h         # Cấu hình tham số hệ thống
│   └── api.h            # Cấu hình API Keys (Bảo mật)
└── spiffs_data/         # Dữ liệu tĩnh sẽ flash vào SPIFFS
```

---

## 📜 Giấy phép & Tác giả

Dự án được phát hành dưới giấy phép **MIT**.

- **Phiên bản:** 4.4.1
- **Tác giả:** chiconghvan
- **Cập nhật cuối:** 2026-03-31 (Migrated to ST7565R SPI LCD & Detailed Wiring Update)
