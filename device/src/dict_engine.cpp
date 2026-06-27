#include "dict_engine.h"
#include <ctype.h>

DictEngine::DictEngine()
    : _bucketCount(0), _ready(false) {
    memset(_buckets, 0, sizeof(_buckets));
    _binPath[0] = '\0';
}

int DictEngine::begin(const char* idxPath, const char* binPath) {
    _ready = false;
    if (_file) _file.close();
    strncpy(_binPath, binPath, sizeof(_binPath) - 1);
    _binPath[sizeof(_binPath) - 1] = '\0';

    File f = SD.open(idxPath);
    if (!f) {
        Serial.printf("DictEngine: cannot open %s\n", idxPath);
        return 0;
    }

    _bucketCount = 0;
    while (f.available() && _bucketCount < MAX_BUCKETS) {
        // dict.idx 每条 10 字节：2 key + 4 start + 4 end
        if (f.read((uint8_t*)&_buckets[_bucketCount], 10) != 10) break;
        _bucketCount++;
    }
    f.close();

    // 持久化打开 dict.bin，所有后续查询复用此句柄
    _file = SD.open(_binPath);
    if (!_file) {
        Serial.printf("DictEngine: cannot open %s\n", _binPath);
        return 0;
    }

    _ready = (_bucketCount > 0);
    Serial.printf("DictEngine: %d buckets, bin=%s (persistent handle)\n",
                  _bucketCount, _binPath);
    return _bucketCount;
}

void DictEngine::bucketOf(const char* word, char out[2]) {
    char a = word[0];
    char b = word[1];
    if (!a) { out[0] = '_'; out[1] = '_'; return; }
    if (a >= 'A' && a <= 'Z') a += 32;
    if (a < 'a' || a > 'z') a = '_';
    if (!b) b = '_';
    else {
        if (b >= 'A' && b <= 'Z') b += 32;
        if (b < 'a' || b > 'z') b = '_';
    }
    out[0] = a;
    out[1] = b;
}

int DictEngine::findBucket(const char bk[2]) {
    for (int i = 0; i < _bucketCount; i++) {
        if (_buckets[i].key[0] == bk[0] && _buckets[i].key[1] == bk[1]) {
            return i;
        }
    }
    return -1;
}

bool DictEngine::readRecordAt(uint32_t offset, char* keyOut,
                              uint8_t* content, uint16_t& len,
                              int keyBufSize) {
    if (!_file) return false;
    if (!_file.seek(offset)) return false;
    uint8_t kl;
    if (_file.read(&kl, 1) != 1) return false;

    int copyLen = (kl < keyBufSize - 1) ? kl : keyBufSize - 1;
    if (_file.read((uint8_t*)keyOut, copyLen) != copyLen) return false;
    keyOut[copyLen] = '\0';
    // 跳过 key 剩余部分
    if (kl > copyLen) _file.seek(_file.position() + (kl - copyLen));

    uint8_t cl[2];
    if (_file.read(cl, 2) != 2) return false;
    len = cl[0] | (cl[1] << 8);

    if (content && len > 0) {
        if (_file.read(content, len) != len) return false;
    }
    return true;
}

bool DictEngine::lookupExact(const char* key, uint8_t* content, uint16_t& len,
                             uint32_t& offset) {
    if (!_ready || !_file) return false;
    char bk[2];
    bucketOf(key, bk);
    int idx = findBucket(bk);
    if (idx < 0) return false;

    uint32_t start = _buckets[idx].start;
    uint32_t end = _buckets[idx].end;

    if (!_file.seek(start)) return false;
    bool found = false;
    while (_file.position() < end && _file.available()) {
        uint32_t pos = _file.position();
        uint8_t kl;
        if (_file.read(&kl, 1) != 1) break;

        // 读 key
        char buf[64];
        int copyLen = (kl < (int)sizeof(buf) - 1) ? kl : (int)sizeof(buf) - 1;
        if (_file.read((uint8_t*)buf, copyLen) != copyLen) break;
        buf[copyLen] = '\0';
        if (kl > copyLen) _file.seek(_file.position() + (kl - copyLen));

        uint8_t cl[2];
        if (_file.read(cl, 2) != 2) break;
        uint16_t recLen = cl[0] | (cl[1] << 8);

        if (strcmp(buf, key) == 0) {
            if (content && recLen > 0) {
                if (_file.read(content, recLen) != recLen) break;
            }
            len = recLen;
            offset = pos;
            found = true;
            break;
        }

        // 跳过 content
        _file.seek(_file.position() + recLen);
    }

    return found;
}

void DictEngine::lookupCandidates(const char* query, Candidate* out,
                                  int& count, int maxCount) {
    count = 0;
    if (!_ready || !_file || !query || !*query) return;

    // 准备小写查询
    char q[64];
    int ql = strlen(query);
    if (ql >= (int)sizeof(q)) ql = sizeof(q) - 1;
    for (int i = 0; i < ql; i++) {
        char c = query[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        q[i] = c;
    }
    q[ql] = '\0';

    // 优先在匹配的 bucket 内查找（前缀候选）
    char bk[2];
    bucketOf(q, bk);
    int idx = findBucket(bk);

    uint32_t scanStart = 0, scanEnd = 0;
    if (idx >= 0) {
        scanStart = _buckets[idx].start;
        scanEnd = _buckets[idx].end;
    }

    auto tryAdd = [&](const char* key, uint16_t recLen, uint32_t pos, bool prefix) -> bool {
        if (count >= maxCount) return false;
        if (prefix || strstr(key, q)) {
            strncpy(out[count].key, key, sizeof(out[count].key) - 1);
            out[count].key[sizeof(out[count].key) - 1] = '\0';
            out[count].len = recLen;
            out[count].offset = pos;
            count++;
            return true;
        }
        return false;
    };

    // 第一轮：同 bucket 内扫描，找前缀 + 包含
    if (idx >= 0 && _file.seek(scanStart)) {
        while (_file.position() < scanEnd && _file.available() && count < maxCount) {
            uint32_t pos = _file.position();
            uint8_t kl;
            if (_file.read(&kl, 1) != 1) break;

            char buf[64];
            int copyLen = (kl < (int)sizeof(buf) - 1) ? kl : (int)sizeof(buf) - 1;
            if (_file.read((uint8_t*)buf, copyLen) != copyLen) break;
            buf[copyLen] = '\0';
            if (kl > copyLen) _file.seek(_file.position() + (kl - copyLen));

            uint8_t cl[2];
            if (_file.read(cl, 2) != 2) break;
            uint16_t recLen = cl[0] | (cl[1] << 8);

            bool isPrefix = (strncmp(buf, q, ql) == 0);
            if (!tryAdd(buf, recLen, pos, isPrefix)) {
                // 满了或不是前缀也不是包含：跳过 content
                _file.seek(_file.position() + recLen);
            } else {
                _file.seek(_file.position() + recLen);
            }
        }
    }

    // 第二轮（可选）：如果还不够，全文件扫描（限制时间）
    // OALDpe 32MB 全扫描约 5-10 秒，首次启动后可以预扫描建二级索引
    // 此处先简化：仅在主 bucket 内查找
}

bool DictEngine::lookupAdjacentHeader(const char* currentKey, bool isPrev,
                                      char* newKeyOut, int keyBufSize,
                                      uint16_t& contentLen, uint32_t& newOffset) {
    contentLen = 0;
    newOffset = 0;
    newKeyOut[0] = '\0';
    if (!_ready || !_file || !currentKey || !*currentKey) return false;

    // 1. 拿当前 key 的 offset
    uint32_t curOffset;
    uint16_t curLen;
    if (!lookupExact(currentKey, nullptr, curLen, curOffset)) return false;

    // 2. 找当前 bucket 和范围
    char bk[2];
    bucketOf(currentKey, bk);
    int bIdx = findBucket(bk);
    if (bIdx < 0) return false;

    uint32_t bStart = _buckets[bIdx].start;
    uint32_t bEnd = _buckets[bIdx].end;

    uint32_t targetOffset;

    if (isPrev) {
        // prev：在 [bStart, curOffset) 内顺序扫描，记录最后一条 record 的 offset
        uint32_t prevOff = (uint32_t)-1;
        if (!_file.seek(bStart)) return false;
        while (_file.position() < curOffset && _file.available()) {
            uint32_t pos = _file.position();
            uint8_t kl;
            if (_file.read(&kl, 1) != 1) break;
            _file.seek(_file.position() + kl);
            uint8_t cl[2];
            if (_file.read(cl, 2) != 2) break;
            uint16_t recLen = cl[0] | (cl[1] << 8);
            uint32_t afterRec = _file.position() + recLen;
            if (afterRec <= curOffset) {
                prevOff = pos;
                _file.seek(afterRec);
            } else {
                break;
            }
        }

        if (prevOff != (uint32_t)-1) {
            targetOffset = prevOff;
        } else {
            // 跨 bucket：上一 bucket 的最后一条
            if (bIdx == 0) return false;  // 已是首词
            uint32_t pbStart = _buckets[bIdx - 1].start;
            uint32_t pbEnd = _buckets[bIdx - 1].end;
            uint32_t lastOff = pbStart;
            if (!_file.seek(pbStart)) return false;
            while (_file.position() < pbEnd && _file.available()) {
                uint32_t pos = _file.position();
                uint8_t kl;
                if (_file.read(&kl, 1) != 1) break;
                _file.seek(_file.position() + kl);
                uint8_t cl[2];
                if (_file.read(cl, 2) != 2) break;
                uint16_t recLen = cl[0] | (cl[1] << 8);
                uint32_t afterRec = _file.position() + recLen;
                if (afterRec <= pbEnd) {
                    lastOff = pos;
                    _file.seek(afterRec);
                } else {
                    break;
                }
            }
            targetOffset = lastOff;
        }
    } else {
        // next：跳过当前 record，读 kl+cl 算出 record 末尾
        if (!_file.seek(curOffset)) return false;
        uint8_t kl;
        if (_file.read(&kl, 1) != 1) return false;
        _file.seek(_file.position() + kl);
        uint8_t cl[2];
        if (_file.read(cl, 2) != 2) return false;
        uint16_t recLen = cl[0] | (cl[1] << 8);
        uint32_t afterCur = _file.position() + recLen;

        if (afterCur < bEnd) {
            targetOffset = afterCur;
        } else {
            // 跨 bucket：下一 bucket 的第一条
            if (bIdx + 1 >= _bucketCount) return false;  // 已是末词
            targetOffset = _buckets[bIdx + 1].start;
        }
    }

    // 3. 读 targetOffset 的 record 头部（kl + key + cl）
    if (!_file.seek(targetOffset)) return false;
    uint8_t kl;
    if (_file.read(&kl, 1) != 1) return false;
    int copyLen = (kl < keyBufSize - 1) ? kl : keyBufSize - 1;
    if (_file.read((uint8_t*)newKeyOut, copyLen) != copyLen) return false;
    newKeyOut[copyLen] = '\0';
    if (kl > copyLen) _file.seek(_file.position() + (kl - copyLen));
    uint8_t cl[2];
    if (_file.read(cl, 2) != 2) return false;
    contentLen = cl[0] | (cl[1] << 8);
    newOffset = targetOffset;

    return true;
}

bool DictEngine::readContentAt(uint32_t offset, uint8_t* content, uint16_t contentLen) {
    if (!_file) return false;
    if (!_file.seek(offset)) return false;
    uint8_t kl;
    if (_file.read(&kl, 1) != 1) return false;
    _file.seek(_file.position() + kl);
    uint8_t cl[2];
    if (_file.read(cl, 2) != 2) return false;
    uint16_t recLen = cl[0] | (cl[1] << 8);
    if (recLen > contentLen) recLen = contentLen;
    if (content && recLen > 0) {
        if (_file.read(content, recLen) != recLen) return false;
    }
    return true;
}
