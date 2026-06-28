#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

/* 屏幕切换开关 — 编译时修改
 *   0 = 内置 ST7789 屏幕 (240x135 横屏)
 *   1 = 外接 ILI9341 2.8" 屏幕 (320x240 横屏, setRotation(3)) */
#define SCR_USE_EXTERNAL  1

/* ============ 字号与行高（内外屏统一）============ */
#define FONT_SIZE_PX      12
#define LINE_HEIGHT       13
#define ASCII_CHAR_W      9    // M5GFX 6x8 × 1.5 缩放后 9px
#define MARGIN_X          2

/* ============ 屏幕尺寸与布局（按 SCR_USE_EXTERNAL 选择）============ */
#if SCR_USE_EXTERNAL
    /* 外屏 ILI9341 横屏 setRotation(3) — 实际 W=320 H=240 */
    #define SCREEN_W         320
    #define SCREEN_H         240
    /* 内容区: 240 - 13(状态栏) - 13(底部 hint) = 214, /13 = 16.46 → 16 行 */
    #define CONTENT_LINES    16
    #define TOTAL_LINES      17    // CONTENT_LINES + 1(状态栏)
    /* PSRAM 单次连续分配上限 128KB; 16*25*256 = 100KB, 留余量 */
    #define MAX_PAGES        25
    #define SCREEN_ROTATION  3
#else
    /* 内屏 ST7789 Cardputer 横屏 setRotation(1) */
    #define SCREEN_W         240
    #define SCREEN_H         135
    #define CONTENT_LINES    9
    #define TOTAL_LINES      10
    /* 9*50*256 = 112.5KB < 128KB 单次分配上限 */
    #define MAX_PAGES        50
    #define SCREEN_ROTATION  1
#endif

#define STATUS_BAR_Y   0
#define CONTENT_Y      LINE_HEIGHT
#define HINT_Y         (SCREEN_H - LINE_HEIGHT)

#endif // DEVICE_CONFIG_H
