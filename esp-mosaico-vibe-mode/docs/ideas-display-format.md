# Ideas 自定义展示资源 v1

本文描述 `display/1`、`MIFF/1` 的内部格式。完整的上传、直接 GET 外层 manifest、
目录化下载及中英离线切换见[文件发布协议](../../mosaico-tools/docs/ideas-file-release/README.zh-CN.md)。

发布工具为 zh-CN 和 en 分别生成 `device/<locale>/display.json` 及相邻字体和图标。
设备预览先读所选语言；确认后保存并校验两套资源，完整目录才成为 current。
`display.json` 是语言、名称、描述、字体和图标的唯一来源，外层只声明文件路径、
大小、摘要。固件依赖来自 `firmware/manifest.json` 的原始 Iris 清单。

每种语言最多两个字体，所有声明资源必须恰好被引用，字体和图标使用当前语言
目录内的 basename。每语言资源总计不超过 1 MiB，双语合计 2 MiB；display 最多
32 KiB，图标为 64×64、8 bit RGBA、非交错静态 PNG，最多 64 KiB。
设备集合含根清单最多 32 个文件、32 MiB。完整限制以协议为准。

字形子集用于发布时确定的文本；文字变化须生成新的不可变 release。
系统固定文案使用独立内置中文字体，用户语言保存在 sysmeta 的 settings NVS，
首次默认中文；Idea 的默认语言不能覆盖该偏好。系统界面、列表名称及本地详情
均可离线切换。列表复用 name 字形块，不要求系统字体包含所有 Idea 字符。

不读取旧 download/1 或 V0.1，不批量迁移、删除已有 NAND 数据，也不因切换
语言改变开机目标。已有不支持格式或身份冲突的目录会保留并报告错误。

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
- `font` 对应当前语言 `assets` 中的一个字体 basename，不能是 URL 或其他资源。
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

`tools/prepare_idea_presentation.py --name ... --description ... --locale zh-CN
--font ... --icon ... --output DIR` 生成纯展示文件，向 stdout 输出文件描述，
不需要六位编号，也不生成 presentation.json。`project package` 调用相同生成器，
把两种语言的 display、字体及可选图标直接纳入 ZIP。

确认前只使用 RAM；取消释放缓存。确认后保留目录层级写入 NAND，并下载普通
应用固件。只有用户另行设置开机应用时才安装到 NOR。目录外 `.roots` 保存本地
提交日志与根摘要，不属于发布内容，不能由服务器覆盖。
