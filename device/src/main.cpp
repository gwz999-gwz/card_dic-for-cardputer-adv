/**
 * Cardputer 电子词典 — 主程序
 *
 * 功能：
 *   - 输入查询（中英文 12px 混合显示）
 *   - 模糊匹配（前缀/包含，最多 8 候选）
 *   - 方向键导航（;,./ = 上/下/左/右）
 *   - 分页浏览长释义
 *   - 显示模式切换（Fn+E/C/A）
 *   - 状态栏（电量 / 模式 / 页码）
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
#include "font_loader.h"
#include "dict_engine.h"
#include "display.h"
#include "color_config.h"

#define SD_CS    12
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
// 启动时 g_st.batteryPct=0（memset 初始），第一次有效读会刷新
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
// 任意状态（候选列表/详情页）都可调用；调用后进入详情页
bool loadAdjacentDetail(bool isPrev) {
    if (g_st.candCount == 0) return false;
    const char* curKey = g_st.cands[g_st.candIndex].key;

    char newKey[64];
    uint16_t newLen;
    uint32_t newOff;
    if (!g_dict.lookupAdjacentHeader(curKey, isPrev, newKey, sizeof(newKey),
                                     newLen, newOff)) {
        return false;  // 已到首/末词
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

    // 用新词替换 cands[0]，candCount=1（左右键不再走候选列表语义）
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
        // 如果唯一命中且 key 完全等于 query，直接进入释义
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
// 4方向智能导航: 在 wordSelMode 中按 4 个方向键移动高亮词
//  ,/ = 同一行/跨行的前/后一个词(按 readingIdx 顺序,首尾循环)
//  ;/. = 上一/下一行: 找 x-center 与当前最接近的词(逐行向上/向下找)
//      若该行无词,继续找下一行(直到找到或越界)
//      都找不到则保持不变
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
        // 在该行找一个 x-center 最接近 curCenter 的词
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
    return curIdx;  // 找不到就保持
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

// 进入选词模式: 调 extractWords, 把选中词设到第一个
static bool enterWordSelMode() {
    int n = g_disp.extractWords();
    if (n <= 0) return false;
    g_st.wordSelMode = true;
    g_st.wordSelIdx = 0;
    return true;
}

// 选词模式中按 Enter: 选中词作为 query, 强制进入候选词界面(不再自动跳详情)
// 流程: 1st Enter → 候选词列表(按 ;/. 选候选) → 2nd Enter → 详情
// 即使 1 个候选且完全匹配, 也停在候选页由用户再按一次 Enter 进入详情
static void lookupSelectedWord() {
    const WordSpan* w = g_disp.getWord(g_st.wordSelIdx);
    if (!w) return;
    strncpy(g_st.input, w->text, INPUT_MAX);
    g_st.input[INPUT_MAX] = '\0';
    g_st.inputLen = strlen(g_st.input);
    g_st.wordSelMode = false;

    // 直接查候选, 强制 showingCands=true (不走 doQuery 的"1 候选=直接进详情"分支)
    g_dict.lookupCandidates(g_st.input, g_st.cands, g_st.candCount, MAX_CANDIDATES);
    g_st.candIndex = 0;
    g_st.showingCands = true;
    g_st.showingDetail = false;
}

// ============ 渲染 ============
void renderAll() {
    M5Cardputer.Display.fillScreen(COLOR_BG);

    bool isDetailView = g_st.showingDetail && g_st.detailBuf
                        && g_st.candCount > 0;

    // 关键顺序：先 layoutContent 算出新的 _pageCount，再画状态栏
    // 之前 drawStatusBar 在前面，导致首次进入词条时 _pageCount 还是上一次的
    // （构造函数初值 1），状态栏不显示 n/m；按一次 ; 后才正确
    if (isDetailView) {
        g_disp.layoutContent(g_st.detailBuf, g_st.detailLen, g_st.mode);
        // 选词模式:每帧重新提取当前页英文词(防止 page/mode 改变后词列表失效)
        if (g_st.wordSelMode) {
            int n = g_disp.extractWords();
            if (n <= 0) {
                g_st.wordSelMode = false;
            } else if (g_st.wordSelIdx >= n) {
                g_st.wordSelIdx = 0;
            }
        }
    }

    // 状态栏左侧：详情页显示 headword，搜索页显示 query
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
            M5Cardputer.Display.setTextSize(1.5f);  // 跟详情页 ASCII 一致
            uint16_t col = (i == g_st.candIndex) ? COLOR_HEADWORD : COLOR_DEFAULT;
            M5Cardputer.Display.setTextColor(col, COLOR_BG);
            M5Cardputer.Display.setCursor(MARGIN_X, y);
            // 截断：屏幕宽 240px / 9px (1.5x 字符宽) ≈ 26 字符
            // "N. " 占 3 字符，故单词限 22 字符（超长加 "..."）
            char word[26];
            truncateForDisplay(word, g_st.cands[i].key, 22);
            char buf[32];
            snprintf(buf, sizeof(buf), "%d. %s", i + 1, word);
            M5Cardputer.Display.print(buf);
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
        g_disp.clearHints();  // 隐藏前先清掉旧提示文字
    }

    // Ctrl 键 (左下角): 仅在详情页中切换选词模式
    //   进入: 提取当前页所有英文词,高亮第一个
    //   退出: 仅由再按一次 Ctrl 触发
    if (st.ctrl && g_st.showingDetail) {
        if (g_st.wordSelMode) {
            g_st.wordSelMode = false;
        } else {
            enterWordSelMode();  // 无词时静默不进入
        }
        return;
    }

    // 选词模式: 方向键导航, Enter 查词; 其他键一律忽略
    if (g_st.wordSelMode) {
        if (st.enter) {
            lookupSelectedWord();
            return;
        }
        for (auto hid : st.hid_keys) {
            int n = g_disp.getWordCount();
            if (n <= 0) {
                g_st.wordSelMode = false;  // 安全兜底
                return;
            }
            switch (hid) {
                case 0x33:  // ; 上
                    g_st.wordSelIdx = navPrevLine(g_st.wordSelIdx);
                    return;
                case 0x37:  // . 下
                    g_st.wordSelIdx = navNextLine(g_st.wordSelIdx);
                    return;
                case 0x36:  // , 左
                    g_st.wordSelIdx = navPrevReading(g_st.wordSelIdx, n);
                    return;
                case 0x38:  // / 右
                    g_st.wordSelIdx = navNextReading(g_st.wordSelIdx, n);
                    return;
                default: break;
            }
        }
        return;  // 选词模式中其他键都吃掉
    }

    // Fn 组合键（优先级最高）
    if (st.fn) {
        for (auto c : st.word) {
            switch (c) {
                case 'q': case 'Q':
                    clearInput(); return;
                case 'b': case 'B':
                    /* 亮度固定 100%，不再可调 */ return;
                case 'f': case 'F':
                    /* TODO: 收藏切换（Phase 3）*/ return;
                case 'h': case 'H':
                    /* TODO: 历史记录（Phase 3）*/ return;
                case 'l': case 'L':
                    /* TODO: 收藏列表（Phase 3）*/ return;
                default: break;
            }
        }
    }

    // Alt 键：循环切换显示模式（全部 → 只中文 → 只英文 → 全部）
    // 放在选词模式 return 之后，避免选词过程中误触发破坏当前选中词
    if (st.alt) {
        g_st.mode = (DisplayMode)((g_st.mode + 2) % 3);
        return;
    }

    // DEL
    if (st.del) {
        backspace();
        return;
    }

    // ` (backtick) → ESC 等价：清空所有输入，退出详情/候选
    for (auto c : st.word) {
        if (c == '`') {
            clearInput();
            return;
        }
    }

    // Enter
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

    // 方向键（;,./ 通过 HID 码识别，避免与 status.word 字符混淆）
    for (auto hid : st.hid_keys) {
        if (hid == 0x33) {  // ; 上
            if (g_st.showingDetail) g_disp.prevPage();
            else if (g_st.showingCands && g_st.candCount > 0) {
                g_st.candIndex = (g_st.candIndex > 0)
                                 ? g_st.candIndex - 1
                                 : g_st.candCount - 1;
            }
            return;
        }
        if (hid == 0x37) {  // . 下
            if (g_st.showingDetail) g_disp.nextPage();
            else if (g_st.showingCands && g_st.candCount > 0) {
                g_st.candIndex = (g_st.candIndex + 1) % g_st.candCount;
            }
            return;
        }
        if (hid == 0x36) {  // , 左
            // 切词典中按字典序的上一词（候选列表/详情页/直接命中皆可用）
            loadAdjacentDetail(true);
            return;
        }
        if (hid == 0x38) {  // / 右
            // 切词典中按字典序的下一词（候选列表/详情页/直接命中皆可用）
            loadAdjacentDetail(false);
            return;
        }
    }

    // 普通字符输入
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
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.fillScreen(COLOR_BG);

    memset(&g_st, 0, sizeof(g_st));
    g_st.mode = MODE_ALL;
    g_st.hintsVisible = true;
    // 亮度固定 100%
    M5Cardputer.Display.setBrightness(255);

    SPI.begin(40, 39, 14, SD_CS);
    g_st.sdOk = SD.begin(SD_CS, SPI, 25000000);
    if (!g_st.sdOk) {
        Serial.println("SD init failed");
        M5Cardputer.Display.setTextColor(COLOR_SD_BAD, COLOR_BG);
        M5Cardputer.Display.setTextSize(1);
        M5Cardputer.Display.setCursor(20, 60);
        M5Cardputer.Display.print("SD Failed");
        return;
    }

    if (g_font.beginLazyFromFile(FONT_PATH) == 0) {
        Serial.println("Font load failed");
        g_st.sdOk = false;
        return;
    }

    g_dict.begin(IDX_PATH, BIN_PATH);
    g_disp.begin(&g_font);

    // 首次读电量，避免 renderAll 状态栏首次显示 0%
    readBattery();

    Serial.println("Setup done");
    renderAll();
}

void loop() {
    M5Cardputer.update();

    if (!g_st.sdOk) {
        delay(500);
        return;
    }

    // 更新电量（readBattery 内部已处理 -1 情况）
    readBattery();

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        Keyboard_Class::KeysState st = M5Cardputer.Keyboard.keysState();
        processKey(st);
        renderAll();
    }

    delay(15);
}