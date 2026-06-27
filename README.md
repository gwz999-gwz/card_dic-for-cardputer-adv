# Cardputer 电子词典

基于 [M5Stack Cardputer](https://shop.m5stack.com/products/m5stack-cardputer-kit-w-m5stamps3)(ESP32-S3) 的离线英汉电子词典固件。词典数据与字体存放在 SD 卡,固件按需按页读取,支持中英文混合显示、模糊查询、分页浏览与多种显示模式。

## 项目结构

```
.
├── device/                  # 设备端固件 (PlatformIO)
│   ├── platformio.ini
│   ├── src/
│   │   ├── main.cpp         # 主程序 / 按键处理 / 状态机
│   │   ├── display.*        # 显示引擎: 中英混排 / 分页 / 选词高亮
│   │   ├── dict_engine.*    # 词典引擎: bucket 索引 + 模糊查找
│   │   └── font_loader.*    # CJF1 字体加载
│   └── data/                # SD 卡数据文件 (/dictcard/)
│       ├── dict.bin         # 词典主体 (二进制)
│       ├── dict.idx         # bucket 索引
│       └── font.bin         # CJF1 格式字体
└── tools/                   # 词典 / 字体构建工具
    ├── mdx_to_dict.py       # MDX → dict.bin / dict.idx
    ├── html_parser.py       # MDX HTML 内容解析
    ├── build_font_ext.py    # TTF → font.bin (CJF1)
    ├── font_extract.py      # 字体提取
    ├── font_editor.html     # 字体可视化编辑器 (浏览器)
    ├── dict_verify.py       # dict.bin 一致性校验
    └── scan_missing_chars.py # 扫描词典中出现但字体缺失的字符
```

## 硬件

- 设备: M5Cardputer (M5StampS3 主控)
- 芯片: ESP32-S3, 8 MB Flash, OPI PSRAM
- 存储: microSD 卡 (建议 ≥ 64 GB, FAT32)

## 烧录

1. 在 SD 卡根目录建立 `dictcard/` 子目录,把生成的 `dict.bin` `dict.idx` `font.bin` 三个文件复制进去(本仓库不含词典数据,需自行生成,见下文)。
2. 按需修改 `device/platformio.ini` 的 `upload_port`(默认 `COM5`)。
3. 编译并烧录:

   ```bash
   cd device
   pio run --target upload
   ```

4. 上电后按任意键,跳过开屏介绍进入查询界面。

## 按键说明

| 键                | 功能                              |
|-------------------|-----------------------------------|
| 字母键            | 输入查询                          |
| Enter             | 执行查询                          |
| `` ` ``           | 清空输入                          |
| Alt               | 切换显示模式 (ALL / EN / CN)      |
| `;` `.`           | 详情页翻页                        |
| `,` `/`           | 详情页前一词 / 后一词             |
| Ctrl              | 进入选词跳查 (4 方向智能导航)     |

状态栏左侧显示当前 headword,右侧显示模式、电量与页码 `n/m`。

## 显示模式

Alt 键循环切换,详情页内容按段落过滤:

- **ALL** — 全部内容
- **EN** — 仅显示英文释义
- **CN** — 仅显示中文释义

## 词典与字体制作

```bash
# 1. MDX → dict.bin / dict.idx
python tools/mdx_to_dict.py path/to/source.mdx device/data/

# 2. TTF → font.bin (CJF1)
python tools/build_font_ext.py tools/chinesefront-12px.ttf device/data/font.bin

# 3. 校验词典一致性 (精确 / 模糊查找回归)
python tools/dict_verify.py device/data/dict.bin device/data/dict.idx

# 4. 扫描词典中出现但字体缺失的字符,生成补字 CSV
python tools/scan_missing_chars.py
```

浏览器打开 `tools/font_editor.html` 可视化补字/调字。
请自行下载牛津10版.mdx文件

## 性能要点

- `dict.bin` 全程持有一个打开句柄,避免每次按键 3–4 次 `SD.open`(ESP32 上单次约 5–15 ms)
- 详情页行级缓存通过 PSRAM 分配(约 72 KB),释放主 SRAM
- bucket 索引二分查找;模糊匹配先前缀后包含,最多返回 8 个候选
- 渲染走 PSRAM 顺序读,layoutContent 阶段一次性摊平所有页

## 许可

本仓库仅包含固件源码与工具脚本。