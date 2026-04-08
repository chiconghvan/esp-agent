/**
 * ===========================================================================
 * @file display_config.h
 * @brief User Interface Configuration for OLED
 * ===========================================================================
 */

#ifndef DISPLAY_CONFIG_H
#define DISPLAY_CONFIG_H

/* ---- Extern Custom Fonts ---- */
extern const uint8_t u8g2_font_cascadiacode_light_11[];
extern const uint8_t u8g2_font_iosevkacharonmono_light_9[];
extern const uint8_t u8g2_font_iosevkacharonmono_light_10[];
extern const uint8_t u8g2_font_iosevkacharonmono_light_11[];
extern const uint8_t u8g2_font_iosevkacharonmono_light_13[];

/* ---- Font Configuration (Granular) ---- */
// Danh sách font: https://github.com/olikraus/u8g2/wiki/fntlistall
#define DISP_FONT_HEADER_LABEL  u8g2_font_cascadiacode_light_11  // For "Deadline"/"Today"
#define DISP_FONT_HEADER_TIME   u8g2_font_cascadiacode_light_11  // For clock
#define DISP_FONT_TASK_ID       u8g2_font_6x13B_mr  // For #ID
#define DISP_FONT_TASK_TITLE    u8g2_font_iosevkacharonmono_light_13     // Task content
#define DISP_FONT_TASK_DUE      u8g2_font_iosevkacharonmono_light_13     // Task content Due

/* ---- Custom Char Widths ---- */
#define DISP_WIDTH_SPACE        3                   // Space width (px)
#define DISP_WIDTH_PUNCTUATION  6                   // Punctuation width : , . ' (px)

/* ---- Bar Size & Position ---- */
#define DISP_HEADER_HEIGHT      10                  // Status bar height
#define DISP_HEADER_Y_OFFSET    9                   // Baseline for text in bar (Result: 1px gap at row 9)

/* ---- Spacings ---- */
#define DISP_MARGIN_TOP         5                   // From Bar to Line 1 (Reduced for 14pt)
#define DISP_LINE_SPACING       15                  // Between Title lines (Increased for 14pt)
#define DISP_MARGIN_DUE         7                   // From Title to Due Time
#define DISP_MARGIN_BOTTOM      1                   // From Due Time to Page Indicator

#define DISP_X_PADDING          0                   // Left margin (pixel)

/* ---- Effects ---- */
#define DISP_ENABLE_ANIMATION   0                   // 1: Enable sliding, 0: Direct

#endif /* DISPLAY_CONFIG_H */
