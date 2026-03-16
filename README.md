# ESP-Agent: Trợ lý nhắc việc thông minh 4.1.0

> Firmware ESP-IDF cho ESP32-C3 Super Mini — Quản lý công việc qua Telegram Bot + OpenAI LLM + Màn hình OLED U8g2 Premium + Firebase Cloud Sync.

---

## 📋 Tính năng mới (v4.1.0)

- **Bộ công cụ Font chuyên dụng**:
    - `u8g2_font_util.py`: Tự động chuyển đổi file `.ttf` bất kỳ sang mảng C cho U8g2.
    - **Sửa lỗi Baseline**: Thuật toán căn chỉnh chân chữ cực chuẩn, không còn tình trạng chữ cao thấp không đều.
    - **Preview OLED thực tế**: Tự động sinh ảnh mô phỏng pixel-perfect 128x64 trên máy tính trước khi nạp vào chip.
    - `switch_font.py`: Kịch bản 1-click để áp dụng font mới vào toàn bộ project (tự động vá code, cấu hình CMake).
- **Trải nghiệm mượt mà 30 FPS**:
    - **Sliding Animation**: Hiệu ứng trượt ngang cực nhanh khi chuyển đổi nội dung task.
    - **I2C Fast Mode**: Tăng tốc bus dữ liệu lên **400kHz**, loại bỏ hoàn toàn hiện tượng lag khi render font lớn.
- **Hỗ trợ Tiếng Việt 100%**: Mở rộng dải mã Unicode để hiển thị hoàn hảo các ký tự phức tạp (Ầ, ầ, Ắ, ắ...).

## 📋 Tính năng nổi bật (v4.0.0)

- **U8g2 Graphics Engine**: Chuyển đổi toàn bộ hệ thống hiển thị sang thư viện U8g2, hỗ trợ hàng ngàn font chữ chất lượng cao và khả năng đồ họa mượt mà.
- **Dynamic Layout Engine**: Tự động tính toán tọa độ hiển thị dựa trên thông số (Ascent/Descent) của font chữ. Thay đổi font mà không bao giờ lo bị lệch dòng hay đè chữ.
- **Tối ưu hiển thị Tiêu đề**:
    - **Smart Wrapping**: Ngắt dòng thông minh tại dấu cách hoặc dấu câu để tận dụng tối đa chiều ngang.
    - **Pixel Ellipsis**: Sử dụng 3 điểm ảnh (1x1px) vẽ thủ công cực kỳ tinh tế thay cho dấu `...` thô kệch của font.
- **AI Intent Parsing**: Sử dụng `gpt-4o-mini` với kiến trúc 2 bước giúp phân loại ý định cực nhanh và trích xuất chi tiết cực kỳ chính xác.
- **Đồng bộ thời gian thông minh**: Lấy giờ chuẩn từ Telegram API Header, đảm bảo thiết bị luôn chạy đúng giờ ngay cả khi SNTP gặp sự cố.
- **Micro-Optimization RAM**: Hệ thống quản lý bộ nhớ động giúp firmware chạy cực kỳ ổn định trên ESP32-C3 chỉ với ~320KB RAM.
- **Captive Portal WiFi Setup**: Tự động phát Wifi AP để cấu hình mạng qua điện thoại cực kỳ tiện lợi.

---

## 🔧 Phân bổ phần cứng

### 1. Linh kiện
- **MCU**: ESP32-C3 Super Mini (RISC-V).
- **Display**: OLED 0.96 inch (SSD1306 qua U8g2 HAL).
- **Communication**: WiFi 2.4GHz.

### 2. Sơ đồ nối dây (Wiring)

| OLED Pin | ESP32-C3 Pin | Chức năng |
|----------|--------------|-----------|
| **VCC**  | 3V3          | Nguồn |
| **GND**  | GND          | Đất |
| **SDA**  | **GPIO 6**   | I2C Data |
| **SCL**  | **GPIO 7**   | I2C Clock |

---

## ⚙️ Cài đặt & Flash Firmware

### Bước 1: Cấu hình hệ thống
1. Sao chép `main/config.h.example` thành `main/config.h`.
2. Điền API Keys của OpenAI, Telegram, Firebase.
3. Tinh chỉnh giao diện trong `main/display/display_config.h` (Fonts, Margins, Spacings).

### Bước 2: Build & Flash
```bash
# Sử dụng ESP-IDF v5.x
idf.py build
idf.py flash monitor
```

---

## 🚀 Hướng dẫn sử dụng

### 1. Qua Telegram
- **Thêm task**: "Nhắc tôi họp báo lúc 2h chiều nay"
- **Hoàn thành**: "Xong việc báo cáo rồi" -> Hệ thống tự dời hạn nếu là task lặp lại.
- **Tìm kiếm**: "Tháng này có lịch nào quan trọng không?"
- **Hoàn tác**: Sử dụng nút **[Undo]** ngay trên tin nhắn phản hồi.

### 2. Qua thiết bị (OLED)
- **Màn hình Carousel**: Tự động cuộn qua các task trong 3 ngày tới.
- **Nút vật lý (BOOT)**: Nhấn để chuyển trang thủ công, Giữ 5 giây để Reset WiFi.

---

## 📁 Cấu trúc thư mục
- `main/actions/`: Dispatcher và logic xử lý AI.
- `main/display/`: Quản lý U8g2, cấu hình font và engine vẽ custom.
- `main/database/`: SPIFFS CRUD & Vector Hybrid Search (Lexical + Semantic).
- `main/utils/`: Time utils, Formatter và Web Log Server.

---

## 📝 Giấy phép & Tác giả
Dự án được phát hành dưới giấy phép **MIT**.

**Phiên bản:** 4.1.0  
**Tác giả:** chiconghvan  
**Cập nhật cuối:** 2026-03-16 (U8g2 Font Suite, 30 FPS Sliding & Baseline alignment fix)
