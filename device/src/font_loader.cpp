#include "font_loader.h"
#include <SD.h>

FontLoader::FontLoader()
    : _binData(nullptr)
    , _index(nullptr)
    , _bitmapStart(0)
    , _lazy(false)
    , _cache(nullptr)
    , _tick(0)
    , _cacheHits(0)
    , _cacheMisses(0)
    , _bitmapData(nullptr)
    , _glyphCount(0)
    , _fontHeight(0)
    , _maxWidth(0)
    , _loaded(false) {
    _path[0] = '\0';
}

FontLoader::~FontLoader() { end(); }

void FontLoader::end() {
    if (_sdFile) _sdFile.close();
    if (_binData) {
        free(_binData);
    } else if (_lazy && _index) {
        // lazy 模式 _index 单独分配
        free(_index);
    }
    if (_cache) {
        free(_cache);
    }
    _binData = nullptr;
    _index = nullptr;
    _bitmapData = nullptr;
    _cache = nullptr;
    _glyphCount = 0;
    _fontHeight = 0;
    _maxWidth = 0;
    _bitmapStart = 0;
    _tick = 0;
    _cacheHits = 0;
    _cacheMisses = 0;
    _loaded = false;
    _lazy = false;
    _path[0] = '\0';
}

static bool parseHeader(File& f, uint16_t& glyphCount,
                        uint8_t& fontH, uint8_t& maxW, size_t& fileSize) {
    fileSize = f.size();
    if (fileSize < 16) return false;
    uint8_t header[16];
    if (f.read(header, 16) != 16) return false;
    if (memcmp(header, "CJF1", 4) != 0) return false;
    glyphCount = header[4] | ((uint16_t)header[5] << 8);
    fontH = header[6];
    maxW = header[7];
    return true;
}

int FontLoader::beginFromFile(const char* path) {
    end();

    File f = SD.open(path);
    if (!f) {
        Serial.printf("FontLoader: cannot open %s\n", path);
        return 0;
    }

    size_t fileSize;
    if (!parseHeader(f, _glyphCount, _fontHeight, _maxWidth, fileSize)) {
        f.close();
        return 0;
    }

    size_t indexSize = (size_t)_glyphCount * sizeof(GlyphIndex);
    size_t minSize = 16 + indexSize;
    if (_glyphCount == 0 || fileSize < minSize) {
        f.close();
        return 0;
    }

    // 一次性读入到 PSRAM（如果可用）或 malloc
    _binData = (uint8_t*)ps_malloc(fileSize);
    if (!_binData) {
        _binData = (uint8_t*)malloc(fileSize);
    }
    if (!_binData) {
        f.close();
        return 0;
    }

    // 头已经读过了 16B，回到文件头
    f.seek(0);
    size_t read = f.read(_binData, fileSize);
    f.close();
    if (read != fileSize) {
        free(_binData);
        _binData = nullptr;
        return 0;
    }

    _index = (GlyphIndex*)(_binData + 16);
    _bitmapData = _binData + 16 + indexSize;
    _lazy = false;
    _loaded = true;

    Serial.printf("FontLoader (eager): %d glyphs, h=%d, max_w=%d, size=%u\n",
                  _glyphCount, _fontHeight, _maxWidth, (unsigned)fileSize);
    return _glyphCount;
}

int FontLoader::beginLazyFromFile(const char* path) {
    end();

    _sdFile = SD.open(path);
    if (!_sdFile) {
        Serial.printf("FontLoader: cannot open %s\n", path);
        return 0;
    }

    size_t fileSize;
    if (!parseHeader(_sdFile, _glyphCount, _fontHeight, _maxWidth, fileSize)) {
        _sdFile.close();
        return 0;
    }

    size_t indexSize = (size_t)_glyphCount * sizeof(GlyphIndex);
    size_t minSize = 16 + indexSize;
    if (_glyphCount == 0 || fileSize < minSize) {
        _sdFile.close();
        return 0;
    }

    // 只把 index 读入 PSRAM（典型 ~42KB for 4233 glyphs）
    _index = (GlyphIndex*)ps_malloc(indexSize);
    if (!_index) {
        _index = (GlyphIndex*)malloc(indexSize);
    }
    if (!_index) {
        _sdFile.close();
        return 0;
    }
    if (_sdFile.read((uint8_t*)_index, indexSize) != indexSize) {
        free(_index);
        _index = nullptr;
        _sdFile.close();
        return 0;
    }

    _bitmapStart = 16 + indexSize;
    _bitmapData = nullptr;
    _binData = nullptr;
    _lazy = true;
    _loaded = true;

    // 分配 LRU 缓存
    size_t cacheBytes = sizeof(GlyphCacheEntry) * FONT_LAZY_CACHE_SIZE;
    _cache = (GlyphCacheEntry*)ps_malloc(cacheBytes);
    if (!_cache) {
        _cache = (GlyphCacheEntry*)malloc(cacheBytes);
    }
    if (_cache) {
        cacheReset();
    }

    strncpy(_path, path, sizeof(_path) - 1);
    _path[sizeof(_path) - 1] = '\0';

    Serial.printf("FontLoader (lazy): %d glyphs, h=%d, max_w=%d, "
                  "index=%u, cache=%d entries\n",
                  _glyphCount, _fontHeight, _maxWidth,
                  (unsigned)indexSize, FONT_LAZY_CACHE_SIZE);
    return _glyphCount;
}

const GlyphIndex* FontLoader::findGlyph(uint32_t unicode) const {
    if (!_loaded || _glyphCount == 0) return nullptr;

    int lo = 0;
    int hi = _glyphCount - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t mu = _index[mid].unicode;
        if (mu == unicode) return &_index[mid];
        if (mu < unicode) lo = mid + 1;
        else hi = mid - 1;
    }
    return nullptr;
}

const uint8_t* FontLoader::getGlyphBitmap(const GlyphIndex* g) {
    if (!g || !_loaded) return nullptr;

    if (!_lazy) {
        // eager 模式：位图在 PSRAM 中
        if (!_bitmapData) return nullptr;
        return _bitmapData + g->dataOffset;
    }

    // lazy 模式：先查 LRU 缓存
    GlyphCacheEntry* e = cacheLookup(g->dataOffset);
    if (e) {
        _cacheHits++;
        return e->bitmap;
    }

    // 缓存未命中：加载
    e = cacheLoad(g->dataOffset, g->width, g->height);
    if (e) {
        _cacheMisses++;
        return e->bitmap;
    }
    return nullptr;
}

GlyphCacheEntry* FontLoader::cacheLookup(uint32_t dataOffset) {
    for (int i = 0; i < FONT_LAZY_CACHE_SIZE; i++) {
        if (_cache[i].dataOffset == dataOffset) {
            _cache[i].lastUsedTick = _tick++;
            return &_cache[i];
        }
    }
    return nullptr;
}

GlyphCacheEntry* FontLoader::cacheLoad(uint32_t dataOffset,
                                       uint8_t width, uint8_t height) {
    if (!_cache) return nullptr;

    // 超出位图尺寸上限的字符（如 16×16 字体）暂不缓存，返回临时读取
    size_t bmpSize = ((width + 7) / 8) * (size_t)height;
    if (bmpSize > FONT_LAZY_CACHE_BMPMAX) {
        Serial.printf("FontLoader: glyph %u×%u bmp=%u > cache max %d, skip\n",
                      width, height, (unsigned)bmpSize, FONT_LAZY_CACHE_BMPMAX);
        return nullptr;
    }

    // 找 LRU 槽位（lastUsedTick 最小者）
    int victim = 0;
    uint32_t minTick = _cache[0].lastUsedTick;
    for (int i = 1; i < FONT_LAZY_CACHE_SIZE; i++) {
        if (_cache[i].lastUsedTick < minTick) {
            minTick = _cache[i].lastUsedTick;
            victim = i;
        }
    }
    GlyphCacheEntry* e = &_cache[victim];

    // 必要时重开 SD 文件
    if (!_sdFile) {
        _sdFile = SD.open(_path);
        if (!_sdFile) return nullptr;
    }

    // 定位到位图段
    if (!_sdFile.seek(_bitmapStart + dataOffset)) return nullptr;
    size_t got = _sdFile.read(e->bitmap, bmpSize);
    if (got != bmpSize) return nullptr;

    e->dataOffset = dataOffset;
    e->width = width;
    e->height = height;
    e->lastUsedTick = _tick++;
    return e;
}

void FontLoader::cacheReset() {
    if (_cache) memset(_cache, 0, sizeof(GlyphCacheEntry) * FONT_LAZY_CACHE_SIZE);
}

int FontLoader::cacheEntries() const {
    if (!_cache) return 0;
    int n = 0;
    for (int i = 0; i < FONT_LAZY_CACHE_SIZE; i++) {
        if (_cache[i].dataOffset != 0) n++;
    }
    return n;
}

void FontLoader::printCacheStats() const {
    int total = _cacheHits + _cacheMisses;
    int hitRate = (total > 0) ? (_cacheHits * 100 / total) : 0;
    Serial.printf("FontLoader cache: %d/%d hits (%d%%), %d entries live\n",
                  _cacheHits, total, hitRate, cacheEntries());
}

uint32_t decodeUTF8(const char** p) {
    const uint8_t* s = (const uint8_t*)*p;
    uint8_t c = *s;
    if (c < 0x80) {
        (*p)++;
        return c;
    }
    if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        uint32_t cp = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        *p += 2;
        return cp;
    }
    if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        uint32_t cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        *p += 3;
        return cp;
    }
    if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80
        && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        uint32_t cp = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12)
                    | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        *p += 4;
        return cp;
    }
    (*p)++;
    return 0xFFFD;
}
