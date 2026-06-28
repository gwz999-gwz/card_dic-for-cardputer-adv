/**
 * Cardputer 电子辞典 — 主程序
 *
 * 功能：
 *   - 输入查询（中英文 12px 混合显示）
 *   - 模糊匹配（前缀/包含，最多 8 候选）
 *   - 方向键导航（;,./ = 上/下/左/右）
 *   - 分页浏览长释义
 *   - 显示模式切换（Alt: 全部 → 只中文 → 只英文）
 *   - 状态栏（电量 / 模式 / 页码）
 *   - 通过 config.h 中 SCR_USE_EXTERNAL 切换内屏(ST7789)/外屏(ILI9341)
 *
 * 数据文件（SD 卡 /dictcard/）：
 *   - dict.bin     词典主体
 *   - dict.idx     bucket 索引
 *   - font.bin     CJF1 字体
 */

#include <Arduino.h>
#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>
#include <M5GFX.h>
#include "config.h"           // SCR_USE_EXTERNAL, SCREEN_W/H, CONTENT_LINES, MAX_PAGES...
#include "font_loader.h"
#include "dict_engine.h"
#include "display.h"
#include "color_config.h"

#if SCR_USE_EXTERNAL
  #include "external_display/LGFX_ILI9341.h"
#endif

#define SD_CS    12
#if SCR_USE_EXTERNAL
// 共享 SPI3_HOST (HSPI): 与外屏 ILI9341 和内置 ST7789 共用一条总线
SPIClass sdSPI(HSPI);
#endif
#define BIN_PATH "/dictcard/dict.bin"
#define IDX_PATH "/dictcard/dict.idx"
#define FONT_PATH "/dictcard/font.bin"
#define HIST_PATH "/dictcard/history.dat"
#define FAV_PATH  "/dictcard/favorites.dat"

#define INPUT_MAX 32
#define PAGE_HINTS_DETAIL ";,.:翻页  ,/:候选  `:清空  Fn+F:收藏"

// 开屏介绍(搜索页首次启动显示,按任意键后永久隐藏)
// 只列已实现的功能:输入/查询/翻页/选词跳查/显示模式
// 未实现的不要写:历史记录(Fn+H)、收藏(Fn+F/L)、单词读音
static const char* const INTRO_LINES[] = {
    "Cardputer 词典",
    "英汉离线词典",
    "",
    "字母键 输入 · Enter 查词",
    "` 清空 · Alt 切换显示模式",
    "",
    "详情页: ;, . 翻页 / , / 前后词",
    "Ctrl 选词跳查",
};
#define INTRO_LINE_COUNT (sizeof(INTRO_LINES) / sizeof(INTRO_LINES[0]))

// ============ 全局对象 ============
FontLoader    g_font;
DictEngine    g_dict;
DisplayManager g_disp;

// 全局 LGFX 指针: 指向当前使用的屏幕
//  内屏 → &M5Cardputer.Display
//  外屏 → &externalDisplay (定义在 external_display/LGFX_ILI9341.cpp)
lgfx::v1::LGFX_Device* g_lcd = nullptr;

// ============ 状态 ============
struct AppState {
    char input[INPUT_MAX + 1];
    int  inputLen;
    Candidate cands[MAX_CANDIDATES];
    int  candCount;
    int  candIndex;
    bool showingCands;
    bool showingDetail;
    uint8_t* detailBuf;
    uint16_t detailLen;
    uint32_t detailOffset;
    DisplayMode mode;
    int  batteryPct;
    bool sdOk;
    bool hintsVisible;   // 首次输入/查询后隐藏提示行
    // 选词跳查 (Ctrl): 仅在详情页有效
    bool wordSelMode;    // 是否在选词模式
    int  wordSelIdx;     // 当前选中的词在 _words[] 中的 index
} g_st;

// ============ 工具 ============
// 把 src 截断到 maxChars 以内，超出部分用 "..." 替代
// 用于候选列表避免超长词溢出到下一行
static void truncateForDisplay(char* dest, const char* src, int maxChars) {
    int srcLen = strlen(src);
    if (srcLen <= maxChars) {
        strcpy(dest, src);
    } else {
        if (maxChars < 3) maxChars = 3;
        strncpy(dest, src, maxChars - 3);
        dest[maxChars - 3] = '\0';
        strcat(dest, "...");
    }
}

// 按屏幕宽度算候选词最大字符数(行字符数 - "N. " 前缀 3 字符)
//  内屏 240/9 - 3 ≈ 23 字符, 外屏 320/9 - 3 ≈ 32 字符
static inline int candMaxChars() {
    return (SCREEN_W - 2 * MARGIN_X) / ASCII_CHAR_W - 3;
}

void appendChar(char c) {
    if (g_st.inputLen < INPUT_MAX) {
        g_st.input[g_st.inputLen++] = c;
        g_st.input[g_st.inputLen] = '\0';
    }
}

void backspace() {
    if (g_st.inputLen > 0) {
        g_st.input[--g_st.inputLen] = '\0';
        g_st.showingCands = false;
        g_st.showingDetail = false;
    }
}

void clearInput() {
    g_st.inputLen = 0;
    g_st.input[0] = '\0';
    g_st.candCount = 0;
    g_st.candIndex = 0;
    g_st.showingCands = false;
    g_st.showingDetail = false;
    g_st.detailLen = 0;
    g_st.wordSelMode = false;
    g_st.wordSelIdx = 0;
}

// 读一次 ADC 电量。只有 >= 0 才更新 g_st.batteryPct
// 返回 -1 (ADC 未就绪) 时保留上次值，避免 -1 被错设成 0
static void readBattery() {
    int bat = M5Cardputer.Power.getBatteryLevel();
    if (bat >= 0) {
        if (bat > 100) bat = 100;
        g_st.batteryPct = bat;
    }
}

bool loadDetailForCandidate(int candIdx) {
    if (candIdx < 0 || candIdx >= g_st.candCount) return false;
    Candidate& c = g_st.cands[candIdx];
    if (g_st.detailBuf) { free(g_st.detailBuf); g_st.detailBuf = nullptr; }
    g_st.detailLen = c.len;
    g_st.detailBuf = (uint8_t*)malloc(c.len);
    if (!g_st.detailBuf) return false;
    uint16_t gotLen;
    if (!g_dict.lookupExact(c.key, g_st.detailBuf, gotLen, g_st.detailOffset)) {
        free(g_st.detailBuf);
        g_st.detailBuf = nullptr;
        return false;
    }
    g_disp.setPage(0);  // 新词从头开始
    return true;
}

// 加载词典中按字典序的上一条/下一条记录到详情页
bool loadAdjacentDetail(bool isPrev) {
    if (g_st.candCount == 0) return false;
    const char* curKey = g_st.cands[g_st.candIndex].key;

    char newKey[64];
    uint16_t newLen;
    uint32_t newOff;
    if (!g_dict.lookupAdjacentHeader(curKey, isPrev, newKey, sizeof(newKey),
                                     newLen, newOff)) {
        return false;
    }

    if (g_st.detailBuf) { free(g_st.detailBuf); g_st.detailBuf = nullptr; }
    g_st.detailBuf = (uint8_t*)malloc(newLen);
    if (!g_st.detailBuf) return false;
    if (!g_dict.readContentAt(newOff, g_st.detailBuf, newLen)) {
        free(g_st.detailBuf);
        g_st.detailBuf = nullptr;
        return false;
    }
    g_st.detailLen = newLen;
    g_st.detailOffset = newOff;

    strncpy(g_st.cands[0].key, newKey, sizeof(g_st.cands[0].key) - 1);
    g_st.cands[0].key[sizeof(g_st.cands[0].key) - 1] = '\0';
    g_st.cands[0].len = newLen;
    g_st.cands[0].offset = newOff;
    g_st.candCount = 1;
    g_st.candIndex = 0;

    g_st.showingDetail = true;
    g_st.showingCands = false;

    g_disp.setPage(0);
    return true;
}

void doQuery() {
    if (g_st.inputLen == 0) {
        g_st.candCount = 0;
        g_st.showingCands = false;
        g_st.showingDetail = false;
        return;
    }
    g_dict.lookupCandidates(g_st.input, g_st.cands, g_st.candCount, MAX_CANDIDATES);
    g_st.candIndex = 0;
    if (g_st.candCount >= 1) {
        g_st.showingCands = true;
        if (g_st.candCount == 1 && strcmp(g_st.cands[0].key, g_st.input) == 0) {
            loadDetailForCandidate(0);
            g_st.showingDetail = true;
            g_st.showingCands = false;
        }
    } else {
        g_st.showingCands = false;
        g_st.showingDetail = false;
    }
}

// ============ 选词跳查 ============
static int navPrevReading(int curIdx, int count) {
    if (count <= 0) return curIdx;
    if (count == 1) return 0;
    return (curIdx - 1 + count) % count;
}
static int navNextReading(int curIdx, int count) {
    if (count <= 0) return curIdx;
    if (count == 1) return 0;
    return (curIdx + 1) % count;
}
static int navPrevLine(int curIdx) {
    const WordSpan* cur = g_disp.getWord(curIdx);
    if (!cur) return curIdx;
    int curLine = cur->ranges[0].lineIdx;
    int curCenter = cur->xCenter;
    int bestIdx = -1;
    int bestDist = 1 << 30;
    for (int targetLine = curLine - 1; targetLine >= 0; targetLine--) {
        for (int i = 0; i < g_disp.getWordCount(); i++) {
            const WordSpan* w = g_disp.getWord(i);
            if (w->ranges[0].lineIdx != targetLine) continue;
            int d = abs(w->xCenter - curCenter);
            if (d < bestDist) {
                bestDist = d;
                bestIdx = i;
            }
        }
        if (bestIdx >= 0) return bestIdx;
    }
    return curIdx;
}
static int navNextLine(int curIdx) {
    const WordSpan* cur = g_disp.getWord(curIdx);
    if (!cur) return curIdx;
    int curLine = cur->ranges[0].lineIdx;
    int curCenter = cur->xCenter;
    int bestIdx = -1;
    int bestDist = 1 << 30;
    for (int targetLine = curLine + 1; targetLine < CONTENT_LINES; targetLine++) {
        for (int i = 0; i < g_disp.getWordCount(); i++) {
            const WordSpan* w = g_disp.getWord(i);
            if (w->ranges[0].lineIdx != targetLine) continue;
            int d = abs(w->xCenter - curCenter);
            if (d < bestDist) {
                bestDist = d;
                bestIdx = i;
            }
        }
        if (bestIdx >= 0) return bestIdx;
    }
    return curIdx;
}

static bool enterWordSelMode() {
    int n = g_disp.extractWords();
    if (n <= 0) return false;
    g_st.wordSelMode = true;
    g_st.wordSelIdx = 0;
    return true;
}

static void lookupSelectedWord() {
    const WordSpan* w = g_disp.getWord(g_st.wordSelIdx);
    if (!w) return;
    strncpy(g_st.input, w->text, INPUT_MAX);
    g_st.input[INPUT_MAX] = '\0';
    g_st.inputLen = strlen(g_st.input);
    g_st.wordSelMode = false;

    g_dict.lookupCandidates(g_st.input, g_st.cands, g_st.candCount, MAX_CANDIDATES);
    g_st.candIndex = 0;
    g_st.showingCands = true;
    g_st.showingDetail = false;
}

// ============ 渲染 ============
void renderAll() {
    if (!g_lcd) return;
    g_lcd->fillScreen(COLOR_BG);

    bool isDetailView = g_st.showingDetail && g_st.detailBuf
                        && g_st.candCount > 0;

    if (isDetailView) {
        g_disp.layoutContent(g_st.detailBuf, g_st.detailLen, g_st.mode);
        if (g_st.wordSelMode) {
            int n = g_disp.extractWords();
            if (n <= 0) {
                g_st.wordSelMode = false;
            } else if (g_st.wordSelIdx >= n) {
                g_st.wordSelIdx = 0;
            }
        }
    }

    const char* statusText;
    if (isDetailView) {
        statusText = g_st.cands[g_st.candIndex].key;
    } else {
        statusText = g_st.input;
    }
    g_disp.drawStatusBar(statusText, g_st.batteryPct,
                         g_st.sdOk, g_st.mode,
                         g_disp.getPage(), g_disp.getPageCount(),
                         isDetailView);

    if (isDetailView) {
        g_disp.drawCurrentPage(g_st.wordSelMode ? g_st.wordSelIdx : -1);
        if (g_st.hintsVisible) g_disp.drawHints(PAGE_HINTS_DETAIL);
    } else if (g_st.showingCands) {
        int n = g_st.candCount;
        int lines = (n < CONTENT_LINES) ? n : CONTENT_LINES;
        for (int i = 0; i < lines; i++) {
            int y = CONTENT_Y + i * LINE_HEIGHT;
            g_lcd->setTextSize(1.5f);
            uint16_t col = (i == g_st.candIndex) ? COLOR_HEADWORD : COLOR_DEFAULT;
            g_lcd->setTextColor(col, COLOR_BG);
            g_lcd->setCursor(MARGIN_X, y);
            // 候选词最大字符数按屏幕宽度动态算
            char word[64];
            truncateForDisplay(word, g_st.cands[i].key, candMaxChars());
            char buf[80];
            snprintf(buf, sizeof(buf), "%d. %s", i + 1, word);
            g_lcd->print(buf);
        }
        char hint[64];
        snprintf(hint, sizeof(hint), "%d 候选 (当前 %d)", n, g_st.candIndex + 1);
        if (g_st.hintsVisible) g_disp.drawHints(hint);
    } else {
        if (g_st.hintsVisible) {
            g_disp.drawIntroScreen(INTRO_LINES, INTRO_LINE_COUNT);
        } else {
            g_disp.clearContentArea();
        }
    }
}

// ============ 按键处理 ============
void processKey(const Keyboard_Class::KeysState& st) {
    if (g_st.hintsVisible) {
        g_st.hintsVisible = false;
        g_disp.clearHints();
    }

    if (st.ctrl && g_st.showingDetail) {
        if (g_st.wordSelMode) {
            g_st.wordSelMode = false;
        } else {
            enterWordSelMode();
        }
        return;
    }

    if (g_st.wordSelMode) {
        if (st.enter) {
            lookupSelectedWord();
            return;
        }
        for (auto hid : st.hid_keys) {
            int n = g_disp.getWordCount();
            if (n <= 0) {
                g_st.wordSelMode = false;
                return;
            }
            switch (hid) {
                case 0x33: g_st.wordSelIdx = navPrevLine(g_st.wordSelIdx); return;
                case 0x37: g_st.wordSelIdx = navNextLine(g_st.wordSelIdx); return;
                case 0x36: g_st.wordSelIdx = navPrevReading(g_st.wordSelIdx, n); return;
                case 0x38: g_st.wordSelIdx = navNextReading(g_st.wordSelIdx, n); return;
                default: break;
            }
        }
        return;
    }

    if (st.fn) {
        for (auto c : st.word) {
            switch (c) {
                case 'q': case 'Q': clearInput(); return;
                case 'b': case 'B': return;
                case 'f': case 'F': return;
                case 'h': case 'H': return;
                case 'l': case 'L': return;
                default: break;
            }
        }
    }

    if (st.alt) {
        g_st.mode = (DisplayMode)((g_st.mode + 2) % 3);
        return;
    }

    if (st.del) {
        backspace();
        return;
    }

    for (auto c : st.word) {
        if (c == '`') {
            clearInput();
            return;
        }
    }

    if (st.enter) {
        if (g_st.showingCands) {
            loadDetailForCandidate(g_st.candIndex);
            g_st.showingDetail = (g_st.detailBuf != nullptr);
            g_st.showingCands = !g_st.showingDetail;
        } else if (g_st.inputLen > 0) {
            doQuery();
        }
        return;
    }

    for (auto hid : st.hid_keys) {
        if (hid == 0x33) {
            if (g_st.showingDetail) g_disp.prevPage();
            else if (g_st.showingCands && g_st.candCount > 0) {
                g_st.candIndex = (g_st.candIndex > 0)
                                 ? g_st.candIndex - 1
                                 : g_st.candCount - 1;
            }
            return;
        }
        if (hid == 0x37) {
            if (g_st.showingDetail) g_disp.nextPage();
            else if (g_st.showingCands && g_st.candCount > 0) {
                g_st.candIndex = (g_st.candIndex + 1) % g_st.candCount;
            }
            return;
        }
        if (hid == 0x36) {
            loadAdjacentDetail(true);
            return;
        }
        if (hid == 0x38) {
            loadAdjacentDetail(false);
            return;
        }
    }

    if (!g_st.showingDetail) {
        for (auto c : st.word) {
            if (c >= 32 && c <= 126) {
                if (c >= 'A' && c <= 'Z') c += 32;
                appendChar((char)c);
            }
        }
    }
}

// ============ Setup / Loop ============
void setup() {
#if SCR_USE_EXTERNAL
    // 外屏模式: 初始化顺序关键(参考外屏点亮参考项目)
    //   ① CS 拉高防干扰  ② M5Cardputer.begin  ③ sdSPI.begin
    //   ④ externalDisplay.init  ⑤ SD.begin
    pinMode(EXT_LCD_CS, OUTPUT);
    pinMode(SD_CS, OUTPUT);
    digitalWrite(EXT_LCD_CS, HIGH);
    digitalWrite(SD_CS, HIGH);
#endif

    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);    // M5GFX 自动检测 ADV (G5/G6 上拉触发)
    // 内屏固定 rotation(1), 与 SCREEN_ROTATION (外屏用) 解耦
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.fillScreen(COLOR_BG);

    memset(&g_st, 0, sizeof(g_st));
    g_st.mode = MODE_ALL;
    g_st.hintsVisible = true;

#if SCR_USE_EXTERNAL
    // 外屏模式: 关闭内屏背光, 初始化外屏(共享 SPI3_HOST)
    M5Cardputer.Display.setBrightness(0);
    sdSPI.begin(40, 39, 14, SD_CS);   // 先初始化 SPI 总线
    digitalWrite(SD_CS, HIGH);
    externalDisplay.init();           // bus_shared=true, 复用同一 SPI3
    externalDisplay.setRotation(SCREEN_ROTATION);
    externalDisplay.setBrightness(255);
    externalDisplay.fillScreen(COLOR_BG);
    g_lcd = &externalDisplay;
#else
    // 内屏模式: 正常开启背光
    M5Cardputer.Display.setBrightness(255);
    g_lcd = &M5Cardputer.Display;
#endif

    // SD 卡在屏幕就绪后再初始化
#if SCR_USE_EXTERNAL
    lcd_quiesce();                     // 释放外屏 SPI
    g_st.sdOk = SD.begin(SD_CS, sdSPI, 25000000, "/sd", 5, false);
#else
    SPI.begin(40, 39, 14, SD_CS);
    g_st.sdOk = SD.begin(SD_CS, SPI, 25000000);
#endif
    if (!g_st.sdOk) {
        Serial.println("SD init failed");
        g_lcd->setTextColor(COLOR_SD_BAD, COLOR_BG);
        g_lcd->setTextSize(1);
        g_lcd->setCursor(20, 60);
        g_lcd->print("SD Failed");
        return;
    }

#if SCR_USE_EXTERNAL
    lcd_quiesce();                     // 释放外屏 SPI 给字体加载
#endif
    if (g_font.beginLazyFromFile(FONT_PATH) == 0) {
        Serial.println("Font load failed");
        g_st.sdOk = false;
        return;
    }

#if SCR_USE_EXTERNAL
    lcd_quiesce();                     // 释放外屏 SPI 给 dict 索引加载
#endif
    g_dict.begin(IDX_PATH, BIN_PATH);

    g_disp.begin(&g_font, g_lcd);

    readBattery();

    Serial.printf("Setup done: SCR_USE_EXTERNAL=%d, screen %dx%d, content_lines=%d, max_pages=%d\n",
                  SCR_USE_EXTERNAL, SCREEN_W, SCREEN_H, CONTENT_LINES, MAX_PAGES);
    renderAll();
}

void loop() {
    M5Cardputer.update();

    if (!g_st.sdOk) {
        delay(500);
        return;
    }

    readBattery();

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        Keyboard_Class::KeysState st = M5Cardputer.Keyboard.keysState();
#if SCR_USE_EXTERNAL
        // 释放外屏 SPI 总线, 让 dict/font 后续 SD 访问独占
        lcd_quiesce();
#endif
        processKey(st);
        renderAll();
    }

    delay(15);
}
