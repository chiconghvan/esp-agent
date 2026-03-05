# ESP-Agent: Trợ lý nhắc việc thông minh v1.1.0

> Firmware ESP-IDF cho ESP32-C3 Super Mini — quản lý công việc qua Telegram Bot + OpenAI LLM + Màn hình OLED SSD1306.

---

## 📋 Tính năng nổi bật

- **Điều khiển bằng tiếng Việt tự nhiên**: Chấm dứt việc nhớ các cú pháp phức tạp. Chỉ cần nhắn tin như đang nói chuyện với một người bạn.
- **AI Intent Parsing**: Sử dụng `gpt-4o-mini` để phân tích yêu cầu (Thêm, Sửa, Xóa, Hoàn thành, Tìm kiếm).
- **Semantic Search**: Tìm kiếm công việc theo ý nghĩa (ví dụ: hỏi "Báo cáo đâu?" khi tiêu đề task là "Nộp văn bản tổng kết") nhờ OpenAI Embeddings.
- **Bảo mật dữ liệu (Safe Delete)**: Cơ chế xác thực 2 bước bằng lệnh `/confirm` trước khi xóa vĩnh viễn dữ liệu.
- **Màn hình OLED Carousel**: Tự động hiển thị các deadline trong 3 ngày tới với hiệu ứng Slide mượt mà.
- **Nút bấm đa năng (Bật/Tắt & WiFi Reset)**: 
    - Nhấn nút BOOT: Cuộn xem các deadline tiếp theo.
    - Giữ nút BOOT (5 giây): Xóa cài đặt WiFi và khởi động lại.
- **Nhắc nhở đa kênh**: Thông báo đồng thời qua Telegram và hiển thị ALERT nhấp nháy trên màn hình OLED.

---

## 🔧 Phần cứng & Nối dây

### 1. Linh kiện
- **MCU**: ESP32-C3 Super Mini (Kích thước cực nhỏ, chip RISC-V).
- **Display**: Màn hình OLED 0.96 inch (SSD1306, độ phân giải 128x64, giao tiếp I2C).
- **Nút bấm**: Sử dụng nút **BOOT (GPIO 9)** có sẵn trên mạch.

### 2. Sơ đồ nối dây (Wiring)

| OLED Pin | ESP32-C3 Pin | Ghi chú |
|----------|--------------|---------|
| **VCC**  | 3V3          | Nguồn 3.3V |
| **GND**  | GND          | Nối đất |
| **SDA**  | **GPIO 6**   | Serial Data |
| **SCL**  | **GPIO 7**   | Serial Clock |

---

## ⚙️ Cài đặt & Flash Firmware

### Bước 1: Chuẩn bị Config
1. Sao chép file `main/config.h.example` thành `main/config.h`.
2. Mở `main/config.h` và điền các thông số:
   - `WIFI_SSID`, `WIFI_PASSWORD`
   - `TELEGRAM_BOT_TOKEN`, `TELEGRAM_CHAT_ID`
   - `OPENAI_API_KEY`

### Bước 2: Build & Flash
Sử dụng môi trường **ESP-IDF** (v5.x):
```bash
# Thiết lập môi trường (nếu chưa có)
./install.sh esp32c3
. ./export.sh

# Build dự án
idf.py build

# Flash vào thiết bị (macOS ví dụ)
idf.py -p /dev/cu.usbmodem* flash monitor
```

---

## 🚀 Hướng dẫn sử dụng

### 1. Qua Telegram
- **Thêm task**: "Nhắc tôi nộp báo cáo lúc 8h sáng mai"
- **Tìm kiếm**: "Tuần này có báo cáo nào không?"
- **Hoàn thành**: "Tôi đã xong task nộp báo cáo"
- **Xóa vĩnh viễn**: "Xóa hẳn task báo cáo" -> Hệ thống sẽ gửi yêu cầu xác nhận -> Gõ `/confirm` để đồng ý.
- **Lệnh nhanh**:
    - `/alltask`: Xem toàn bộ danh sách công việc.
    - `/t [ID]`: Xem chi tiết công việc theo mã ID (Ví dụ: `/t 23`).

### 2. Qua thiết bị (OLED)
- **Màn hình chờ (Idle)**: Tự động xoay vòng hiển thị các task đến hạn trong vòng 3 ngày tới.
- **Chấm chỉ thị (Dots)**: Các chấm ở đáy màn hình cho biết số lượng task bạn đang có và vị trí hiện tại.
- **Nút bấm**: Nhấn nút vật lý trên mạch để cuộn nhanh qua các task mà không cần đợi tự động chuyển trang.

---

## 📁 Cấu trúc thư mục chính
- `main/actions/`: Logic xử lý từng loại yêu cầu từ người dùng.
- `main/display/`: Driver SSD1306 và quản lý giao diện Carousel/Alert.
- `main/database/`: Hệ thống lưu trữ SPIFFS và tìm kiếm Vector (Semantic Search).
- `main/telegram/`: Giao tiếp với Telegram Bot API.

---

## 📝 Giấy phép
Dự án được phát hành dưới giấy phép **MIT**. Mọi đóng góp xin vui lòng tạo Pull Request hoặc Issue trên GitHub.

---
**Phiên bản:** v1.1.0  
**Tác giả:** chiconghvan  
**Cập nhật cuối:** 2026-03-05
