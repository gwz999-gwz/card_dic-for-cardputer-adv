"""
字典文件验证器：测试 dict.bin + dict.idx 的读取和查找逻辑

不依赖设备 C++ 代码，模拟 C++ 端的 bucket 索引查找：
1. 把 dict.idx 读入内存（bucket 列表）
2. 测试查找：computer, run, (all) by itself, 不存在的词
3. 测试模糊匹配（前缀/包含）
"""

import struct
import sys
from pathlib import Path

TAG_NAMES = {
    0x00: '\\n', 0x01: 'HEADWORD', 0x02: 'POS', 0x03: 'PHONETIC',
    0x04: 'DEF', 0x05: 'EXAMPLE', 0x06: 'NUM', 0x07: 'XREF',
    0x08: 'AUDIO_UK', 0x09: 'AUDIO_US',
    0x10: 'PARA_EN', 0x11: 'PARA_CN', 0x12: 'PARA_HEAD'
}


def decode(content, max_bytes=400):
    i = 0
    lines = []
    while i < min(len(content), max_bytes):
        b = content[i]
        if b in TAG_NAMES:
            lines.append(f'[{TAG_NAMES[b]}]')
            i += 1
        else:
            j = i
            while j < len(content) and content[j] not in TAG_NAMES:
                j += 1
            try:
                s = content[i:j].decode('utf-8', errors='replace')
                if s.strip():
                    lines.append(f'  "{s[:80]}"')
            except:
                pass
            i = j
    return '\n'.join(lines)


def load_index(idx_path):
    """加载 bucket 索引"""
    buckets = {}
    with open(idx_path, 'rb') as f:
        for _ in range(1000):
            data = f.read(10)
            if len(data) < 10:
                break
            key, start, end = struct.unpack('<2sII', data)
            buckets[key.decode('ascii')] = (start, end)
    return buckets


def normalize(s):
    return s.strip().lower()


def bucket_key(word):
    if not word:
        return '__'
    a = word[0]
    b = word[1] if len(word) > 1 else '_'
    a = a if 'a' <= a <= 'z' else '_'
    b = b if 'a' <= b <= 'z' else '_'
    return a + b


def main():
    if len(sys.argv) < 3:
        print(f'Usage: {sys.argv[0]} <dict.bin> <dict.idx>')
        sys.exit(1)

    bin_path = Path(sys.argv[1])
    idx_path = Path(sys.argv[2])

    print(f'Loading index from {idx_path} ...')
    buckets = load_index(idx_path)
    print(f'Loaded {len(buckets)} buckets')

    # 测试精确查找
    test_queries = ['computer', 'run', '(all) by itself', 'good', 'hello', 'xyzabc']
    print()
    print('=' * 70)
    print('Exact lookups:')
    print('=' * 70)

    with open(bin_path, 'rb') as f:
        for q in test_queries:
            qn = normalize(q)
            bk = bucket_key(qn)
            if bk not in buckets:
                print(f'\n--- {q!r} → bucket {bk!r} NOT FOUND ---')
                continue

            start, end = buckets[bk]
            f.seek(start)
            found = None
            candidates = []
            while f.tell() < end:
                pos = f.tell()
                kl = f.read(1)
                if not kl:
                    break
                key_len = kl[0]
                key = f.read(key_len).decode('utf-8', errors='replace')
                cl_bytes = f.read(2)
                if len(cl_bytes) < 2:
                    break
                content_len = struct.unpack('<H', cl_bytes)[0]
                content = f.read(content_len)

                if key == qn:
                    found = (key, content)
                    break

                if key.startswith(qn):
                    candidates.append(('PREFIX', key))
                elif qn in key:
                    candidates.append(('CONTAINS', key))

                if pos + 3 + key_len + 2 + content_len > end:
                    break

            if found:
                print(f'\n--- {q!r} → FOUND ({len(found[1])} bytes) ---')
                print(decode(found[1]))
            else:
                print(f'\n--- {q!r} → NOT FOUND (candidates: {candidates[:5]}) ---')


if __name__ == '__main__':
    main()