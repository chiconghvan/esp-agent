/**
 * ===========================================================================
 * @file ssd1306.c
 * @brief Driver SSD1306 OLED 128×64 qua I2C cho ESP-IDF
 * ===========================================================================
 */

#include "config.h"
#include "ssd1306.h"
#include "font6x8.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ssd1306";

#define I2C_NUM         I2C_NUM_0
#define I2C_FREQ_HZ     400000
#define SSD1306_ADDR    0x3C

/* Frame buffer */
static uint8_t s_fb[SSD1306_BUF_SIZE];

/* --------------------------------------------------------------------------
 * Gửi command byte tới SSD1306
 * -------------------------------------------------------------------------- */
static esp_err_t ssd1306_cmd(uint8_t cmd)
{
    uint8_t buf[2] = { 0x00, cmd };  /* Co=0, D/C#=0 → command */
    return i2c_master_write_to_device(I2C_NUM, SSD1306_ADDR, buf, 2, pdMS_TO_TICKS(100));
}

/* --------------------------------------------------------------------------
 * Khởi tạo I2C bus + SSD1306
 * -------------------------------------------------------------------------- */
esp_err_t ssd1306_init(int sda_gpio, int scl_gpio)
{
    /* Cấu hình I2C master */
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };

    esp_err_t err = i2c_param_config(I2C_NUM, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2c_driver_install(I2C_NUM, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C install failed: %s", esp_err_to_name(err));
        return err;
    }

    /* SSD1306 init sequence */
    static const uint8_t init_cmds[] = {
        0xAE,       /* Display OFF */
        0xD5, 0x80, /* Clock divide ratio */
        0xA8, 0x3F, /* Multiplex ratio (64-1) */
        0xD3, 0x00, /* Display offset = 0 */
        0x40,       /* Start line = 0 */
        0x8D, 0x14, /* Charge pump ON (internal VCC) */
        0x20, 0x00, /* Memory addressing: horizontal */
        0xA1,       /* Segment remap (flip horizontal) */
        0xC8,       /* COM scan direction (flip vertical) */
        0xDA, 0x12, /* COM pins config */
        0x81, 0xCF, /* Contrast */
        0xD9, 0xF1, /* Pre-charge period */
        0xDB, 0x40, /* VCOMH deselect level */
        0xA4,       /* Display from RAM */
        0xA6,       /* Normal display (not inverted) */
        0xAF,       /* Display ON */
    };

    for (size_t i = 0; i < sizeof(init_cmds); i++) {
        err = ssd1306_cmd(init_cmds[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SSD1306 init cmd[%d] failed", (int)i);
            return err;
        }
    }

    ssd1306_clear();
    ssd1306_update();

    ESP_LOGI(TAG, "SSD1306 128x64 init OK (SDA=%d, SCL=%d)", sda_gpio, scl_gpio);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Xóa frame buffer
 * -------------------------------------------------------------------------- */
void ssd1306_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

/* --------------------------------------------------------------------------
 * Gửi frame buffer ra SSD1306
 * -------------------------------------------------------------------------- */
void ssd1306_update(void)
{
    /* Set column address 0..127 */
    ssd1306_cmd(0x21);
    ssd1306_cmd(0x00);
    ssd1306_cmd(0x7F);

    /* Set page address 0..7 */
    ssd1306_cmd(0x22);
    ssd1306_cmd(0x00);
    ssd1306_cmd(0x07);

    /* Gửi data (prefix 0x40 = Co=0, D/C#=1 → data) */
    uint8_t buf[SSD1306_BUF_SIZE + 1];
    buf[0] = 0x40;
    memcpy(buf + 1, s_fb, SSD1306_BUF_SIZE);
    i2c_master_write_to_device(I2C_NUM, SSD1306_ADDR, buf, sizeof(buf), pdMS_TO_TICKS(200));
}

/* --------------------------------------------------------------------------
 * Vẽ 1 pixel
 * -------------------------------------------------------------------------- */
void ssd1306_draw_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= SSD1306_WIDTH || y < 0 || y >= SSD1306_HEIGHT) return;
    int page = y / 8;
    int bit = y % 8;
    int idx = page * SSD1306_WIDTH + x;

    if (on) {
        s_fb[idx] |= (1 << bit);
    } else {
        s_fb[idx] &= ~(1 << bit);
    }
}

/* --------------------------------------------------------------------------
 * Vẽ ký tự
 * -------------------------------------------------------------------------- */
void ssd1306_draw_char(int x, int y, char c, bool inverted)
{
    uint8_t idx = (uint8_t)c;

    for (int col = 0; col < 6; col++) {
        uint8_t line = font6x8[idx][col];
        
        for (int row = 0; row < 8; row++) {
            bool pixel = (line >> row) & 1;
            if (inverted) pixel = !pixel;
            ssd1306_draw_pixel(x + col, y + row, pixel);
        }
    }
}

/* --------------------------------------------------------------------------
 * Vẽ chuỗi
 * -------------------------------------------------------------------------- */
void ssd1306_draw_string(int x, int y, const char *str, bool inverted)
{
    int cx = x;
    while (*str) {
        char c = *str;
        int char_w = 6;
        if (c == ' ') char_w = 3;
        else if (c == '.' || c == ',' || c == ':' || c == ';') char_w = 4;
        
        if (cx + char_w > SSD1306_WIDTH) break;
        
        ssd1306_draw_char(cx, y, c, inverted);
        cx += char_w;
        str++;
    }
}


/* --------------------------------------------------------------------------
 * Vẽ Wi-Fi Icon (11x8) theo cường độ tín hiệu
 * -------------------------------------------------------------------------- */
void ssd1306_draw_wifi_icon(int x, int y, int level, bool inverted)
{
    /* 
     * Thiết kế mới: 3 cột (bars) có độ cao tăng dần, không khung.
     * Cột 1: rộng 2px, cao 4px.
     * Cột 2: rộng 2px, cao 6px.
     * Cột 3: rộng 2px, cao 8px.
     * Cách nhau 1px.
     */
    
    uint8_t bmp[11] = { 0 };
    
    // Bar 1 (cao 4px: bits 4-7) - Căn lề dưới
    uint8_t h4 = 0xF0;
    // Bar 2 (cao 6px: bits 2-7) - Căn lề dưới
    uint8_t h6 = 0xFC;
    // Bar 3 (cao 8px: bits 0-7) - Toàn bộ
    uint8_t h8 = 0xFF;

    if (level >= 1) {
        bmp[2] = h4; bmp[3] = h4;
    }
    if (level >= 2) {
        bmp[5] = h6; bmp[6] = h6;
    }
    if (level >= 3) {
        bmp[8] = h8; bmp[9] = h8;
    }
    // Level 0: Không vẽ gì (hoặc có thể vẽ bar 1 mờ nếu muốn)

    for (int col = 0; col < 11; col++) {
        uint8_t line = bmp[col];
        for (int row = 0; row < 8; row++) {
            bool pixel = (line >> row) & 1;
            if (inverted) pixel = !pixel;
            ssd1306_draw_pixel(x + col, y + row, pixel);
        }
    }
}



/* --------------------------------------------------------------------------
 * Vẽ thanh ngang inverted
 * -------------------------------------------------------------------------- */
void ssd1306_draw_inverted_bar(int page_y, const char *text)
{
    ssd1306_fill_rect(0, page_y, SSD1306_WIDTH, 10, true);
    ssd1306_draw_string(2, page_y + 1, text, true);
}
void ssd1306_draw_string_wrapped(int x, int y, int max_width, const char *str, bool inverted)
{
    int cx = x;
    int cy = y;
    const char *ptr = str;

    while (*ptr) {
        /* Bỏ qua khoảng trắng ở đầu dòng */
        if (cx == x && *ptr == ' ') {
            ptr++;
            continue;
        }

        /* Tìm từ tiếp theo (đến dấu cách hoặc kết thúc chuỗi) */
        const char *word_end = ptr;
        while (*word_end && *word_end != ' ') {
            word_end++;
        }

        int word_len = word_end - ptr;
        int word_width_px = word_len * 6;

        /* Nếu từ không vừa dòng hiện tại (và không phải từ quá dài > max_width) */
        if (cx + word_width_px > max_width && word_width_px <= (max_width - x)) {
            cx = x;
            cy += 9;
            if (cy + 8 > SSD1306_HEIGHT) break;
            
            /* Sau khi xuống dòng, bỏ qua khoảng trắng nếu có */
            if (*ptr == ' ') {
                ptr++;
                continue;
            }
        }

        /* Vẽ từng ký tự của từ */
        while (ptr < word_end) {
            if (cx + 6 > max_width) {
                cx = x;
                cy += 9;
                if (cy + 8 > SSD1306_HEIGHT) break;
            }
            ssd1306_draw_char(cx, cy, *ptr, inverted);
            cx += 6;
            ptr++;
        }

        /* Vẽ dấu cách sau từ nếu còn chuỗi */
        if (*ptr == ' ') {
            if (cx + 6 <= max_width) {
                ssd1306_draw_char(cx, cy, ' ', inverted);
                cx += 6;
            } else {
                /* Dấu cách ở cuối dòng -> tự động xuống dòng cho từ sau */
                cx = x;
                cy += 9;
                if (cy + 8 > SSD1306_HEIGHT) break;
            }
            ptr++;
        }
    }
}


/* --------------------------------------------------------------------------
 * Vẽ đường kẻ ngang
 * -------------------------------------------------------------------------- */
void ssd1306_draw_hline(int x, int y, int width)
{
    for (int i = 0; i < width; i++) {
        ssd1306_draw_pixel(x + i, y, true);
    }
}

/* --------------------------------------------------------------------------
 * Vẽ thanh tiến trình
 * -------------------------------------------------------------------------- */
void ssd1306_draw_progress(int x, int y, int width, int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    /* Khung ngoài */
    ssd1306_draw_hline(x, y, width);
    ssd1306_draw_hline(x, y + 7, width);
    for (int row = 0; row < 8; row++) {
        ssd1306_draw_pixel(x, y + row, true);
        ssd1306_draw_pixel(x + width - 1, y + row, true);
    }

    /* Fill bên trong */
    int fill_w = (width - 4) * percent / 100;
    for (int row = y + 2; row < y + 6; row++) {
        for (int col = x + 2; col < x + 2 + fill_w; col++) {
            ssd1306_draw_pixel(col, row, true);
        }
    }
}

/* --------------------------------------------------------------------------
 * Fill rectangle
 * -------------------------------------------------------------------------- */
void ssd1306_fill_rect(int x, int y, int w, int h, bool on)
{
    for (int row = y; row < y + h; row++) {
        for (int col = x; col < x + w; col++) {
            ssd1306_draw_pixel(col, row, on);
        }
    }
}

/* --------------------------------------------------------------------------
 * Đảo ngược display (hardware invert)
 * -------------------------------------------------------------------------- */
void ssd1306_invert_display(bool invert)
{
    ssd1306_cmd(invert ? 0xA7 : 0xA6);
}
