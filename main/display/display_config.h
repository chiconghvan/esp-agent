/**
 * ===========================================================================
 * @file display_config.h
 * @brief Cấu hình giao diện người dùng cho màn hình OLED
 * ===========================================================================
 */

#ifndef DISPLAY_CONFIG_H
#define DISPLAY_CONFIG_H

/* ──── Cấu hình Font (Granular) ──── */
// Danh sách font: https://github.com/olikraus/u8g2/wiki/fntlistall
#define DISP_FONT_HEADER_LABEL  u8g2_font_t0_11_mr  // Cho chữ "Deadline"/"Today"
#define DISP_FONT_HEADER_TIME   u8g2_font_lucasfont_alternate_tf  // Cho đồng hồ
#define DISP_FONT_TASK_ID       u8g2_font_6x13B_mr  // Cho ký hiệu #ID
#define DISP_FONT_TASK_TITLE    u8g2_font_7x14_mr  // Cho nội dung tiêu đề
#define DISP_FONT_TASK_DUE      u8g2_font_7x14_mr  // Cho thời hạn (Due time)

/* ──── Cấu hình Chiều rộng Ký tự Tùy chỉnh (Force Width) ──── */
#define DISP_WIDTH_SPACE        3                   // Khoảng cách phím cách (px)
#define DISP_WIDTH_PUNCTUATION  6                   // Khoảng cách cho các dấu : , . ' (px)

/* ──── Kích thước Bar & Vị trí ──── */
#define DISP_HEADER_HEIGHT      11                  // Độ cao thanh trạng thái (pixel)
#define DISP_HEADER_Y_OFFSET    10                   // Baseline cho text trong thanh trạng thái

/* ──── Cấu hình Khoảng cách (Spacings) ──── */
#define DISP_MARGIN_TOP         4                   // Từ Bar xuống dòng 1
#define DISP_LINE_SPACING       15                  // Dãn dòng giữa 2 dòng Title
#define DISP_MARGIN_DUE         6                   // Từ Title xuống Due Time
#define DISP_MARGIN_BOTTOM      3                   // Từ Due Time xuống Page Indicator (chấm tròn)

#define DISP_X_PADDING          2                   // Lề trái màn hình (pixel)

/* ──── Hiệu ứng ──── */
#define DISP_ENABLE_ANIMATION   0                   // 1: Bật hiệu ứng trượt, 0: Chuyển trang trực tiếp

#endif /* DISPLAY_CONFIG_H */
