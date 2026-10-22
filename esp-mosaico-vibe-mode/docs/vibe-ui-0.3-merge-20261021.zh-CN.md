# Vibe UI 0.3 迁移分支合并记录

日期：2026-10-21。目标分支为 `feat/261020`。

## 来源与比对

- 来源：[feat/vibe-ui-0.3-migration-20261021](https://glab.espressif.cn/intern-projects/esp-mosaico-vibecoding/vibe-mode-0.3/-/tree/feat/vibe-ui-0.3-migration-20261021)，提交 `099c99cae042a44125bd896dab03ca9625f58647`。
- 本地远端名：`vibe-mode-0.3`，使用该 GitLab 的 SSH 端口 `8266`。
- 合并前目标：`4670b46`；本地备份分支：`codex/backup-feat-261020-before-ui-migration-20261021`。
- 迁移分支从 rebase 前的 `9e65e79` 开始增加 31 个提交，其中 27 个为非合并提交，净改动 102 个文件。

目标分支已经把旧功能序列 rebase 到 main，`9e65e79` 对应的新提交为
`e801880`。用 `git range-diff` 核对旧、新功能序列后，以 `9e65e79` 为迁移增量
基线进行三方内容合并，最终仍保留目标和来源两个真实父提交。
这样不会因旧历史的重复冲突覆盖目标分支后续修复。

| 范围 | 合入内容 |
| --- | --- |
| UI 与交互 | 首页、Ideas、下载键盘、应用详情、帮助、共享信息编辑和重置进度；AI 键导航、滑动反馈、圆角图标及触摸震动调整 |
| 设备状态 | 电池有效读数与图标、BQ27220 读取失败保护、CST9220 无效触摸报告保护 |
| 默认设置 | 新配置默认不自动熄屏、不自动旋转；读取已有保存的设置 |
| 主机接口 | 文件下载在完成设备 CLOSE 后发送最后一块；本地系统更新等待 data link 就绪 |
| 保留的目标修复 | Windows USB/串口并发、更新验收、进度采样、capture 历史与 `mosaico.ai` 域名迁移 |

固件版本沿用来源的 `0.3.7`；ESP-Iris 和 ESP-Mosaico Tools 仍为 `0.2.0`，
Recovery ABI 2、布局版本 5 及固定槽保持原契约。

字体生成器同时保留来源的补充字形机制与目标分支的 UTF-8 读取修复。
布局、字形、ID 及嵌入包按合并后的源码重新生成，资源包为 364,149 字节，
82 项来源/产物摘要检查通过。新增的 `ui/assets/qr/ideas.png` 原为旧 Ideas
域名，已改为 `https://mosaico.ai/`；现有二维码生成器同步维护两种尺寸。
两份二维码均通过解码核对，旧尺寸图片保留原文件字节。

## 验证

- Tools/Vibe 联合全量初次运行：1073 passed、26 skipped、1 failed。
  唯一失败是来源分支未同步更新的保存取消测试：离开详情页会先取消自动简介预览，
  随后取消保存时累计应为两次。补充对第一次取消的检查并将总数改为 2 后，
  保存取消/重试与简介退出两项定向复测均通过（2 passed），未改动产品实现。
  初次失败日志仍保留；未把定向复测写成又一次全量通过。
  跳过项为两个 Windows 专用用例和 24 个需显式启用的真实 pre-commit 环境用例。
- ESP-Iris 主机测试：868 passed；从 `ESP-Iris/components/esp_iris/tools` 运行，排除设备 E2E。
- UI 渲染器 ASan/UBSan：1 passed，覆盖资源生命周期和重复输入。
- 抽查模拟器首页、中文编号校验提示及下载确认页，字体和布局显示正常；截图保存于验证目录的 `screenshots/`。
- 变更的 Iris Python 文件通过 Ruff 与 Python 3.8 语法检查。
- 暂存源码导出目录通过仓库布局检查；UI 来源摘要与 `git diff --check` 通过。
- ESP32-S31 固件在全新构建目录编译通过，版本为 `0.3.7`，QIO、Recovery ABI 2。

构建使用现有 ESP-IDF 6.2 checkout，提交
`7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe`；该 checkout 原有三个
`esp_driver_cam/dvp` 文件的本地改动，本次没有修改 SDK。
镜像为 1,685,936 字节，`0x1c0000` 固定槽剩余 149,072 字节。
固定槽大小及 USB/Iris 内部 BSS 检查均通过。
镜像 SHA-256：`d2fad9a5175becad1f8e5c7b94b216f1a0bea37c0db297b6c7d6d0a465f4b838`。

本地原始测试日志、JUnit 和构建产物保存在
`/tmp/mosaico-ui-merge-20261021/`，不作为发布包提交。
此次没有进行设备写入、设备验收或 Windows 回归，也没有替换
`prebuilt/recovery`。本次主机测试和构建结果不替代实体触摸、面板、电池、
重启与更新流程的设备验证。
