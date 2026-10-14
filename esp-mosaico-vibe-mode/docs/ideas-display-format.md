# Ideas 自定义展示资源 v1

本格式用于发布时已经确定的应用名称和描述。云端为所选语言生成所需资源，设备
保存到 NAND，在下载确认页和离线信息页使用；不把字体写进应用 NOR 分区。
字体子集不是完整的系统字体，也不支持任意新增动态文本。改变文字、字号或语言
需要重新生成展示资源并发布新 release。

## 下载依赖

`mosaico-ideas/download/1` 在现有应用安装清单外声明：

```json
{
  "presentation": "display.json",
  "fonts": ["title.mif", "body.mif"],
  "icon": "icon.png"
}
```

以上文件必须全部出现在 `files` 中，具有独立的 `name/path/size/sha256`。
名称只能引用当前清单内的文件。固件、展示 JSON、字体和图标不能重复占用同一文件；
所有文件必须被引用。`fonts` 最多两个且都必须被文字块使用。字体不能再声明依赖，
因此没有循环或递归下载。总清单最多 12 个文件、8 个固件组件、32 MiB，展示资源
合计最多 1 MiB。安装用的 `esp-iris-system-update/0.2` 清单保持不变。

解析请求增加 `download_schema`、`display_schema`、`font_format`，值分别为
`mosaico-ideas/download/1`、`mosaico-ideas/display/1`、`MIFF/1`。

V0.2 尚未正式发布，直接修改 `/1` 开发契约，不新增 `/2`，也不保留旧 GSPB
清单兼容路径。下载清单、展示描述和字体文件的版本各自独立；正式发布并冻结后，
只有对应格式发生不兼容变更时才升级其版本。

云端和发布工具统一生成当前契约。旧 GSPB 清单与临时 `/2` NAND 测试包不会被
识别为当前可用 Ideas；需要输入六位编号重新下载，由完整新包替换。此变更不自动
迁移或清空 NAND，也不改变 NOR 中已有应用的启动目标。

## display.json

以下为结构示例，字形 ID 和位置必须由发布工具根据字体生成：

```json
{
  "schema": "mosaico-ideas/display/1",
  "locale": "zh-CN",
  "icon": "icon.png",
  "name": {
    "text": "天气",
    "font": "title.mif",
    "width": 328,
    "height": 84,
    "glyphs": [[14000, 2, 33], [23422, 30, 33]]
  },
  "description": {
    "text": "晴",
    "font": "body.mif",
    "width": 416,
    "height": 176,
    "glyphs": [[20500, 2, 24]]
  }
}
```

- UTF-8、最多 32 KiB；拒绝重复键、未知键、嵌入 NUL、深度超过 8、尾随数据。
- `text`、`locale`、`icon` 必须与外层 application 及 icon 字段一致；无图标时 `icon` 为 `""`。
- `font` 对应外层 `fonts` 的一个文件名，不能是 URL 或其他资源。
- 名称块最大 328×84，描述块最大 416×176；尺寸是排版区域，不是屏幕绝对坐标。
- `glyphs` 最多 512 条 `[glyph_id, x, baseline_y]`，坐标以该文字块左上角为原点、
  x 向右、y 向下，单位为整数像素。字形实际位图必须完整落在块内。非空文本须有字形。
- ID 是当前字体子集内的字形 ID，不是 Unicode 码点；连字、组合字符和双向排版
  已在发布端处理。设备按数组顺序混合位图，不重新排版 UTF-8 字符串。
- 当前固件使用固定文本区域；未来 UI 适配器可移动文字块和改变颜色。改字号或重排
  需要发布新的布局/字形，不能仅靠缩放标题获得高质量的任意字号。

## MIFF/1 字形文件

自定义二进制，扩展名 `.mif`，magic 为 `MIFF`。所有整数为小端序，不采用 C 结构体
内存布局。没有压缩、外部指针、执行代码或框架资源 ID。每文件最多 1 MiB。

24 字节文件头：

| 偏移 | 类型 | 内容 |
|---:|---|---|
| 0 | 4 bytes | ASCII `MIFF` |
| 4 | u16 | 版本 1 |
| 6 | u16 | 头长度 24 |
| 8 | u32 | 文件总长度，必须与实际相等 |
| 12 | u16 | 字形数 1–512 |
| 14 | u16 | 栅格化字号，1–96 px |
| 16 | u16 | 行高，1–192 px |
| 18 | u16 | 保留标志，必须为 0 |
| 20 | u32 | 字形表偏移，必须为 24 |

接着是每条 20 字节的字形记录，按 ID 严格递增：

| 记录内偏移 | 类型 | 内容 |
|---:|---|---|
| 0 | u32 | 非零字形 ID |
| 4 | u32 | 位图绝对文件偏移 |
| 8 | u16 | 位图宽，0–192 |
| 10 | u16 | 位图高，0–192 |
| 12 | i16 | 左侧 bearing，-192–192 |
| 14 | i16 | 基线上方 bearing，-192–192 |
| 16 | i32 | 水平 advance，26.6 定点数，范围 ±192 px |

位图按记录顺序紧跟字形表，每个字形为 `width * height` 字节、逐行 A8 灰度，
0 透明、255 不透明。空白字形宽高均为 0。拒绝间隙、重叠、未声明尾部数据，
不允许单边为零的尺寸。位图绘制起点为 `(x + left, baseline_y - top)`。
advance 和行高保留排版度量，当前固定文案的实际位置以 JSON 为准。

## 发布与渲染边界

`tools/ideas_font.py` 使用 FreeType 和
[libraqm](https://host-oman.github.io/libraqm/raqm-Raqm.html) 在发布主机完成字形选择和
双向排版；按 shaping cluster 换行，保存结果后设备不需要这些库。

`ideas_font.c` 负责字形表校验和查询，`ideas_presentation.c` 负责 JSON 依赖解析、
元数据一致性、字形边界验证和通用 RGB565 混合。这两部分只使用标准 C 和 cJSON，
可直接用于其他 UI。GSP 相关调用仅在 `vibe_ui.c` 中，接收普通像素和 PNG。

发布工具还生成 `presentation.json` 供云端组装清单，它不是设备资源。云端必须把
`display.json/title.mif/body.mif/icon.png` 及对应哈希纳入新 release 的 files。
设备先下载这些资源并校验，再显示确认页；取消只释放内存。确认后复用缓存写入
NAND，并下载固件；只有用户另行设置开机应用时才安装到 NOR。
