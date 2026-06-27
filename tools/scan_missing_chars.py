#!/usr/bin/env python3
"""
扫描 dict.bin 中出现、但 font.bin (CJF1) 中缺失的 Unicode 字符，
按字典中出现次数从多到少排序，输出 CSV（供补字参考）。

用法:
    python scan_missing_chars.py [dict.bin] [font.bin] [output.csv]

默认值基于 device/data/ 目录。

dict.bin 条目格式（小端）:
    [1B] key_len
    [NB] key (UTF-8)
    [2B] content_len
    [MB] content (含 0x00–0x12 标签字节的 UTF-8 标记流)

font.bin (CJF1) 格式:
    [4B] magic 'CJF1'
    [2B] glyph_count (LE)
    [1B] font_height
    [1B] max_width
    [8B] reserved
    [glyph_count × 10B] 索引: [4B unicode][4B offset][1B w][1B h]
    [...]                位图数据
"""

import sys
import struct
from collections import Counter
from pathlib import Path


def load_font_codepoints(font_path: Path) -> set:
    """从 CJF1 字体文件中读取所有已包含的 Unicode 码点。"""
    with open(font_path, 'rb') as f:
        magic = f.read(4)
        if magic != b'CJF1':
            raise ValueError(f"{font_path}: 不是 CJF1 字体（magic={magic!r}）")
        (glyph_count,) = struct.unpack('<H', f.read(2))
        f.read(2 + 8)  # font_height, max_width, reserved
        cps = set()
        for _ in range(glyph_count):
            chunk = f.read(10)
            if len(chunk) < 10:
                raise ValueError(f"{font_path}: 索引区读取中断")
            (cp,) = struct.unpack('<I', chunk[:4])
            cps.add(cp)
    return cps


def scan_dict_chars(dict_path: Path) -> Counter:
    """遍历 dict.bin 条目，统计每个 Unicode 字符的出现次数。
    跳过 < 0x20 的控制字节（标记流 0x00–0x12 等）。"""
    counter = Counter()
    with open(dict_path, 'rb') as f:
        while True:
            head = f.read(1)
            if not head:
                break
            key_len = head[0]
            key_bytes = f.read(key_len)
            if len(key_bytes) < key_len:
                break
            content_len_bytes = f.read(2)
            if len(content_len_bytes) < 2:
                break
            (content_len,) = struct.unpack('<H', content_len_bytes)
            content = f.read(content_len)
            if len(content) < content_len:
                break

            for blob in (key_bytes, content):
                for ch in blob.decode('utf-8', errors='ignore'):
                    cp = ord(ch)
                    if cp < 0x20:
                        continue
                    counter[cp] += 1
    return counter


def main():
    default_dir = Path(r"C:\Users\gwz19\Desktop\card电子辞典\device\data")
    dict_path = Path(sys.argv[1]) if len(sys.argv) > 1 else default_dir / "dict.bin"
    font_path = Path(sys.argv[2]) if len(sys.argv) > 2 else default_dir / "font.bin"
    out_csv   = Path(sys.argv[3]) if len(sys.argv) > 3 else default_dir / "missing_chars.csv"

    for p in (dict_path, font_path):
        if not p.exists():
            print(f"ERROR: 文件不存在 {p}")
            sys.exit(1)

    print(f"[1/3] 读取字体码点: {font_path}")
    font_cps = load_font_codepoints(font_path)
    print(f"      font.bin 包含 {len(font_cps)} 个码点")

    print(f"[2/3] 扫描字典: {dict_path}")
    dict_counter = scan_dict_chars(dict_path)
    total_chars = sum(dict_counter.values())
    print(f"      dict.bin 中共 {len(dict_counter)} 个不同字符，累计 {total_chars} 次")

    print(f"[3/3] 比对并写入 CSV: {out_csv}")
    missing = [(cp, cnt) for cp, cnt in dict_counter.items() if cp not in font_cps]
    missing.sort(key=lambda x: (-x[1], x[0]))

    with open(out_csv, 'w', encoding='utf-8', newline='') as f:
        f.write("code_point,count\n")
        for cp, cnt in missing:
            f.write(f"U+{cp:04X},{cnt}\n")

    print(f"      缺失字符 {len(missing)} 个")
    if missing:
        # Windows 默认 GBK 控制台可能无法打印部分 Unicode，强制 UTF-8 输出
        try:
            sys.stdout.reconfigure(encoding='utf-8')
        except Exception:
            pass
        print("      Top 10:")
        for cp, cnt in missing[:10]:
            try:
                ch = chr(cp)
                ch_repr = f"{ch!r}"
            except Exception:
                ch_repr = '?'
            print(f"        U+{cp:04X}  {cnt:>6}  {ch_repr}")
    print("Done.")


if __name__ == "__main__":
    main()