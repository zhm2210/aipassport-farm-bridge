// TFT_eSPI User_Setup.h — FoloToy AI Passport (ESP32-C3 + ST7789P3 240x320)
// 用法：把本文件复制到 Arduino/libraries/TFT_eSPI/User_Setup.h（覆盖原文件）
#define USER_SETUP_INFO "FoloToy AI Passport ESP32-C3"

#define ST7789_DRIVER
#define TFT_WIDTH  240
#define TFT_HEIGHT 320

// ---- 引脚（FoloTy AI Passport 实测）----
#define TFT_CS    1    // GPIO1  LCD CS
#define TFT_DC    20   // GPIO20 LCD DC
#define TFT_SCLK  8    // GPIO8  LCD SCLK (SPI2)
#define TFT_MOSI  9    // GPIO9  LCD MOSI
#define TFT_RST  -1    // 未接 MCU，用软件复位
#define TFT_BL   21    // GPIO21 背光 PWM（LEDC 5kHz/10bit）
#define TFT_BACKLIGHT_ON HIGH

// ---- SPI ----
#define SPI_FREQUENCY        40000000   // 40MHz mode 0
#define SPI_READ_FREQUENCY   20000000
#define SPI_TOUCH_FREQUENCY  2500000

// ---- 颜色 ----
// ★ FoloToy AI Passport 的 ST7789P3 是 BGR 序（青会显示成橙），
//   TFT_eSPI 里 TFT_BGR=0 / TFT_RGB=1，必须写 TFT_BGR 才正确
#define TFT_RGB_ORDER TFT_BGR

// ---- 字体 ----
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT
