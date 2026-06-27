#ifndef COLOR_CONFIG_H
#define COLOR_CONFIG_H

#include <Arduino.h>

// ============ 屏幕布局 ============
#define SCREEN_W      240
#define SCREEN_H      135
#define LINE_HEIGHT   13       // 12px 字 + 1px 行距
#define TOTAL_LINES   10       // 含状态栏
#define CONTENT_LINES 9        // 内容区
#define MARGIN_X      2
#define STATUS_BAR_Y  0
#define CONTENT_Y     LINE_HEIGHT

// ============ 颜色（RGB565）============
// 背景与前景
#define COLOR_BG       ((uint16_t)0x0000)  // 黑
#define COLOR_DEFAULT  ((uint16_t)0xFFFF)  // 默认前景白

// 标记颜色（与 dict.bin 内容字节对应）
#define COLOR_HEADWORD  ((uint16_t)0xFFE0)  // 亮黄
#define COLOR_POS       ((uint16_t)0x07FF)  // 青
#define COLOR_PHONETIC  ((uint16_t)0xFD20)  // 橙
#define COLOR_DEF       ((uint16_t)0xFFFF)  // 白
#define COLOR_EXAMPLE   ((uint16_t)0xC618)  // 灰
#define COLOR_NUM       ((uint16_t)0x07E0)  // 绿
#define COLOR_XREF      ((uint16_t)0x9D7C)  // 淡灰

// 状态栏
#define COLOR_INPUT     ((uint16_t)0xFFE0)  // 输入框（亮黄）
#define COLOR_BAT_OK    ((uint16_t)0x07E0)  // 电量正常（绿）
#define COLOR_BAT_LOW   ((uint16_t)0xF800)  // 电量低（红）
#define COLOR_SD_BAD    ((uint16_t)0xF800)  // SD 失败（红）
#define COLOR_MODE_ALL  ((uint16_t)0xFFFF)  // 全部模式（白）
#define COLOR_MODE_EN   ((uint16_t)0x07FF)  // 英文模式（青）
#define COLOR_MODE_CN   ((uint16_t)0xFD20)  // 中文模式（橙）

// ============ 标记字节（与 dict.bin 内容流对应）============
#define TAG_NEWLINE   0x00
#define TAG_HEADWORD  0x01
#define TAG_POS       0x02
#define TAG_PHONETIC   0x03
#define TAG_DEF       0x04
#define TAG_EXAMPLE   0x05
#define TAG_NUM       0x06
#define TAG_XREF      0x07
#define TAG_AUDIO_UK  0x08
#define TAG_AUDIO_US  0x09
// 0x0A-0x0F 保留

// 段落类型标记
#define TAG_PARA_EN   0x10
#define TAG_PARA_CN   0x11
#define TAG_PARA_HEAD 0x12

// ============ 显示模式 ============
enum DisplayMode : uint8_t {
    MODE_ALL = 0,
    MODE_EN_ONLY,
    MODE_CN_ONLY
};

inline uint16_t tagToColor(uint8_t tag) {
    switch (tag) {
        case TAG_HEADWORD: return COLOR_HEADWORD;
        case TAG_POS:      return COLOR_POS;
        case TAG_PHONETIC: return COLOR_PHONETIC;
        case TAG_DEF:      return COLOR_DEF;
        case TAG_EXAMPLE:  return COLOR_EXAMPLE;
        case TAG_NUM:      return COLOR_NUM;
        case TAG_XREF:     return COLOR_XREF;
        default:           return COLOR_DEFAULT;
    }
}

// 段落是否在当前模式下显示
inline bool paraVisible(uint8_t paraTag, DisplayMode mode) {
    if (paraTag == TAG_PARA_HEAD) return true;
    if (mode == MODE_ALL) return true;
    if (mode == MODE_EN_ONLY) return paraTag == TAG_PARA_EN;
    if (mode == MODE_CN_ONLY) return paraTag == TAG_PARA_CN;
    return true;
}

inline const char* modeLabel(DisplayMode mode) {
    if (mode == MODE_EN_ONLY) return "EN";
    if (mode == MODE_CN_ONLY) return "CN";
    return "A";
}

inline uint16_t modeColor(DisplayMode mode) {
    if (mode == MODE_EN_ONLY) return COLOR_MODE_EN;
    if (mode == MODE_CN_ONLY) return COLOR_MODE_CN;
    return COLOR_MODE_ALL;
}

#endif // COLOR_CONFIG_H