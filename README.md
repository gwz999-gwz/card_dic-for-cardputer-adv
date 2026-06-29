# Cardputer 电子词典

基于 [M5Stack Cardputer](https://shop.m5stack.com/products/m5stack-cardputer-kit-w-m5stamps3) (ESP32-S3) 的离线英汉电子词典固件。词典数据与字体存放于 SD 卡，固件按需读取，支持中英文混合显示、模糊查询、分页浏览与多种显示模式。

**v10 新增**：支持外接 2.8" ILI9341 屏幕（320×240，16 行），按键 / SD 卡 / 选词跳查全部正常工作。

![cardputer-adv](https://img.shields.io/badge/device-M5Cardputer--ADV-blue)
![esp32-s3](https://img.shields.io/badge/ESP32--S3-8MB%20Flash-orange)
![platformio](https://img.shields.io/badge/build-PlatformIO-green)
![offline](https://img.shields.io/badge/dict-offline-lightgrey)

## 特性

- 🔍 **离线查询**：词典与字体存 SD 卡，无网络依赖
- 🎯 **模糊匹配**：前缀优先 + 包含兜底，最多 8 候选
- 📖 **多分页**：按 `;` `.` 翻页（环绕），状态栏显示 `n/m`
- 🌐 **模式切换**：按 `Alt` 循环 `ALL → CN → EN`
- 🎨 **选词跳查**：按 `Ctrl` 进入，方向键导航，`Enter` 查词
- 🖥️ **双屏支持**：内屏 ST7789 (240×135) / 外屏 ILI9341 (320×240)，编译时切换
- 💾 **PSRAM 优化**：字典内容 32 MB 不驻留，字体索引 47 KB 落 PSRAM
- 🔧 **字形编辑**：自带浏览器端 `font_editor.html`，可视化补缺

## 硬件

### 主控
- M5Stack Cardputer (M5StampS3 + Cardputer 底座)
- ESP32-S3, 8 MB Flash, OPI PSRAM
- microSD 卡（建议 ≥ 64 GB, FAT32）

### 屏幕（编译时切换）
| 模式 | 屏幕 | 分辨率 | 行数 | 触发宏 |
|------|------|--------|------|--------|
| 内屏（默认） | ST7789 | 240×135 | 9 | `SCR_USE_EXTERNAL=0` |
| 外屏 | ILI9341 2.8" | 320×240 | 16 | `SCR_USE_EXTERNAL=1` |

### 外屏接线

```
ILI9341:   CS  RST  DC  MOSI  SCK  LED
Cardputer: G5  G3   G6  G14   G40  G4
SD 卡:    CS→G12  MOSI→G14  SCK→G40  MISO→G39
VCC → 5V_OUT → AMS1117-3.3 → 3.3V
GND → GND
```

## 快速开始

### 1. 准备 SD 卡
在 SD 卡根目录建立 `dictcard/` 子目录，将 `dict.bin` / `dict.idx` / `font.bin` 三个文件复制进去（仓库不含词典数据，需自行生成）。

### 2. 修改烧录端口
编辑 `device/platformio.ini`：
```ini
upload_port = COM5    # 改成你的实际端口
```

### 3. 切换屏幕模式（可选）
编辑 `device/src/config.h`：
```c
#define SCR_USE_EXTERNAL  1   # 0=内屏 240x135  1=外屏 320x240
```

### 4. 编译烧录
```bash
cd device
pio run --target upload
```

### 5. 上电使用
按任意键跳过开屏介绍进入查询界面。

## 按键说明

| 键 | 功能 |
|----|------|
| 字母键 / 数字 / 符号 | 输入查询 |
| `Enter` | 执行查询 / 进入候选词详情 |
| `` ` `` | 一键清空 |
| `Alt` | 切换显示模式（ALL / CN / EN） |
| `;` `.` | 详情页翻页（环绕）|
| `,` `/` | 详情页前一词 / 后一词 |
| `Ctrl` | 进入选词跳查（4 方向智能导航）|
| `Fn+Q` | 清空输入 |
| `DEL` | 退格 |

状态栏左侧：当前 headword（亮黄）
状态栏右侧：`n/m` 页码 → 电量 → 模式

## 显示模式

按 `Alt` 循环切换，详情页内容按段落过滤：

- **ALL** — 全部内容
- **CN** — 仅中文释义（隐藏英文释义与例句）
- **EN** — 仅英文释义（隐藏中文释义与例句）

## 文件结构

```
.
├── device/                  # 设备端固件 (PlatformIO)
│   ├── platformio.ini
│   ├── src/
│   │   ├── main.cpp         # 主程序 / 按键处理 / 状态机
│   │   ├── display.*        # 显示引擎：中英混排 / 分页 / 选词高亮
│   │   ├── dict_engine.*    # 词典引擎：bucket 索引 + 模糊查找
│   │   ├── font_loader.*    # CJF1 字体懒加载
│   │   ├── color_config.h   # 颜色 / 段标记常量
│   │   ├── config.h         # 屏幕切换 / 布局参数
│   │   └── external_display/  # v10 外屏 ILI9341 驱动
│   └── data/                # SD 卡数据 (/dictcard/)
│       ├── dict.bin         # 词典主体（二进制）
│       ├── dict.idx         # bucket 索引
│       └── font.bin         # CJF1 格式字体
└── tools/                   # 词典 / 字体构建工具
    ├── mdx_to_dict.py       # MDX → dict.bin / dict.idx
    ├── html_parser.py       # MDX HTML 内容解析
    ├── build_font_ext.py    # TTF → font.bin (CJF1)
    ├── font_extract.py      # 从内置 font_builtin_data.h 抽字形
    ├── font_editor.html     # 浏览器端字形编辑器
    ├── dict_verify.py       # dict.bin 一致性校验
    └── scan_missing_chars.py # 扫描字体缺失字符
```

## 词典与字体制作

```bash
# 1. MDX → dict.bin / dict.idx
python tools/mdx_to_dict.py path/to/source.mdx device/data/

# 2. TTF → font.bin (CJF1)
python tools/build_font_ext.py tools/chinesefront-12px.ttf device/data/font.bin

# 3. 校验词典一致性（精确 / 模糊查找回归）
python tools/dict_verify.py device/data/dict.bin device/data/dict.idx

# 4. 扫描字体缺失字符，生成补字 CSV
python tools/scan_missing_chars.py
```

浏览器打开 `tools/font_editor.html` 可视化补字 / 调字。

> ⚠️ 牛津词典 MDX 文件需自行下载（推荐牛津高阶英汉双解第 10 版 OALDpe）。

## 性能要点

- `dict.bin` 全程保持一个打开句柄，避免每次按键 3–4 次 `SD.open`（单次 5–15 ms）
- 字体懒加载：index 常驻 PSRAM（~47 KB），位图按需从 SD 读 + 128 项 LRU 缓存
- bucket 索引二分查找；模糊匹配先前缀后包含
- 渲染走 PSRAM 顺序读，layoutContent 一次性摊平所有页


## 许可

本仓库仅包含固件源码与工具脚本。词典数据版权归原作者所有。
