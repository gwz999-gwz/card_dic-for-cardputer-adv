"""
从 font_builtin_data.h 中提取原始字节，生成 font.bin
C 数组格式：const unsigned char FONT_BUILTIN_DATA[] = { 0x43, 0x4a, ..., 0x00 };
"""

import re
import sys
from pathlib import Path


def extract_header_bytes(h_file):
    text = h_file.read_text(encoding='utf-8')
    # 找到数组内容
    m = re.search(r'\{([^}]+)\}', text, re.DOTALL)
    if not m:
        raise RuntimeError('cannot find array body')
    body = m.group(1)
    # 提取所有 0x?? 字节
    bytes_list = re.findall(r'0x([0-9a-fA-F]{2})', body)
    return bytes(int(b, 16) for b in bytes_list)


def main():
    if len(sys.argv) < 3:
        print(f'Usage: {sys.argv[0]} <input.h> <output.bin>')
        sys.exit(1)

    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])
    data = extract_header_bytes(src)
    print(f'Extracted {len(data)} bytes from {src}')
    # 验证 CJF1 magic
    if data[:4] != b'CJF1':
        raise RuntimeError(f'Bad magic: {data[:4]!r}')
    glyph_count = data[4] | (data[5] << 8)
    print(f'CJF1 magic OK, glyph_count={glyph_count}, '
          f'height={data[6]}, max_width={data[7]}')
    dst.write_bytes(bytes(data))
    print(f'Wrote {dst}')


if __name__ == '__main__':
    main()