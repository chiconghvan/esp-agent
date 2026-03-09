/**
 * ===========================================================================
 * @file ssd1306.h
 * @brief Driver cho OLED SSD1306 128x64 qua I2C
 * ===========================================================================
 */

#ifndef SSD1306_H
#define SSD1306_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#define SSD1306_WIDTH       128
#define SSD1306_HEIGHT      64
#define SSD1306_PAGES       (SSD1306_HEIGHT / 8)
#define SSD1306_BUF_SIZE    (SSD1306_WIDTH * SSD1306_PAGES)

/**
 * @brief Khởi tạo SSD1306 qua I2C
 * @param sda_gpio Chân SDA
 * @param scl_gpio Chân SCL
 * @return ESP_OK nếu thành công
 */
esp_err_t ssd1306_init(int sda_gpio, int scl_gpio);

/** Xóa toàn bộ frame buffer */
void ssd1306_clear(void);

/** Gửi frame buffer ra màn hình */
void ssd1306_update(void);

/** Vẽ 1 pixel */
void ssd1306_draw_pixel(int x, int y, bool on);

/** Vẽ ký tự ASCII tại (x, y), font 5×7 */
void ssd1306_draw_char(int x, int y, char c, bool inverted);

/** Vẽ chuỗi tại (x, y) */
void ssd1306_draw_string(int x, int y, const char *str, bool inverted);
void ssd1306_draw_string_wrapped(int x, int y, int max_width, const char *str, bool inverted);

/** Vẽ font 6x8 (cho thử nghiệm) */
void ssd1306_draw_char_6x8(int x, int y, char c, bool inverted);
void ssd1306_draw_string_6x8(int x, int y, const char *str, bool inverted);

/** Vẽ Wifi Icon 11x8 */
void ssd1306_draw_wifi_icon(int x, int y, int level, bool inverted);

/** Vẽ font 4x6 (cho thử nghiệm) */
void ssd1306_draw_char_4x6(int x, int y, char c, bool inverted);
void ssd1306_draw_string_4x6(int x, int y, const char *str, bool inverted);

/** Vẽ thanh ngang inverted (nền đen full width, chữ trắng) */
void ssd1306_draw_inverted_bar(int page_y, const char *text);

/** Vẽ đường kẻ ngang */
void ssd1306_draw_hline(int x, int y, int width);

/** Vẽ thanh tiến trình */
void ssd1306_draw_progress(int x, int y, int width, int percent);

/** Fill 1 vùng chữ nhật */
void ssd1306_fill_rect(int x, int y, int w, int h, bool on);

/** Đảo ngược toàn bộ màn hình (dùng cho flash effect) */
void ssd1306_invert_display(bool invert);

#endif /* SSD1306_H */
