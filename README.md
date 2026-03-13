# ESP-Agent: Trợ lý nhắc việc thông minh 3.1.1

> Firmware ESP-IDF cho ESP32-C3 Super Mini — quản lý công việc qua Telegram Bot + OpenAI LLM + Màn hình OLED SSD1306 + Firebase Cloud Sync.

---

## 📋 Tính năng nổi bật

- **Điều khiển bằng tiếng Việt tự nhiên**: Chấm dứt việc nhớ các cú pháp phức tạp. Chỉ cần nhắn tin như đang nói chuyện với một người bạn.
- **AI Intent Parsing Cải Tiến (Kiến trúc 2 bước)**: Sử dụng `gpt-4o-mini` để phân loại ý định (B1) cực nhanh và trích xuất chi tiết (B2) chính xác. Khắc phục hoàn toàn lỗi lệch ngày tháng, phân biệt rõ ràng giữa việc sửa, xóa, và tìm kiếm.
- **Tối ưu Thời gian Tương Lai (Bối cảnh Đa chiều)**: Giải quyết dứt điểm điểm yếu của LLM khi không hiểu vòng lặp thời gian thực tế. Thuật toán tự rẽ nhánh cung cấp Bối cảnh cụ thể của "Tuần Sau", "Quý Tới". Task lặp lại vĩnh viễn không bao giờ sợ bị lùi ngày.
- **Micro-Optimization RAM System (Cấp phát động)**: Tái sử dụng vùng nhớ HTTP Response, triệt tiêu Mảng tĩnh cỡ bự (Static Arrays / Stack) tránh hoàn toàn hiện tượng Guru Meditation (Stack Overflow) khi Firmware Up-Scale.
- **Web Log Viewer Delta Update**: Xem log hệ thống không dây trực tiếp qua trình duyệt tại địa chỉ `http://esp-agent.local/log`. Ứng dụng công nghệ Chunked Transfer Encoding kết hợp JS Vanilla Delta Streaming để chỉ lấy các Log mới (siêu mượt, không tốn RAM).
- **Đồng bộ thời gian siêu tốc (Mới)**: Lấy thời gian chuẩn xác ngay từ HTTP Date Header của Telegram thay vì chờ SNTP.
- **Task Định Kỳ Thông Minh**: Tự động dời hạn sang chu kỳ tiếp theo và giữ nguyên ID khi bạn đánh dấu "hoàn thành" một task lặp lại.
- **Hybrid Search (Adaptive Alpha Weighting)**: Kết hợp sức mạnh của tìm kiếm ngữ nghĩa (Semantic) và từ khóa chính xác (Lexical). Tự động điều chỉnh trọng số để tìm chính xác cả những từ viết tắt hoặc kí hiệu đặc thù (VD: "nq 66.7").
- **Captive Portal WiFi Setup**: Tự động phát Wifi AP khi chưa có cấu hình. Giao diện Web hiện đại, mượt mà giúp người dùng quét và chọn mạng WiFi bằng điện thoại một cách dễ dàng.
- **Bảo mật dữ liệu (Safe Delete)**: Cơ chế xác thực 2 bước bằng lệnh `/confirm` trước khi xóa vĩnh viễn dữ liệu.
- **Màn hình OLED Carousel**: Tự động hiển thị các deadline trong 3 ngày tới với hiệu ứng Slide mượt mà.
- **Nút bấm đa năng (Bật/Tắt & WiFi Reset)**: 
    - Nhấn nút BOOT: Cuộn xem các deadline tiếp theo.
    - Giữ nút BOOT (5 giây): Xóa cài đặt WiFi và khởi động lại.
- **Firebase Cloud Sync**: Tự động đồng bộ dữ liệu lên Firebase Realtime Database. Khôi phục dữ liệu tự động khi đổi thiết bị (Pull on Boot).
- **Tính năng Hoàn tác (Undo)**: Cho phép hoàn tác nhanh thao tác Thêm/Sửa/Xóa/Hoàn thành cuối cùng thông qua nút bấm trên Telegram.

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
   - `FIREBASE_HOST`, `FIREBASE_AUTH` (Database Secret)

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
    - `/t [ID]`: Xem chi tiết toàn bộ (12 trường) công việc theo mã ID (Ví dụ: `/t 23`).

### 2. Qua thiết bị (OLED)
- **Màn hình chờ (Idle)**: Tự động xoay vòng hiển thị các task đến hạn trong vòng 3 ngày tới.
- **Chấm chỉ thị (Dots)**: Các chấm ở đáy màn hình cho biết số lượng task bạn đang có và vị trí hiện tại.
- **Nút bấm**: Nhấn nút vật lý trên mạch để cuộn nhanh qua các task mà không cần đợi tự động chuyển trang.

---

## 📁 Cấu trúc thư mục chính
- `main/actions/`: Logic xử lý từng loại yêu cầu từ người dùng với kiến trúc dispatcher 2 bước.
- `main/display/`: Driver SSD1306 và quản lý giao diện Carousel/Alert.
- `main/database/`: Hệ thống lưu trữ SPIFFS và tìm kiếm Vector (Semantic Search).
- `main/telegram/`: Giao tiếp với Telegram Bot API.
- `main/utils/`: HTTP Server Log, Format phản hồi (Formatter) và xử lý thời gian.

---

## 📝 Giấy phép
Dự án được phát hành dưới giấy phép **MIT**. Mọi đóng góp xin vui lòng tạo Pull Request hoặc Issue trên GitHub.

---
**Phiên bản:** 3.1.1  
**Tác giả:** chiconghvan  
**Cập nhật cuối:** Lần gần nhất (v3.1.1 - Micro Optimization & Time Context Fixing)
