#ifndef DICT_ENGINE_H
#define DICT_ENGINE_H

#include <Arduino.h>
#include <SD.h>
#include "color_config.h"

#define MAX_BUCKETS  729      // 27*27
#define MAX_CANDIDATES 8

struct __attribute__((packed)) BucketRecord {
    char     key[2];   // 'a'-'z' + '_'
    uint32_t start;
    uint32_t end;
};

struct Candidate {
    char key[64];      // 词条（小写）
    uint16_t len;      // content 长度
    uint32_t offset;   // dict.bin 中的字节偏移
};

class DictEngine {
public:
    DictEngine();

    // 加载索引（dict.idx）。返回加载的 bucket 数
    int begin(const char* idxPath, const char* binPath);

    // 精确查找：命中返回 true，content/len 填充
    bool lookupExact(const char* key, uint8_t* content, uint16_t& len,
                     uint32_t& offset);

    // 模糊查找：先前缀再包含，最多 maxCount 条
    void lookupCandidates(const char* query, Candidate* out, int& count,
                          int maxCount = MAX_CANDIDATES);

    // 找 currentKey 在字典序中的上一条/下一条记录头部
    // （仅读 kl+key+cl，不读 content；调用者按 contentLen 分配后用 readContentAt 读 content）
    // isPrev=true 找上一条，到首词返回 false
    // isPrev=false 找下一条，到末词返回 false
    bool lookupAdjacentHeader(const char* currentKey, bool isPrev,
                              char* newKeyOut, int keyBufSize,
                              uint16_t& contentLen, uint32_t& newOffset);

    // 按 offset 读 record 的 content 到 content（content 必须 ≥ contentLen）
    bool readContentAt(uint32_t offset, uint8_t* content, uint16_t contentLen);

    // 检查文件存在性
    bool isReady() const { return _ready; }

private:
    BucketRecord _buckets[MAX_BUCKETS];
    int _bucketCount;
    bool _ready;
    char _binPath[64];

    // 持久化 dict.bin 句柄：begin() 打开后全程复用，避免每次查询 SD.open/close
    // ESP32 上 SD.open 一次约 5–15ms，原来按 ,// 一次按键要 3–4 次 open
    File _file;

    // 在指定 byte offset 读取单条记录到 content 缓冲；返回 true
    bool readRecordAt(uint32_t offset, char* keyOut, uint8_t* content,
                      uint16_t& len, int keyBufSize);

    // 计算 bucket key
    void bucketOf(const char* word, char out[2]);

    // 二分查找 bucket 索引
    int findBucket(const char bk[2]);
};

#endif // DICT_ENGINE_H