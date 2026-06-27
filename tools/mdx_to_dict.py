"""
MDX → dict.bin + dict.idx 转换器

用法：
  python mdx_to_dict.py <input.mdx> <output_dir>

输出：
  <output_dir>/dict.bin   二进制词典主体
  <output_dir>/dict.idx   bucket 索引

字典条目格式（小端）：
  [1B] key_len
  [NB] key (UTF-8, 已转小写)
  [2B] content_len
  [MB] content (来自 html_parser 的标记流)

bucket 索引格式：
  每条 10 字节：
    [2B] bucket_key (2 个 ASCII 字符，如 'ap', '__')
    [4B] start_offset
    [4B] end_offset
"""

import sys
import struct
from pathlib import Path
from readmdict import MDX
from html_parser import parse_html


def normalize_key(key_bytes):
    """把词条 key 转成小写 UTF-8 字符串，过滤 @@@LINK 重定向。"""
    try:
        s = key_bytes.decode('utf-8').strip().lower()
    except UnicodeDecodeError:
        s = key_bytes.decode('gbk', errors='replace').strip().lower()
    return s


def bucket_key(word):
    """
    词条前两字符生成 bucket key。
    非 a-z 字符用 '_' 替代。
    """
    if not word:
        return b'__'
    a = word[0]
    b = word[1] if len(word) > 1 else '_'
    a = a if 'a' <= a <= 'z' else '_'
    b = b if 'a' <= b <= 'z' else '_'
    return (a + b).encode('ascii')


def main():
    if len(sys.argv) < 3:
        print(f'Usage: {sys.argv[0]} <input.mdx> <output_dir>')
        sys.exit(1)

    src = Path(sys.argv[1])
    out_dir = Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)

    if not src.exists():
        print(f'ERROR: {src} not found')
        sys.exit(1)

    print(f'Loading {src} ...')
    mdx = MDX(str(src))
    items = list(mdx.items())
    print(f'Total entries in MDX: {len(items)}')

    # 准备条目列表
    print('Parsing entries ...')
    entries = []
    skipped = 0
    for k, v in items:
        key = normalize_key(k)
        if not key or not v:
            skipped += 1
            continue
        # 跳过 @@@LINK 重定向条目
        if v.startswith(b'@@@LINK='):
            skipped += 1
            continue
        # 解析 HTML
        content = parse_html(v)
        if len(content) < 2:
            skipped += 1
            continue
        entries.append((key, content))

    print(f'Valid entries: {len(entries)}  Skipped: {skipped}')

    # 按 key 排序
    print('Sorting ...')
    entries.sort(key=lambda e: e[0])

    # 生成 dict.bin 并记录 offset
    print('Writing dict.bin ...')
    dict_path = out_dir / 'dict.bin'
    idx_path = out_dir / 'dict.idx'

    # bucket -> [start, end]
    buckets = {}
    current_offset = 0

    with open(dict_path, 'wb') as f:
        for key, content in entries:
            key_bytes = key.encode('utf-8')
            if len(key_bytes) > 255:
                # 过长截断（OALD 词条通常较短）
                key_bytes = key_bytes[:255]
                key = key_bytes.decode('utf-8', errors='ignore')
            # 写入 [1B key_len][NB key][2B content_len][MB content]
            record = struct.pack('<B', len(key_bytes)) + key_bytes
            record += struct.pack('<H', len(content)) + content
            f.write(record)

            # 记录 bucket 范围
            bk = bucket_key(key)
            if bk not in buckets:
                buckets[bk] = [current_offset, current_offset]
            buckets[bk][1] = current_offset + len(record)

            current_offset += len(record)

    print(f'dict.bin size: {current_offset} bytes ({current_offset / 1024 / 1024:.2f} MB)')

    # 生成 dict.idx
    print('Writing dict.idx ...')
    with open(idx_path, 'wb') as f:
        # 按 bucket key 排序
        for bk in sorted(buckets):
            start, end = buckets[bk]
            f.write(struct.pack('<2sII', bk, start, end))

    print(f'dict.idx: {len(buckets)} buckets')
    print('Done.')


if __name__ == '__main__':
    main()