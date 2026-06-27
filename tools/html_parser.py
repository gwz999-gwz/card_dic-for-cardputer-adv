"""
OALDpe HTML → 颜色/段落标记流 转换器（v4：段驱动 + 严格空行控制）

输入：HTML 字符串（UTF-8 编码）
输出：bytes 流，由以下标记字节构成：

颜色标记（控制前景色）：
  0x00         换行 \\n
  0x01         HEADWORD 词头（亮黄）
  0x02         POS 词性（青色）
  0x03         PHONETIC 音标（橙色）
  0x04         DEF 默认释义（白色；CN 模式不再变绿）
  0x05         EXAMPLE 例句（灰色）
  0x06         NUM 编号（绿色）
  0x07         XREF 交叉引用（淡灰）
  0x08         AUDIO_UK 英式发音占位（[UK]）
  0x09         AUDIO_US 美式发音占位（[US]）

段落类型标记（控制显示过滤）：
  0x10         PARA_EN 英文段
  0x11         PARA_CN 中文段
  0x12         PARA_HEAD 词头/标题区（始终显示）

设计原则（v4）：
- 每个段（segment）由 [PARA_TYPE][COLOR][text] 开头
- 段内可有多个 (COLOR, text_chunk) 子片段
- 段非空才输出 [NEWLINE] 闭合（避免空行）
- 段与段之间由"上一段的 NEWLINE + 下一段的 PARA_TYPE"分隔
- 连续 NEWLINE 由 C++ 端 layoutContent 折叠（不写入行缓冲）
- 段内换行由 C++ 端硬切，切行时插入同 COLOR 保证颜色一致
"""

import re
from bs4 import BeautifulSoup, NavigableString, Tag

# 段落标记
PARA_HEAD = 0x12
PARA_EN   = 0x10
PARA_CN   = 0x11

# 颜色标记
TAG_NEWLINE  = 0x00
TAG_HEADWORD = 0x01
TAG_POS      = 0x02
TAG_PHONETIC = 0x03
TAG_DEF      = 0x04
TAG_EXAMPLE  = 0x05
TAG_NUM      = 0x06
TAG_XREF     = 0x07
TAG_AUDIO_UK = 0x08
TAG_AUDIO_US = 0x09

# 需要过滤的标签（不渲染也不留文本）
DROP_TAGS = {'img', 'video', 'picture', 'script', 'style', 'meta', 'link'}


class SegmentBuilder:
    """
    流式构造标记流。

    用法：
        b = SegmentBuilder(out)
        b.open(PARA_EN, TAG_DEF, 'text')   # 段 1 开启
        b.add(TAG_NUM, 'more')             # 段 1 内追加子片段
        b.close()                          # 段 1 结束（NEWLINE 由下次 open/finalize 触发）
        b.open(PARA_CN, TAG_DEF, '中文')   # 段 2
        b.close()
        b.finalize()                       # 闭合最后一段

    严格保证：
    - 段非空才发 NEWLINE（_has_content 控制）
    - open 自动闭合上一段（_close_prev）
    - add/close/open 顺序安全（防御性）
    """

    def __init__(self, out):
        self.out = out
        self._has_content = False  # 当前段是否包含可见字节（text 或 marker）
        self._seg_open = False     # 当前段是否已 open（PARA_TYPE 已发）

    def _write_color(self, color):
        self.out.append(color)

    def _write_text(self, text):
        if not text or not text.strip():
            return
        text = re.sub(r'\s+', ' ', text)
        self.out.extend(text.encode('utf-8', errors='replace'))
        self._has_content = True

    def _write_marker(self, marker):
        """写入会产生可见输出的标记字节（如音频占位 [UK]/[US]）"""
        self.out.append(marker)
        self._has_content = True

    def _close_prev(self):
        """闭合上一段：仅在 _has_content=True 时发 NEWLINE"""
        if self._has_content:
            self.out.append(TAG_NEWLINE)
            self._has_content = False
        self._seg_open = False

    def open(self, para_type, color, text=''):
        """开启新段。如果上一段有内容，先发 NEWLINE 闭合。"""
        self._close_prev()
        self.out.append(para_type)
        self._seg_open = True
        self._write_color(color)
        if text:
            self._write_text(text)

    def add(self, color, text=''):
        """当前段内追加 (color, text) 子片段。text 为空也写 color 字节。"""
        if not self._seg_open:
            return
        self._write_color(color)
        if text:
            self._write_text(text)

    def add_marker(self, marker):
        """添加会产生可见输出的标记（如音频占位 [UK]/[US]）"""
        if not self._seg_open:
            return
        self._write_marker(marker)

    def close(self):
        """标记当前段结束（NEWLINE 由下次 open/finalize 触发）"""
        pass

    def finalize(self):
        self._close_prev()
        self._seg_open = False


def parse_html(raw_bytes):
    """入口：MDX 词条原始字节 → 标记流"""
    try:
        text = raw_bytes.decode('utf-8')
    except UnicodeDecodeError:
        text = raw_bytes.decode('gbk', errors='replace')
    soup = BeautifulSoup(text, 'lxml')
    return _convert(soup)


def _convert(soup):
    """核心转换逻辑"""
    out = bytearray()
    b = SegmentBuilder(out)

    oaldpe = soup.find('oaldpe') or soup

    # ========== 1. 词头区（PARA_HEAD） ==========
    headword_text = None
    h1 = oaldpe.find('h1', class_='headword')
    if h1:
        headword_text = h1.get_text(strip=True)
    else:
        idm = oaldpe.find('span', class_='idm')
        if idm:
            headword_text = idm.get_text(strip=True)
        else:
            any_hw = oaldpe.find(class_=re.compile(r'headword'))
            if any_hw:
                headword_text = any_hw.get_text(strip=True)

    if headword_text:
        b.open(PARA_HEAD, TAG_HEADWORD, headword_text)
        b.close()

    # 词性（取第一个 span.pos）
    pos_span = oaldpe.find('span', class_='pos')
    if pos_span:
        pos_text = pos_span.get_text(' ', strip=True)
        if pos_text:
            b.open(PARA_HEAD, TAG_POS, pos_text)
            b.close()

    # 音标（只从第一个 phonetics 容器取，避免进入 verb_forms 等次要表格）
    top_phonetics = oaldpe.find('span', class_='phonetics')
    if top_phonetics:
        phons = top_phonetics.find_all('span', class_='phon')
        audios = top_phonetics.find_all('a', class_=re.compile(r'audio_play_button'))
        if phons:
            b.open(PARA_HEAD, TAG_PHONETIC, phons[0].get_text(' ', strip=True))
            if len(phons) > 1:
                b.add(TAG_PHONETIC, ' ' + phons[1].get_text(' ', strip=True))
            for a in audios:
                cls = a.get('class', [])
                href = a.get('href', '')
                if 'pron-uk' in cls or '__gb_' in href:
                    b.add_marker(TAG_AUDIO_UK)
                elif 'pron-us' in cls or '__us_' in href:
                    b.add_marker(TAG_AUDIO_US)
            b.close()

    # ========== 2. 义项（PARA_EN + PARA_CN） ==========
    sense_lists = oaldpe.find_all('ol', class_=re.compile(r'^sense'))

    for ol in sense_lists:
        # recursive=True: senses_multiple 内部会用 <span class="shcut-g">
        # 包裹 li.sense（甚至再嵌一层 <h2>），需要递归才能找到。
        for li in ol.find_all('li', class_='sense', recursive=True):
            # 编号
            iteration = li.find('span', class_='iteration', recursive=False)
            num_text = ''
            if iteration:
                num_text = re.sub(r'[^\d]', '', iteration.get_text(strip=True))

            # 英文释义
            def_span = li.find('span', class_='def', recursive=True)
            def_text = ''
            if def_span:
                def_text = def_span.get_text(' ', strip=True)

            # 中文释义（deft > chn）
            cn_text = ''
            deft = li.find('deft', recursive=True)
            if deft:
                chn = deft.find('chn')
                if chn:
                    cn_text = chn.get_text(' ', strip=True)

            # 输出编号+英文释义（合并为同一 PARA_EN 段）
            if num_text and def_text:
                b.open(PARA_EN, TAG_NUM, num_text + '. ')
                b.add(TAG_DEF, def_text)
                b.close()
            elif num_text:
                b.open(PARA_EN, TAG_NUM, num_text + '. ')
                b.close()
            elif def_text:
                b.open(PARA_EN, TAG_DEF, def_text)
                b.close()
            # 三者皆空 → 不输出该 li 的 PARA_EN 段（避免空行）

            # 输出中文释义（独立 PARA_CN 段）
            if cn_text:
                b.open(PARA_CN, TAG_DEF, '：' + cn_text)
                b.close()

            # 例句（li > ul.examples > li > span.x [+ xt]）
            for x in li.find_all('span', class_='x', recursive=True):
                # 英文部分：剥除 xt 子标签
                en_parts = []
                for child in x.children:
                    if isinstance(child, NavigableString):
                        en_parts.append(str(child))
                    elif isinstance(child, Tag):
                        if child.name == 'xt':
                            continue
                        if child.name in DROP_TAGS:
                            continue
                        en_parts.append(child.get_text(' ', strip=True))
                en_text = ' '.join(p for p in en_parts if p).strip()

                # 中文部分
                xt = x.find('xt')
                cn_text_x = ''
                if xt:
                    chn = xt.find('chn')
                    if chn:
                        cn_text_x = chn.get_text(' ', strip=True)

                if en_text:
                    b.open(PARA_EN, TAG_EXAMPLE, '• ' + en_text)
                    b.close()
                if cn_text_x:
                    b.open(PARA_CN, TAG_EXAMPLE, '  ' + cn_text_x)
                    b.close()

    # ========== 3. 交叉引用 ==========
    xrefs = oaldpe.find('span', class_='xrefs')
    if xrefs:
        prefix = xrefs.find('span', class_='prefix')
        prefix_text = prefix.get_text(strip=True) if prefix else ''
        xh_list = xrefs.find_all('span', class_='xh')
        if xh_list or prefix_text:
            parts = []
            if prefix_text:
                parts.append(prefix_text + ':')
            for x in xh_list:
                parts.append(x.get_text(strip=True))
            if parts:
                b.open(PARA_HEAD, TAG_XREF, ' ' + ' '.join(parts))
                b.close()

    b.finalize()
    return bytes(out)
