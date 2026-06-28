#include "display.h"
#include <M5GFX.h>   // LGFX_Device 完整定义（display.h 仅前向声明）

DisplayManager::DisplayManager()
    : _font(nullptr)
    , _lcd(nullptr)
    , _mode(MODE_ALL)
    , _lines(nullptr)
    , _lineCap(0)
    , _totalLines(0)
    , _pageCount(1)
    , _currentPage(0)
    , _wordCount(0) {}

DisplayManager::~DisplayManager() {
    end();
}

void DisplayManager::begin(FontLoader* font, lgfx::v1::LGFX_Device* lcd) {
    _font = font;
    _lcd = lcd;
    if (!_lines) {
        _lineCap = MAX_PAGES * CONTENT_LINES;
        size_t bytes = sizeof(RenderLine) * _lineCap;
        _lines = (RenderLine*)ps_malloc(bytes);
        if (!_lines) {
            // PSRAM 不可用时回退到常规 RAM
            _lines = (RenderLine*)malloc(bytes);
        }
        if (_lines) {
            memset(_lines, 0, bytes);
            Serial.printf("DisplayManager: %d RenderLines in %s (%u bytes)\n",
                          _lineCap, _lines && bytes > 0 ? "PSRAM" : "RAM",
                          (unsigned)bytes);
        } else {
            Serial.println("DisplayManager: RenderLine alloc failed!");
            _lineCap = 0;
        }
    }
}

void DisplayManager::end() {
    if (_lines) {
        free(_lines);
        _lines = nullptr;
    }
    _lineCap = 0;
    _totalLines = 0;
    _pageCount = 1;
    _currentPage = 0;
    _wordCount = 0;
}

int DisplayManager::charWidth(uint8_t b) {
    // 参考 cardputer-macro：ASCII 用 M5GFX 内置 6×8 字体 × 1.5 缩放，
    // 渲染为 9×12，纵向匹配 CJK 字形；横向推进统一 7px（轻微重叠以提高密度）
    if (b < 0x80) return ASCII_CHAR_W;
    return 12;  // CJK
}

int DisplayManager::utf8CharWidth(const uint8_t* p) {
    uint8_t c = *p;
    if (c < 0x80) return charWidth(c);
    // 通过字体查询实际宽度
    if (_font && _font->isLoaded()) {
        const char* pp = (const char*)p;
        uint32_t cp = decodeUTF8(&pp);
        const GlyphIndex* g = _font->findGlyph(cp);
        if (g) return g->width;
    }
    return 12;  // CJK 默认 12
}

// 字符宽度（带字体回退）
static inline int effectiveCharWidth(uint8_t b, FontLoader* font) {
    if (b < 0x80) {
        return 9;  // ASCII：1.5x 缩放后渲染 9px，推进也用 9px（避免重叠）
    }
    if (font && font->isLoaded()) {
        uint8_t buf[5] = {b, 0, 0, 0, 0};
        const char* p2 = (const char*)buf;
        uint32_t cp = decodeUTF8(&p2);
        const GlyphIndex* g = font->findGlyph(cp);
        if (g) return g->width;
    }
    return 12;
}

bool DisplayManager::putUtf8Char(RenderLine& line, int& usedWidth,
                                 const uint8_t*& p, const uint8_t* end) {
    // 估算字符宽度
    int w = effectiveCharWidth(*p, _font);

    // 检查是否需要换行
    if (usedWidth + w > SCREEN_W - 2 * MARGIN_X) {
        return false;  // 需要换行
    }
    if (line.len + 4 > MAX_LINE_BYTES) {
        return false;
    }

    // 拷贝 UTF-8 字符（1-4 字节）
    int bytes = 1;
    uint8_t c = *p;
    if ((c & 0x80) == 0) bytes = 1;
    else if ((c & 0xE0) == 0xC0) bytes = 2;
    else if ((c & 0xF0) == 0xE0) bytes = 3;
    else if ((c & 0xF8) == 0xF0) bytes = 4;

    if (p + bytes > end) return false;

    for (int i = 0; i < bytes; i++) {
        line.data[line.len++] = p[i];
    }
    usedWidth += w;
    p += bytes;
    return true;
}

int DisplayManager::layoutContent(const uint8_t* content, uint16_t contentLen,
                                   DisplayMode mode) {
    _totalLines = 0;
    _pageCount = 1;
    // 注意：不要重置 _currentPage，否则每次 renderAll 时翻页结果会丢失
    // 主程序在加载新词时自己调 setPage(0)
    _mode = mode;  // renderLine 不再用 _mode 做颜色覆盖，但保留用于将来扩展
    if (!content || contentLen == 0) {
        _currentPage = 0;
        return 0;
    }

    int maxLines = _lineCap;
    if (maxLines <= 0 || !_lines) {
        _pageCount = 1;
        return 0;
    }
    RenderLine* cur = &_lines[0];
    cur->len = 0;
    int usedWidth = 0;
    uint8_t curColorTag = TAG_DEF;  // 当前 chunk 的颜色 tag（自动换行时复用）

    const uint8_t* p = content;
    const uint8_t* end = content + contentLen;

    while (p < end && _totalLines < maxLines) {
        uint8_t b = *p;

        if (b == TAG_NEWLINE) {
            // 段尾：闭合当前行（空行折叠：cur 为空时直接跳过）
            if (cur->len > 0) {
                if (_totalLines + 1 >= maxLines) break;
                _totalLines++;
                cur = &_lines[_totalLines];
                cur->len = 0;
                usedWidth = 0;
            }
            p++;
            continue;
        }

        // 段落标记 0x10-0x12：开新段
        if (b >= TAG_PARA_EN && b <= TAG_PARA_HEAD) {
            uint8_t para = b;
            p++;
            // peek 段内第一个 color tag（不消耗）
            uint8_t firstColor = 0;
            const uint8_t* q = p;
            while (q < end && *q != TAG_NEWLINE
                   && !(*q >= TAG_PARA_EN && *q <= TAG_PARA_HEAD)) {
                if (*q >= TAG_HEADWORD && *q <= TAG_AUDIO_US) {
                    firstColor = *q;
                    break;
                }
                q++;
            }
            // 段可见性：原 mode 过滤
            bool visible = paraVisible(para, mode);
            // v4：CN 模式额外隐藏例句段（段内第一色为 TAG_EXAMPLE）
            if (visible && mode == MODE_CN_ONLY && firstColor == TAG_EXAMPLE) {
                visible = false;
            }
            if (!visible) {
                // 跳过整个段（p 已过 PARA_TYPE，到 NEWLINE 或下一个 PARA_*）
                while (p < end && *p != TAG_NEWLINE
                       && !(*p >= TAG_PARA_EN && *p <= TAG_PARA_HEAD)) {
                    p++;
                }
                continue;
            }
            // 段可见：当前行非空则换行（段永远在独立行）
            if (cur->len > 0) {
                if (_totalLines + 1 >= maxLines) break;
                _totalLines++;
                cur = &_lines[_totalLines];
                cur->len = 0;
                usedWidth = 0;
            }
            // PARA_TYPE 写入行首（renderLine 跳过）
            if (cur->len < MAX_LINE_BYTES) {
                cur->data[cur->len++] = para;
            }
            continue;
        }

        // 颜色标记 0x01-0x09
        if (b >= TAG_HEADWORD && b <= TAG_AUDIO_US) {
            if (cur->len < MAX_LINE_BYTES) {
                cur->data[cur->len++] = b;
            }
            curColorTag = b;
            p++;
            continue;
        }

        // 普通字符（含 UTF-8 多字节）：peek 字节数和宽度
        int bytes = 1;
        uint8_t c = b;
        if ((c & 0x80) == 0) bytes = 1;
        else if ((c & 0xE0) == 0xC0) bytes = 2;
        else if ((c & 0xF0) == 0xE0) bytes = 3;
        else if ((c & 0xF8) == 0xF0) bytes = 4;
        if (p + bytes > end) { p++; continue; }

        int w = utf8CharWidth(p);

        // 硬切：超宽时换行，新行开头插入 curColorTag（颜色继承）
        if (usedWidth + w > SCREEN_W - 2 * MARGIN_X) {
            if (_totalLines + 1 >= maxLines) break;
            _totalLines++;
            cur = &_lines[_totalLines];
            cur->len = 0;
            usedWidth = 0;
            if (cur->len < MAX_LINE_BYTES) {
                cur->data[cur->len++] = curColorTag;
            }
        }
        // 写字符
        for (int i = 0; i < bytes && cur->len < MAX_LINE_BYTES; i++) {
            cur->data[cur->len++] = p[i];
        }
        usedWidth += w;
        p += bytes;
    }

    // 收尾：最后一段
    if (cur->len > 0 && _totalLines < maxLines) {
        _totalLines++;
    }

    _pageCount = (_totalLines + CONTENT_LINES - 1) / CONTENT_LINES;
    if (_pageCount < 1) _pageCount = 1;
    if (_pageCount > MAX_PAGES) _pageCount = MAX_PAGES;
    if (_currentPage >= _pageCount) _currentPage = _pageCount - 1;
    if (_currentPage < 0) _currentPage = 0;
    return _totalLines;
}

void DisplayManager::setPage(int p) {
    if (p < 0) p = 0;
    if (p >= _pageCount) p = _pageCount - 1;
    _currentPage = p;
}

// 选词模式:把当前 WordSpan 暂存到 _words[] (按 1 行保存)
// hasPrevFrag=true 时升级为 2 行词,自动合并 prevFragment 信息
static inline void saveWordSpan(WordSpan* words, int& wordCount, int maxWords,
                                int localLine, int x, int xEnd, const char* cur,
                                bool& hasPrevFrag,
                                int prevFragLine, int prevFragX, int prevFragXEnd) {
    if (wordCount >= maxWords) return;
    WordSpan& ws = words[wordCount++];
    memset(&ws, 0, sizeof(ws));
    ws.rangeCount = 1;
    ws.ranges[0].lineIdx = localLine;
    ws.ranges[0].x = x;
    ws.ranges[0].xEnd = xEnd;
    ws.xCenter = (x + xEnd) / 2;
    strncpy(ws.text, cur, sizeof(ws.text) - 1);
    ws.text[sizeof(ws.text) - 1] = '\0';
    if (hasPrevFrag) {
        // 升级为跨两行词
        ws.rangeCount = 2;
        ws.ranges[1] = ws.ranges[0];
        ws.ranges[0].lineIdx = prevFragLine;
        ws.ranges[0].x = prevFragX;
        ws.ranges[0].xEnd = prevFragXEnd;
        ws.xCenter = (prevFragX + prevFragXEnd + xEnd - x) / 2;
        hasPrevFrag = false;
    }
}

// 扫描当前页所有英文词,按阅读顺序排序
// 处理硬切跨两行的情况: 上一行末尾的字母片段会与下一行开头的字母片段合并
int DisplayManager::extractWords() {
    _wordCount = 0;
    if (!_lines || _lineCap <= 0) return 0;

    int startLine = _currentPage * CONTENT_LINES;
    int endLine = startLine + CONTENT_LINES;
    if (endLine > _totalLines) endLine = _totalLines;

    bool hasPrevFrag = false;
    char prevFragText[64] = "";
    int prevFragLine = -1, prevFragX = -1, prevFragXEnd = -1;

    for (int li = startLine; li < endLine; li++) {
        int localLine = li - startLine;
        RenderLine& line = _lines[li];
        int x = MARGIN_X;
        const uint8_t* p = line.data;
        const uint8_t* end = line.data + line.len;

        char cur[64] = "";
        int curLen = 0;
        int curStartX = x, curEndX = x;
        bool inWord = false;

        while (p < end) {
            uint8_t b = *p;

            if (b == TAG_NEWLINE || (b >= TAG_PARA_EN && b <= TAG_PARA_HEAD)) {
                p++; continue;
            }
            if (b >= TAG_HEADWORD && b <= TAG_AUDIO_US) {
                p++; continue;
            }

            if (b < 0x80) {
                bool isLetter = (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z');
                if (isLetter) {
                    if (!inWord) {
                        inWord = true;
                        curLen = 0;
                        curStartX = x;
                        cur[0] = '\0';
                        if (hasPrevFrag) {
                            // 跨行续接:从 prevFragment 继续
                            strncpy(cur, prevFragText, sizeof(cur) - 1);
                            cur[sizeof(cur) - 1] = '\0';
                            curLen = strlen(cur);
                        }
                    }
                    if (curLen < 63) {
                        char lc = (b >= 'A' && b <= 'Z') ? (b + 32) : b;
                        cur[curLen++] = lc;
                        cur[curLen] = '\0';
                    }
                    curEndX = x + ASCII_CHAR_W;
                } else {
                    if (inWord) {
                        saveWordSpan(_words, _wordCount, MAX_WORDS,
                                     localLine, curStartX, curEndX, cur,
                                     hasPrevFrag,
                                     prevFragLine, prevFragX, prevFragXEnd);
                        inWord = false;
                        curLen = 0;
                        cur[0] = '\0';
                    }
                }
                x += ASCII_CHAR_W;
                p++;
            } else {
                if (inWord) {
                    saveWordSpan(_words, _wordCount, MAX_WORDS,
                                 localLine, curStartX, curEndX, cur,
                                 hasPrevFrag,
                                 prevFragLine, prevFragX, prevFragXEnd);
                    inWord = false;
                    curLen = 0;
                    cur[0] = '\0';
                }
                int bytes = 1;
                if ((b & 0xE0) == 0xC0) bytes = 2;
                else if ((b & 0xF0) == 0xE0) bytes = 3;
                else if ((b & 0xF8) == 0xF0) bytes = 4;
                if (p + bytes > end) { p++; continue; }
                x += utf8CharWidth(p);
                p += bytes;
            }
        }

        // 行末处理
        if (inWord && curLen > 0) {
            if (hasPrevFrag) {
                // 罕见:3+ 行硬切,把当前 prevFragment 单独保存
                if (_wordCount < MAX_WORDS) {
                    WordSpan& ws = _words[_wordCount++];
                    memset(&ws, 0, sizeof(ws));
                    ws.rangeCount = 1;
                    ws.ranges[0].lineIdx = prevFragLine;
                    ws.ranges[0].x = prevFragX;
                    ws.ranges[0].xEnd = prevFragXEnd;
                    ws.xCenter = (prevFragX + prevFragXEnd) / 2;
                    strncpy(ws.text, prevFragText, sizeof(ws.text) - 1);
                    ws.text[sizeof(ws.text) - 1] = '\0';
                }
            }
            hasPrevFrag = true;
            strncpy(prevFragText, cur, sizeof(prevFragText) - 1);
            prevFragText[sizeof(prevFragText) - 1] = '\0';
            prevFragLine = localLine;
            prevFragX = curStartX;
            prevFragXEnd = curEndX;
        } else {
            if (hasPrevFrag) {
                // 上一行硬切未续接,作为单行词保存
                if (_wordCount < MAX_WORDS) {
                    WordSpan& ws = _words[_wordCount++];
                    memset(&ws, 0, sizeof(ws));
                    ws.rangeCount = 1;
                    ws.ranges[0].lineIdx = prevFragLine;
                    ws.ranges[0].x = prevFragX;
                    ws.ranges[0].xEnd = prevFragXEnd;
                    ws.xCenter = (prevFragX + prevFragXEnd) / 2;
                    strncpy(ws.text, prevFragText, sizeof(ws.text) - 1);
                    ws.text[sizeof(ws.text) - 1] = '\0';
                }
                hasPrevFrag = false;
                prevFragText[0] = '\0';
            }
        }
    }

    // 页内最后一行末尾的 prevFragment 未续接,作为单行词保存
    if (hasPrevFrag && _wordCount < MAX_WORDS) {
        WordSpan& ws = _words[_wordCount++];
        memset(&ws, 0, sizeof(ws));
        ws.rangeCount = 1;
        ws.ranges[0].lineIdx = prevFragLine;
        ws.ranges[0].x = prevFragX;
        ws.ranges[0].xEnd = prevFragXEnd;
        ws.xCenter = (prevFragX + prevFragXEnd) / 2;
        strncpy(ws.text, prevFragText, sizeof(ws.text) - 1);
        ws.text[sizeof(ws.text) - 1] = '\0';
    }

    // 按 (lineIdx, x) 升序排序,填 readingIdx
    for (int i = 0; i < _wordCount - 1; i++) {
        int minIdx = i;
        for (int j = i + 1; j < _wordCount; j++) {
            int aLine = _words[j].ranges[0].lineIdx;
            int aX    = _words[j].ranges[0].x;
            int bLine = _words[minIdx].ranges[0].lineIdx;
            int bX    = _words[minIdx].ranges[0].x;
            if (aLine < bLine || (aLine == bLine && aX < bX)) {
                minIdx = j;
            }
        }
        if (minIdx != i) {
            WordSpan tmp = _words[i];
            _words[i] = _words[minIdx];
            _words[minIdx] = tmp;
        }
    }
    for (int i = 0; i < _wordCount; i++) {
        _words[i].readingIdx = i;
    }
    return _wordCount;
}

void DisplayManager::nextPage() {
    if (_pageCount <= 1) return;
    _currentPage = (_currentPage + 1) % _pageCount;
}

void DisplayManager::prevPage() {
    if (_pageCount <= 1) return;
    _currentPage = (_currentPage == 0) ? _pageCount - 1 : _currentPage - 1;
}

void DisplayManager::clearContentArea() {
    if (_lcd) _lcd->fillRect(0, CONTENT_Y, SCREEN_W, SCREEN_H - CONTENT_Y, COLOR_BG);
}

void DisplayManager::renderLine(int lineIdx, int y, int hlStart, int hlEnd) {
    if (lineIdx < 0 || lineIdx >= _totalLines) return;
    RenderLine& line = _lines[lineIdx];

    int x = MARGIN_X;
    uint16_t curColor = COLOR_DEFAULT;

    const uint8_t* p = line.data;
    const uint8_t* end = line.data + line.len;

    // 选词模式:此字符的 x 范围是否落在 [hlStart, hlEnd) 内 → 反色
    auto isHighlighted = [&](int cx) {
        return hlStart >= 0 && cx >= hlStart && cx < hlEnd;
    };
    // 反色显示:fg <-> bg 互换
    auto drawColor = [&](uint16_t fg, int cx) {
        if (isHighlighted(cx)) {
            _lcd->setTextColor(COLOR_BG, fg);
        } else {
            _lcd->setTextColor(fg, COLOR_BG);
        }
    };

    while (p < end) {
        uint8_t b = *p;

        if (b == TAG_NEWLINE) {
            p++;
            continue;
        }

        // 段落标记（layoutContent 已过滤，这里跳过不渲染）
        if (b >= TAG_PARA_EN && b <= TAG_PARA_HEAD) {
            p++;
            continue;
        }

        // 颜色标记：行缓冲内出现的颜色字节（来自段头或自动换行时 layoutContent 插入）
        if (b >= TAG_HEADWORD && b <= TAG_AUDIO_US) {
            curColor = tagToColor(b);
            p++;
            // 音频占位符直接显示文本
            if (b == TAG_AUDIO_UK) {
                if (isHighlighted(x)) {
                    // 反色块覆盖 [UK]
                    _lcd->fillRect(x, y, 4 * 6, 12, curColor);
                    _lcd->setTextSize(1);
                    _lcd->setTextColor(COLOR_BG, curColor);
                    _lcd->setCursor(x, y);
                    _lcd->print("[UK]");
                } else {
                    _lcd->setTextSize(1);
                    _lcd->setTextColor(curColor, COLOR_BG);
                    _lcd->setCursor(x, y);
                    _lcd->print("[UK]");
                }
                x += 4 * 6;
            } else if (b == TAG_AUDIO_US) {
                if (isHighlighted(x)) {
                    _lcd->fillRect(x, y, 4 * 6, 12, curColor);
                    _lcd->setTextSize(1);
                    _lcd->setTextColor(COLOR_BG, curColor);
                    _lcd->setCursor(x, y);
                    _lcd->print("[US]");
                } else {
                    _lcd->setTextSize(1);
                    _lcd->setTextColor(curColor, COLOR_BG);
                    _lcd->setCursor(x, y);
                    _lcd->print("[US]");
                }
                x += 4 * 6;
            }
            continue;
        }

        // 普通字符（含 UTF-8）
        if (b < 0x80) {
            // ASCII：M5GFX 6×8 字体 × 1.5 缩放，纵向匹配 12px CJK
            drawColor(curColor, x);
            _lcd->setTextSize(1.5f);
            _lcd->setCursor(x, y);
            _lcd->print((char)b);
            x += ASCII_CHAR_W;
            p++;
        } else {
            // 多字节 UTF-8：用字体 drawBitmap
            if (_font && _font->isLoaded()) {
                uint8_t buf[5] = {0};
                int bytes = 1;
                uint8_t c = b;
                if ((c & 0xE0) == 0xC0) bytes = 2;
                else if ((c & 0xF0) == 0xE0) bytes = 3;
                else if ((c & 0xF8) == 0xF0) bytes = 4;
                for (int i = 0; i < bytes && (p + i) < end; i++) {
                    buf[i] = p[i];
                }
                buf[bytes] = '\0';

                const char* pp = (const char*)buf;
                uint32_t cp = decodeUTF8(&pp);
                const GlyphIndex* g = _font->findGlyph(cp);
                if (g) {
                    // 延迟模式下走 LRU 缓存；立即模式直接返回 PSRAM 指针
                    const uint8_t* bmp = _font->getGlyphBitmap(g);
                    if (bmp) {
                        if (isHighlighted(x)) {
                            _lcd->fillRect(x, y, g->width, g->height, curColor);
                            _lcd->drawBitmap(x, y, (uint8_t*)bmp,
                                                           g->width, g->height,
                                                           COLOR_BG, curColor);
                        } else {
                            _lcd->drawBitmap(x, y, (uint8_t*)bmp,
                                                           g->width, g->height,
                                                           curColor, COLOR_BG);
                        }
                    } else {
                        drawColor(COLOR_BAT_LOW, x);
                        _lcd->setTextSize(1);
                        _lcd->setCursor(x, y);
                        _lcd->print("?");
                    }
                    x += g->width;
                } else {
                    // 缺字显示 ?
                    drawColor(COLOR_BAT_LOW, x);
                    _lcd->setTextSize(1);
                    _lcd->setCursor(x, y);
                    _lcd->print("?");
                    x += 6;
                }
                p += bytes;
            } else {
                p++;
            }
        }
    }
}

void DisplayManager::drawCurrentPage(int highlightWordIdx) {
    clearContentArea();
    int start = _currentPage * CONTENT_LINES;
    // 选词模式:要在此页高亮的词 (可能跨两行)
    const WordSpan* hl = nullptr;
    if (highlightWordIdx >= 0 && highlightWordIdx < _wordCount) {
        hl = &_words[highlightWordIdx];
    }
    for (int i = 0; i < CONTENT_LINES; i++) {
        int idx = start + i;
        if (idx >= _totalLines) break;
        int y = CONTENT_Y + i * LINE_HEIGHT;
        int hlStart = -1, hlEnd = -1;
        if (hl) {
            for (int r = 0; r < hl->rangeCount; r++) {
                if (hl->ranges[r].lineIdx == i) {
                    hlStart = hl->ranges[r].x;
                    hlEnd = hl->ranges[r].xEnd;
                    break;
                }
            }
        }
        renderLine(idx, y, hlStart, hlEnd);
    }
    // 页码已移至状态栏右侧（drawStatusBar），不在此处绘制
}

void DisplayManager::drawStatusBar(const char* text, int batteryPct, bool sdOk,
                                   DisplayMode mode, int currentPage, int pageCount,
                                   bool isDetail) {
    (void)sdOk;  // SD 失败时启动已卡住，主界面不再单独显示
    // 顶部 13px 区域
    _lcd->fillRect(0, 0, SCREEN_W, LINE_HEIGHT, COLOR_BG);

    // 字号 1.5x：6×8 → 9×12，与内容区 ASCII 一致
    const float SB_SIZE = 1.5f;
    const int   SB_CHAR_W = 9;  // 1.5x 后字符推进宽度
    char buf[16];

    // ============ 右侧（从右到左画）============
    // 布局：模式 | 电量 | 页码(多页时) | headword
    // 宽度：模式 18px(2字符) | 电量 36px(4字符) | 页码 45px(5字符)
    int rx = SCREEN_W - MARGIN_X;  // 238

    // 模式标签
    _lcd->setTextSize(SB_SIZE);
    _lcd->setTextColor(modeColor(mode), COLOR_BG);
    _lcd->setCursor(rx - 2 * SB_CHAR_W, 1);
    _lcd->print(modeLabel(mode));
    rx -= 2 * SB_CHAR_W + 2;  // 220

    // 电量
    snprintf(buf, sizeof(buf), "%d%%", batteryPct);
    _lcd->setTextColor(
        batteryPct <= 20 ? COLOR_BAT_LOW : COLOR_BAT_OK, COLOR_BG);
    _lcd->setCursor(rx - 4 * SB_CHAR_W, 1);
    _lcd->print(buf);
    rx -= 4 * SB_CHAR_W + 2;  // 182

    // 页码（详情页 + 多页时）：紧贴 headword 右侧
    if (isDetail && pageCount > 1) {
        snprintf(buf, sizeof(buf), " %d/%d", currentPage + 1, pageCount);
        _lcd->setTextColor(COLOR_XREF, COLOR_BG);
        _lcd->setCursor(rx - (int)strlen(buf) * SB_CHAR_W, 1);
        _lcd->print(buf);
        rx -= (int)strlen(buf) * SB_CHAR_W + 2;
    }

    // ============ 左侧 headword / query ============
    // 截断到 rx 允许的字符数
    int maxChars = (rx - MARGIN_X) / SB_CHAR_W;  // 可用宽度 / 字符宽
    char trunc[32];
    if (isDetail) {
        if (text && text[0]) {
            int n = strlen(text);
            if (n > maxChars && maxChars >= 3) {
                strncpy(trunc, text, maxChars - 3);
                trunc[maxChars - 3] = '\0';
                strcat(trunc, "...");
            } else {
                strncpy(trunc, text, sizeof(trunc) - 1);
                trunc[sizeof(trunc) - 1] = '\0';
            }
            _lcd->setTextColor(COLOR_HEADWORD, COLOR_BG);
            _lcd->setCursor(MARGIN_X, 1);
            _lcd->print(trunc);
        }
    } else {
        // 搜索页："> query" 形式
        if (text && text[0]) {
            // "> " 占 2 字符，剩余给 query
            int queryMax = maxChars - 2;
            if (queryMax < 1) queryMax = 1;
            if ((int)strlen(text) > queryMax && queryMax >= 3) {
                strncpy(trunc, text, queryMax - 3);
                trunc[queryMax - 3] = '\0';
                strcat(trunc, "...");
            } else {
                strncpy(trunc, text, sizeof(trunc) - 1);
                trunc[sizeof(trunc) - 1] = '\0';
            }
            _lcd->setTextColor(COLOR_INPUT, COLOR_BG);
            _lcd->setCursor(MARGIN_X, 1);
            _lcd->print("> ");
            _lcd->print(trunc);
        } else {
            _lcd->setTextColor(COLOR_XREF, COLOR_BG);
            _lcd->setCursor(MARGIN_X, 1);
            _lcd->print("> _");
        }
    }
}

int DisplayManager::drawMixedString(int x, int y, const char* str, uint16_t color,
                                  int hlStart, int hlEnd) {
    // 中英混排渲染: ASCII 走 M5GFX 内置字体(1.5x), CJK 走 font.bin (drawBitmap)
    // _lcd->print() 只支持内置字体,不能直接打中文
    if (!str) return x;
    const uint8_t* p = (const uint8_t*)str;
    while (*p) {
        uint8_t b = *p;

        // 高亮判定
        bool highlighted = (hlStart >= 0 && x >= hlStart && x < hlEnd);
        uint16_t fg = highlighted ? COLOR_BG : color;
        uint16_t bg = highlighted ? color : COLOR_BG;

        if (b < 0x80) {
            // ASCII: 内置 6x8 × 1.5 → 9x12
            _lcd->setTextSize(1.5f);
            _lcd->setTextColor(fg, bg);
            _lcd->setCursor(x, y);
            _lcd->print((char)b);
            x += ASCII_CHAR_W;
            p++;
        } else {
            // CJK / 多字节 UTF-8
            int bytes = 1;
            if ((b & 0xE0) == 0xC0) bytes = 2;
            else if ((b & 0xF0) == 0xE0) bytes = 3;
            else if ((b & 0xF8) == 0xF0) bytes = 4;

            if (_font && _font->isLoaded()) {
                uint8_t buf[5] = {0};
                for (int i = 0; i < bytes; i++) buf[i] = p[i];
                buf[bytes] = '\0';
                const char* pp = (const char*)buf;
                uint32_t cp = decodeUTF8(&pp);
                const GlyphIndex* g = _font->findGlyph(cp);
                if (g) {
                    const uint8_t* bmp = _font->getGlyphBitmap(g);
                    if (bmp) {
                        if (highlighted) {
                            _lcd->fillRect(x, y, g->width, g->height, color);
                            _lcd->drawBitmap(x, y, (uint8_t*)bmp,
                                                           g->width, g->height,
                                                           COLOR_BG, color);
                        } else {
                            _lcd->drawBitmap(x, y, (uint8_t*)bmp,
                                                           g->width, g->height,
                                                           color, COLOR_BG);
                        }
                    } else {
                        // 位图缺失 → 画 "?"
                        _lcd->setTextSize(1);
                        _lcd->setTextColor(fg, bg);
                        _lcd->setCursor(x, y);
                        _lcd->print("?");
                    }
                    x += g->width;
                } else {
                    // 缺字 → 画 "?" (1x 字体, 推进 6px, 与 renderLine 一致)
                    _lcd->setTextSize(1);
                    _lcd->setTextColor(fg, bg);
                    _lcd->setCursor(x, y);
                    _lcd->print("?");
                    x += 6;
                }
            } else {
                x += 12;  // 字体未加载时的兜底
            }
            p += bytes;
        }
    }
    return x;
}

void DisplayManager::drawHints(const char* hint) {
    // 底部提示行（紧贴屏幕底部，13px 高度）
    int y = SCREEN_H - LINE_HEIGHT + 1;
    _lcd->fillRect(0, y - 1, SCREEN_W, LINE_HEIGHT, COLOR_BG);
    drawMixedString(MARGIN_X, y, hint, COLOR_XREF);
}

void DisplayManager::drawIntroScreen(const char* const lines[], int n) {
    // 开屏介绍:状态栏下方开始,每行 13px,最多 CONTENT_LINES 行(9 行)
    _lcd->fillRect(0, CONTENT_Y, SCREEN_W,
                                 SCREEN_H - CONTENT_Y, COLOR_BG);
    for (int i = 0; i < n && i < CONTENT_LINES; i++) {
        int y = CONTENT_Y + i * LINE_HEIGHT;
        if (lines[i]) drawMixedString(MARGIN_X, y, lines[i], COLOR_XREF);
    }
}

void DisplayManager::clearHints() {
    int y = SCREEN_H - LINE_HEIGHT + 1;
    _lcd->fillRect(0, y - 1, SCREEN_W, LINE_HEIGHT, COLOR_BG);
}