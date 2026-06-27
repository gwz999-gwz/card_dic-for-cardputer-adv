#ifndef FONT_LOADER_H
#define FONT_LOADER_H

#include <Arduino.h>
#include <SD.h>
#include <stdint.h>

/* CJF1 字体格式
 * Header (16 bytes):
 *   [0..3]   magic "CJF1"
 *   [4..5]   total_glyphs (uint16 LE)
 *   [6]      font_height (uint8)
 *   [7]      max_width   (uint8)
 *
 * Index (total_glyphs × 10 bytes, sorted by unicode):
 *   [0..3]   unicode     (uint32 LE)
 *   [4..7]   data_offset (uint32 LE, offset within bitmap section)
 *   [8]      width       (uint8)
 *   [9]      height      (uint8)
 *
 * Bitmap (1bpp, MSB-left, row-major, padded per row):
 *   size = ((width + 7) / 8) * height
 */

/* 延迟加载缓存
 * - Eager 模式：beginFromFile() 一次性读整个 .bin 到 PSRAM，缓存无意义
 * - Lazy  模式：beginLazyFromFile() 只读 index，位图按需从 SD 读，LRU 缓存
 */
#define FONT_LAZY_CACHE_SIZE   128    // 缓存条目数
#define FONT_LAZY_CACHE_BMPMAX 36     // 单个字形位图最大字节（12×12 = 24，留余量）

struct __attribute__((packed)) GlyphIndex {
    uint32_t unicode;      // offset 0
    uint32_t dataOffset;   // offset 4
    uint8_t  width;        // offset 8
    uint8_t  height;       // offset 9
};

struct __attribute__((packed)) GlyphCacheEntry {
    uint32_t dataOffset;           // 0 = 空槽
    uint32_t lastUsedTick;
    uint8_t  width;
    uint8_t  height;
    uint8_t  bitmap[FONT_LAZY_CACHE_BMPMAX];
};

class FontLoader {
public:
    FontLoader();
    ~FontLoader();

    // 立即加载：一次性把整个 .bin 读入 PSRAM。成功返回字形数，失败返回 0
    int beginFromFile(const char* path);

    // 延迟加载：只读 index（约 42KB），位图按需从 SD 读 + LRU 缓存
    // 启动更快、PSRAM 占用更小；同屏字形数量有限，缓存命中率 > 90%
    int beginLazyFromFile(const char* path);

    // 二分查找字形（仅依赖 index，不读位图）。失败返回 nullptr
    const GlyphIndex* findGlyph(uint32_t unicode) const;

    // 获取字形位图。eager 模式返回 PSRAM 指针；lazy 模式返回 LRU 缓存指针
    // 失败 / 缺字返回 nullptr
    const uint8_t* getGlyphBitmap(const GlyphIndex* g);

    // 直接访问位图段基址（仅 eager 模式有意义，lazy 模式返回 nullptr）
    const uint8_t* getBitmapData() const { return _bitmapData; }

    uint16_t getGlyphCount() const { return _glyphCount; }
    uint8_t  getFontHeight()  const { return _fontHeight; }
    uint8_t  getMaxWidth()    const { return _maxWidth; }
    bool     isLoaded()       const { return _loaded; }
    bool     isLazy()         const { return _lazy; }

    // 缓存统计（调试用，Serial 输出）
    int  cacheHits()   const { return _cacheHits; }
    int  cacheMisses() const { return _cacheMisses; }
    int  cacheEntries() const;
    void printCacheStats() const;

    void end();

private:
    // eager 模式：_binData 持有整文件，_index 指向其中
    uint8_t*     _binData;
    // lazy  模式：_index 单独分配，_sdFile 保持打开
    GlyphIndex*  _index;
    File         _sdFile;
    uint32_t     _bitmapStart;       // 位图段在文件中的绝对偏移
    char         _path[64];
    bool         _lazy;

    GlyphCacheEntry* _cache;        // LRU 缓存
    uint32_t _tick;
    int      _cacheHits;
    int      _cacheMisses;

    uint8_t*     _bitmapData;        // eager: PSRAM 中位图段首址
    uint16_t     _glyphCount;
    uint8_t      _fontHeight;
    uint8_t      _maxWidth;
    bool         _loaded;

    // 缓存原语
    GlyphCacheEntry* cacheLookup(uint32_t dataOffset);
    GlyphCacheEntry* cacheLoad(uint32_t dataOffset, uint8_t width, uint8_t height);
    void cacheReset();
};

// 解码 UTF-8 字符并前进指针
uint32_t decodeUTF8(const char** p);

#endif // FONT_LOADER_H
