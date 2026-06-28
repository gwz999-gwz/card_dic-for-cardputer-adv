#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>
#include "font_loader.h"
#include "color_config.h"

namespace lgfx { inline namespace v1 { class LGFX_Device; } }

#define MAX_LINES_PER_PAGE CONTENT_LINES
#define MAX_LINE_BYTES     256
// MAX_PAGES 在 config.h 中按 SCR_USE_EXTERNAL 定义（内屏 50, 外屏 25）
#define MAX_WORDS          200   // 选词模式：单页最多提取的英文词数

struct RenderLine {
    uint16_t len;
    uint8_t  data[MAX_LINE_BYTES];
};

// 选词模式：单个英文词的屏幕位置 + 文本
// rangeCount=1 → 单行; =2 → 硬切跨两行(两端都画反色高亮,查词用合并后的 text)
struct WordRange {
    int lineIdx;    // 页内行号 0..CONTENT_LINES-1
    int x, xEnd;    // 像素坐标 [x, xEnd)
};
struct WordSpan {
    int rangeCount;
    WordRange ranges[2];
    int readingIdx;   // 阅读顺序索引 (lineIdx 升序, x 升序)
    int xCenter;      // 词中心 x 坐标(用 ;; 上下导航时算 x 距离)
    char text[64];    // 完整英文词(小写,用于查词)
};

class DisplayManager {
public:
    DisplayManager();
    ~DisplayManager();

    // 初始化：传入字体 + LGFX 设备指针(内屏 M5Cardputer.Display 或外屏 externalDisplay)
    void begin(FontLoader* font, lgfx::v1::LGFX_Device* lcd);
    void end();

    // 把 dict.bin 的 content 渲染为多行（应用段落过滤）
    // 返回总行数
    int layoutContent(const uint8_t* content, uint16_t contentLen,
                      DisplayMode mode = MODE_ALL);

    int  getPageCount() const { return _pageCount; }
    int  getPage()      const { return _currentPage; }
    void setPage(int p);
    void nextPage();
    void prevPage();

    // 渲染当前页（不画状态栏）
    // highlightWordIdx >= 0 时,把对应词的范围反色高亮
    void drawCurrentPage(int highlightWordIdx = -1);

    // 提取当前页所有英文词,按阅读顺序排序
    // 返回词数,存入内部 _words[]。每次换词/换页时重新调用
    int  extractWords();
    int  getWordCount() const { return _wordCount; }
    const WordSpan* getWord(int idx) const {
        return (idx >= 0 && idx < _wordCount) ? &_words[idx] : nullptr;
    }

    // 渲染状态栏
    //   isDetail=true  → 显示当前 headword（无 "> " 前缀）
    //   isDetail=false → 显示 "> 输入" 形式
    //   currentPage / pageCount：详情页多页时，在状态栏右侧显示 "n/m" 页码
    void drawStatusBar(const char* text, int batteryPct, bool sdOk,
                       DisplayMode mode, int currentPage, int pageCount,
                       bool isDetail = false);

    // 渲染底部提示行（按键说明）
    void drawHints(const char* hint);

    // 渲染开屏多行介绍(状态栏下方开始往下铺 n 行,行高 LINE_HEIGHT)
    // 用于首次启动或搜索页的欢迎/按键说明
    void drawIntroScreen(const char* const lines[], int n);

    // 清除底部提示行（用于首次按键后永久隐藏）
    void clearHints();

    void clearContentArea();

private:
    FontLoader* _font;
    lgfx::v1::LGFX_Device* _lcd;   // 指向当前 LGFX 设备（内屏/外屏）
    DisplayMode _mode;             // 当前显示模式（CN 模式下隐藏例句/变绿中文）
    // 全部页面摊平（最大 32 页 × 9 行 = 288 行 × 256B ≈ 72KB）
    // PSRAM 动态分配：释放 72KB 常规 SRAM；PSRAM 顺序读性能足够 layoutContent 用
    RenderLine* _lines;
    int         _lineCap;          // _lines 容量（行数）
    int         _totalLines;       // layoutContent 后的总行数
    int         _pageCount;
    int         _currentPage;

    // 选词模式:当前页英文词列表(extractWords 填充)
    WordSpan    _words[MAX_WORDS];
    int         _wordCount;

    // 单字节：宽度估计（无字体时也工作）
    int charWidth(uint8_t b);
    int utf8CharWidth(const uint8_t* p);

    // ASCII 字符渲染宽度：在 config.h 中定义为 ASCII_CHAR_W=9
    // (M5GFX 6×8 × 1.5 缩放 = 9px 渲染宽, 字符刚好相接)

    // 内部：把一个字符写入到 line 缓冲（处理换行/缓冲满）
    bool putChar(RenderLine& line, int& usedWidth, uint8_t b);
    bool putUtf8Char(RenderLine& line, int& usedWidth, const uint8_t*& p,
                     const uint8_t* end);

    // 在指定 line 渲染一行（用字体）
    // hlStart/hlEnd: 高亮 x 范围 [hlStart, hlEnd),hlStart<0 不高亮
    void renderLine(int lineIdx, int y, int hlStart = -1, int hlEnd = -1);

    // 从 (x, y) 开始渲染 UTF-8 字符串(中英混排): ASCII 走内置字体,CJK 走 font.bin
    // hlStart/hlEnd: 高亮 x 范围(同 renderLine), -1=不高亮
    // 返回最终 x 位置
    int drawMixedString(int x, int y, const char* str, uint16_t color,
                        int hlStart = -1, int hlEnd = -1);
};

#endif // DISPLAY_H