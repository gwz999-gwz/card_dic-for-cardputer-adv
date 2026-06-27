#!/usr/bin/env python3
"""
ComboKey Font Builder (Extended)
================================
扩展版：从 TTF 提取更全面的 Unicode 范围，重点覆盖音标 / 拼音 / 各类符号，
便于用 font_editor.html 检查后手动补缺。

输入: simhei.ttf (默认) 或 chinesefront-12px.ttf
输出: font_ext.bin (CJF1 格式，与设备端 font_loader.cpp 完全兼容)

用法:
    python build_font_ext.py [ttf_path] [output_path] [font_size]
    python build_font_ext.py --list-ranges         # 列出所有 Unicode 范围

输出文件:
    font_ext.bin           - 主字体文件
    font_ext.bin.skipped   - TTF 中缺失的码点（可作为手动补缺的参考清单）
"""

import sys
import os
import struct
import time
from PIL import ImageFont, Image, ImageDraw

# ============================================================
# 扩展 Unicode 范围（重点：音标 / 拼音 / 西欧 / 希腊 / 通用符号 / CJK 全集）
# ============================================================
DEFAULT_RANGES = [
    # —— 基础拉丁 ——
    (0x0020, 0x007E, "ASCII 可见字符"),
    (0x00A0, 0x00FF, "Latin-1 补充"),
    (0x0100, 0x017F, "Latin Extended-A（带音调的西欧字母）"),
    (0x0180, 0x024F, "Latin Extended-B"),

    # —— 音标 / 拼音（重点）——
    (0x0250, 0x02AF, "IPA Extensions（核心音标：ə ɪ ʊ ɛ æ ɔ ʃ ʒ θ ð ŋ ...）"),
    (0x02B0, 0x02FF, "Spacing Modifier Letters（ˈ ˌ ː ˑ ˞ ɐ ɑ ...）"),
    (0x1D00, 0x1D7F, "Phonetic Extensions（ɐ ɒ ɓ ɔ ɕ ɖ ɗ ɘ ...）"),
    (0x1D80, 0x1DBF, "Phonetic Extensions Supplement"),
    (0x1DC0, 0x1DFF, "Combining Diacritical Marks Supplement"),
    (0x20D0, 0x20FF, "Combining Diacritical Marks for Symbols"),

    # —— 变音符号 ——
    (0x0300, 0x036F, "Combining Diacritical Marks（ˇ ˋ ˊ ̈ ̀ ́ ̂ ̃ ̄ ̆ ̇ ̈ ̊ ...）"),

    # —— 希腊 / 西里尔 ——
    (0x0370, 0x03FF, "Greek and Coptic（α β γ δ ε ζ ...）"),
    (0x0400, 0x04FF, "Cyrillic"),

    # —— 通用标点 / 符号 ——
    (0x2000, 0x206F, "General Punctuation（— – … · • ' \" \" « » ‹ ›）"),
    (0x2070, 0x209F, "Superscripts and Subscripts（¹ ² ³ ₀ ₁ ₂ ...）"),
    (0x20A0, 0x20CF, "Currency Symbols（$ € £ ¥ ₩ ₽ ¢ ...）"),
    (0x2100, 0x214F, "Letterlike Symbols（℃ ℉ № ℡ ™ © ® ...）"),
    (0x2150, 0x218F, "Number Forms（Ⅰ Ⅱ Ⅲ ⅓ ¼ ½ ¾ ...）"),
    (0x2190, 0x21FF, "Arrows（← → ↑ ↓ ⇒ ⇔ ↔ ↕ ...）"),
    (0x2200, 0x22FF, "Mathematical Operators（± × ÷ ≤ ≥ ≠ ≈ ∞ ...）"),
    (0x2300, 0x23FF, "Misc Technical（⌂ ⌘ ⌥ ⌫ ⌦ ⏏ ⏰ ...）"),
    (0x2500, 0x257F, "Box Drawing（─ │ ┌ ┐ └ ┘ ├ ┤ ┬ ┴ ┼）"),
    (0x2580, 0x259F, "Block Elements（▀ ▁ ▂ ▃ ▄ ▅ ▆ ▇ █ ▉ ▊ ▋ ▌ ▍ ▎ ▏）"),
    (0x25A0, 0x25FF, "Geometric Shapes（■ □ ▢ ▲ △ ▼ ▽ ◆ ◇ ○ ● ...）"),

    # —— CJK 符号 / 日韩 ——
    (0x3000, 0x303F, "CJK Symbols and Punctuation（、 。 「」 『』 （） 【】 〈〉 《》 々 〆 ...）"),
    (0x3040, 0x309F, "Hiragana（平假名 ぁ-ん）"),
    (0x30A0, 0x30FF, "Katakana（片假名 ァ-ン）"),
    (0x3130, 0x318F, "Hangul Compatibility Jamo（ㄅ ㄆ ㄇ ... ㄱ ㄴ ㄷ ...）"),
    (0x3190, 0x319F, "Kanbun"),

    # —— CJK 统一表意 ——
    (0x3400, 0x4DBF, "CJK Unified Ideographs Extension A（罕用汉字 6582）"),
    (0x4E00, 0x9FFF, "CJK Unified Ideographs（核心汉字 20992）"),
    (0xAC00, 0xD7A3, "Hangul Syllables（韩文音节）"),

    # —— 全角 / 半角 ——
    (0xFF00, 0xFFEF, "Halfwidth and Fullwidth Forms（全角字符 240）"),
]


def list_ranges():
    print("=" * 72)
    print(f"{'范围':<22} {'字符数':>8}  说明")
    print("-" * 72)
    total = 0
    for start, end, desc in DEFAULT_RANGES:
        count = end - start + 1
        total += count
        print(f"U+{start:04X}–U+{end:04X}    {count:>8}  {desc}")
    print("-" * 72)
    print(f"{'合计':<22} {total:>8}  （去重后约此数量）")
    print("=" * 72)


def collect_chars(ranges, extra_chars=""):
    """从范围 + extra 收集字符，Unicode 排序去重。"""
    seen = set()
    result = []
    for ch in extra_chars:
        if ch not in seen:
            seen.add(ch)
            result.append(ch)
    for start, end, _ in ranges:
        for cp in range(start, end + 1):
            ch = chr(cp)
            if ch not in seen:
                seen.add(ch)
                result.append(ch)
    result.sort(key=ord)
    return result


# ============================================================
# 位图渲染（与原 build_font.py 完全一致，1bpp MSB-left）
# ============================================================
def render_glyph(font, char, font_height, target_width):
    """
    渲染单字到位图。
    返回 (w, h, bytes, has_ink) 或 None（TTF 中无此字符）。
    """
    if target_width is None:
        target_width = font_height

    try:
        bbox = font.getbbox(char)
    except Exception:
        bbox = None

    # TTF 中无此字符 → 跳过（不写入）
    if bbox is None or bbox[2] <= bbox[0] or bbox[3] <= bbox[1]:
        return None

    x1, y1, x2, y2 = bbox
    w, h = x2 - x1, y2 - y1

    pad = 2
    img_w = w + pad * 2
    img_h = max(h, font_height) + pad * 2

    img = Image.new('L', (img_w, img_h), 0)
    draw = ImageDraw.Draw(img)
    draw.text((pad - x1, pad - y1), char, font=font, fill=255)
    bw = img.point(lambda p: 255 if p > 0 else 0, '1')

    bbox2 = bw.getbbox()
    if bbox2 is None:
        # 字符存在但无墨（如纯空白 / 控制符）→ 用空位图
        w, h = target_width, font_height
        return (w, h, b'\x00' * ((w + 7) // 8 * h), False)

    cropped = bw.crop(bbox2)
    crop_w, crop_h = cropped.size

    final_w = max(crop_w, target_width)
    final_h = max(crop_h, font_height)
    final_img = Image.new('1', (final_w, final_h), 0)
    x_off = (final_w - crop_w) // 2
    y_off = (final_h - crop_h) // 2
    final_img.paste(cropped, (x_off, y_off))

    width, height = final_w, final_h
    row_bytes = (width + 7) // 8
    raw_bytes = bytearray(row_bytes * height)

    pixels = list(final_img.getdata())
    for y in range(height):
        for x in range(width):
            val = pixels[y * width + x]
            if val:
                byte_idx = y * row_bytes + (x >> 3)
                bit_mask = 1 << (7 - (x & 7))
                raw_bytes[byte_idx] |= bit_mask

    return (width, height, bytes(raw_bytes), True)


# ============================================================
# BIN 构建
# ============================================================
def build_bin(ttf_path, output_path, font_size=12, target_width=None,
              ranges=None, extra_chars=""):
    print(f"[FontBuilder] Loading: {ttf_path}")
    font = ImageFont.truetype(ttf_path, font_size)

    test_bbox = font.getbbox('国')
    font_height = max(test_bbox[3] - test_bbox[1], font_size) if test_bbox else font_size
    print(f"[FontBuilder] Font height: {font_height}px")

    # —— 提取 TTF 实际包含的字符集（cmap） ——
    # 用 cmap 跳过 TTF 中不存在的字形（避免被回退渲染为 .notdef 方框占位）
    try:
        cmap = font.font.getBestCmap()
        supported = set(cmap.keys()) if cmap else None
        if supported is not None:
            print(f"[FontBuilder] TTF 实际包含 {len(supported)} 个码点（cmap）")
    except (AttributeError, Exception) as e:
        print(f"[FontBuilder] cmap 不可用，回退到 getbbox 检查: {e}")
        supported = None

    # —— 探测 .notdef 方框的位图 ——
    # 直接用 U+0288（ʈ）和 U+028F（ʏ）：这两个 IPA 字符在 SimHei 中确认缺失，
    # 渲染时会回退到 TTF 的 .notdef 方框占位。用它们的位图作为"占位符"模板，
    # 后续任何渲染结果与之一致的字符均视为缺失字形，跳过不写。
    notdef_bmps = []  # 多个参考位图（不同 TTF 的 .notdef 大小可能不同）
    for probe in [chr(0x0288), chr(0x028F)]:
        probe_result = render_glyph(font, probe, font_height, target_width)
        if probe_result is not None:
            _, _, pb, has_ink = probe_result
            if has_ink and pb not in notdef_bmps:
                notdef_bmps.append(pb)
    if notdef_bmps:
        print(f"[FontBuilder] 已探测到 {len(notdef_bmps)} 个 .notdef 方框位图，"
              f"将跳过渲染为方框的字形")
    else:
        print(f"[FontBuilder] .notdef 为空，跳过方框过滤")

    if target_width is None:
        target_width = font_height
    if ranges is None:
        ranges = DEFAULT_RANGES

    chars = collect_chars(ranges, extra_chars)
    total = len(chars)
    print(f"[FontBuilder] {len(ranges)} ranges, {total} unique code points")

    index_entries = []   # [(unicode, offset, w, h)]
    bitmap_chunks = []
    current_offset = 0
    skipped = []         # TTF 中缺失的码点（写入清单供手动补缺）
    skipped_notdef = 0  # 因 .notdef 跳过的计数（subset of skipped）
    start_time = time.time()
    last_print = start_time

    for i, ch in enumerate(chars):
        cp = ord(ch)
        # 第一道过滤：cmap 检查（TTF 中完全不存在的字形）
        if supported is not None and cp not in supported:
            skipped.append(cp)
            continue
        result = render_glyph(font, ch, font_height, target_width)
        if result is None:
            skipped.append(cp)
            continue
        w, h, bmp, has_ink = result
        # 第二道过滤：.notdef 方框占位（TTF 回退渲染的占位符）
        if notdef_bmps and bmp in notdef_bmps:
            skipped.append(cp)
            skipped_notdef += 1
            continue
        index_entries.append((cp, current_offset, w, h))
        bitmap_chunks.append(bmp)
        current_offset += len(bmp)

        # 每 2 秒打印一次进度
        now = time.time()
        if now - last_print > 2:
            elapsed = now - start_time
            rate = (i + 1) / elapsed if elapsed > 0 else 0
            eta = (total - i - 1) / rate if rate > 0 else 0
            print(f"  ... {i + 1}/{total} "
                  f"(kept {len(index_entries)}, skipped {len(skipped)}) "
                  f"{rate:.0f}/s ETA {eta:.0f}s")
            last_print = now

    glyph_count = len(index_entries)
    max_width = max(w for (_, _, w, _) in index_entries) if index_entries else target_width
    bitmap_total = sum(len(b) for b in bitmap_chunks)
    index_size = glyph_count * 10
    total_size = 16 + index_size + bitmap_total

    print(f"[FontBuilder] Kept: {glyph_count}, Skipped (TTF 中缺失): {len(skipped)}")
    print(f"[FontBuilder] Max width: {max_width}px")
    print(f"[FontBuilder] Index: {index_size/1024:.1f}KB, Bitmap: {bitmap_total/1024:.1f}KB")
    print(f"[FontBuilder] Total BIN: {total_size/1024:.1f}KB")

    with open(output_path, 'wb') as f:
        f.write(b'CJF1')
        f.write(struct.pack('<H', glyph_count))
        f.write(struct.pack('<B', font_height))
        f.write(struct.pack('<B', max_width))
        f.write(b'\x00' * 8)
        for unic, offset, w, h in index_entries:
            f.write(struct.pack('<I', unic))
            f.write(struct.pack('<I', offset))
            f.write(struct.pack('<B', w))
            f.write(struct.pack('<B', h))
        for bmp in bitmap_chunks:
            f.write(bmp)

    # 写缺失清单（供 font_editor.html 后续手动补缺参考）
    skipped_path = output_path + '.skipped.txt'
    with open(skipped_path, 'w', encoding='utf-8') as f:
        f.write(f"# {len(skipped)} code points missing in TTF: {ttf_path}\n")
        f.write(f"# font_size={font_size}\n\n")
        # 按范围分组
        prev_end = -2
        range_start = None
        cur_range = []
        for cp in sorted(skipped):
            if cp == prev_end + 1 and cur_range:
                cur_range.append(cp)
            else:
                if cur_range:
                    _write_range(f, cur_range)
                cur_range = [cp]
            prev_end = cp
        if cur_range:
            _write_range(f, cur_range)
    print(f"[FontBuilder] Skipped list: {skipped_path}")
    print(f"[FontBuilder] Written: {output_path}")
    return output_path


def _write_range(f, cps):
    if len(cps) == 1:
        f.write(f"U+{cps[0]:04X}  ({cps[0]})\n")
    else:
        f.write(f"U+{cps[0]:04X}–U+{cps[-1]:04X}  ({cps[0]}..{cps[-1]}, {len(cps)} chars)\n")


# ============================================================
# Main
# ============================================================
def main():
    if len(sys.argv) > 1 and sys.argv[1] == '--list-ranges':
        list_ranges()
        return

    script_dir = os.path.dirname(os.path.abspath(__file__))
    ttf_path = os.path.join(script_dir, 'chinesefront-12px.ttf')
    output_path = os.path.join(script_dir, 'font_ext.bin')
    font_size = 12

    if len(sys.argv) > 1:
        ttf_path = sys.argv[1]
    if len(sys.argv) > 2:
        output_path = sys.argv[2]
    if len(sys.argv) > 3:
        font_size = int(sys.argv[3])

    if not os.path.exists(ttf_path):
        alt = os.path.join(script_dir, 'chinesefront-12px.ttf')
        if os.path.exists(alt):
            print(f"[FontBuilder] TTF not found at {ttf_path}, falling back to {alt}")
            ttf_path = alt
        else:
            print(f"ERROR: TTF not found at {ttf_path}")
            sys.exit(1)

    build_bin(ttf_path, output_path, font_size=font_size)


if __name__ == "__main__":
    main()
