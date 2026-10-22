# 2026-10-18 UI handoff 端侧验证

参考版本为 `v0.3-ui-20261018-handoff`（`f0b53c0`），验证源码位于
`codex/vibe-ui-handoff-20261018` 工作树，包含进度百分比 Doto Black 字体修正。
本次完成真实 ESP32-S31 上的固件安装、设备 UI 交互和启动回归，未替换 reviewed prebuilt bundle。

## 标题比例与自动滚动补充验证

修正 Ideas 列表、本地详情与简介预览的长标题。原实现将已换行的标题位图压缩到
单行视口，导致字形变扁；现在根据已发布的字形与排版位置重排为单行，保留字形比例。
超过可用宽度时以 28 px/s 水平往返滚动，两端各停留 1.2 秒；短标题静止，重新进入页面
时从开头显示。标题滚动限制在各自视口内，元数据和操作按钮保持固定。
发布端显示资源、Recovery ABI 和持久化数据格式均不变。

- 固件 BIN：1,682,896 bytes；固定槽剩余 152,112 bytes；构建 0 warnings。
- BIN SHA-256：`66d9e59e182297136e7f91eaae5eac7daead6ff94dd9796d4f5d533ac5a336ff`。
- ELF SHA-256：`69a342a13fa772d54b9f2543695a4a70e2df53e3fc01d491c645ecded36a5c0d`。
- 资源大小：1,701,012 raw / 362,192 compressed bytes，保持原资源预算。
- Vibe Mode 全量主机测试 702 项通过（480.30 s）；收集用例后新增的中英文标题参数
  另由下述 5 项发布字形回归覆盖。Tools 测试沿用本次改动已通过的 205 项、24 项环境跳过。
- 中英文长标题的三页导航回归 2 项通过，验证滚动、返回复位和标题区外像素不变。
- 真实发布字形资源的重排回归 5 项通过，包含 TimeTiles 中文标题和英文长标题。
- ASan/UBSan：渲染器回归 1 项、Ideas worker 与缓存所有权回归 67 项通过。

使用实时布局摘要 `8d28cdded305dd5f31c4a2aec315e4577e8e55f8ade887775d543306e6eb59a6`
制作 recovery-only 更新包。操作 `00fae60d-d300-4f9e-a2a1-f94ef62549b9` 成功，
返回 `healthy: true` 和匹配的目标 ELF。Boot ID 从 `2353760214562721792` 变为
`16812782643044761989`；bootloader、分区表和 layout 5 保持不变。
`marquee-final-status.json` 确认 HEALTHY、崩溃计数 0、无待处理崩溃恢复。

用户在新固件安装后手动操作设备，并明确确认“我手动验证通过了”。自动化侧仅进入
管理页和截图；用户随后进入 TimeTiles 本地详情页。`marquee-library-start.png` 实际记录
的是该详情页，已核对标题按原比例单行显示；文件名不代表截图页面。
此确认记录为本次标题修复的人工验收，不扩展为未逐项记录的物理按键、旋转或触感测试。

构建日志：`.codex-runs/ui-handoff/marquee-device-build/20261018-200032-build-664925/raw.log`。
消费工作区证据：`marquee-inventory-before.json`、`marquee-update.json`、
`marquee-inventory-after.json`、`marquee-final-status.json`。所有设备操作均通过消费工作区
`mosaico.py` launcher；临时应用到其 pinned utils 的只读诊断补丁已还原，工作树干净，
本次验证持有的设备连接已释放。以下保留前两轮验证的历史结果。

## 详情布局与 Open 文案补充验证

修正云端/本地简介共用的详情页：名称进入顶部标题区；图标右侧依次显示状态、
版本与大小、ID；分隔线以下的简介独立滚动，底部操作按钮固定。保留已发布的
328×84 标题和 416×176 正文 RGB565 原尺寸，进入下一次预览时复位滚动位置。
打开确认按钮统一显示 `Open` / `打开`；保留未保存开发版本将被覆盖的提示和等待状态。

- 该轮固件 BIN：1,680,560 bytes；固定槽剩余 154,448 bytes；构建 0 warnings。
- BIN SHA-256：`da89d62c3a07fb997ddb822ca97585f9ffc1961e2b7a22bbfb84c85554d6922c`。
- ELF SHA-256：`8df8dd9503c3a371abed67fff3dc806837c155f92a43598b84e61655eea7c953`。
- 资源大小：1,701,012 raw / 362,167 compressed bytes，保持原资源预算。
- UI、资源和字体相关回归 12 项通过；其余 Vibe Mode 主机测试 641 项通过。
- Tools 测试 205 项通过、24 项环境相关跳过。

设备在本轮安装前已使用不同于首轮的应用分区表；按首轮布局制作的包被预检拒绝，
未发生写入。为经由消费工作区 launcher 取得实时证据，增加只读
`iris device-status --inventory`，同时检查读取前后 Boot ID 一致。
使用官方 `build_system_update_bundle` 的 recovery-only manifest 接口，以实时分区摘要
`8d28cdded305dd5f31c4a2aec315e4577e8e55f8ade887775d543306e6eb59a6`
作为包的完整布局前置条件；未更改校验规则，包中只有 recovery 组件。

成功更新操作 `be6bcb69-cbbe-4928-8e20-9ed4693eb796` 返回 `succeeded`、
`healthy: true` 和匹配的目标 ELF。Boot ID 从 `12757816258885122060` 变为
`2353760214562721792`。安装前后分区表与 bootloader 摘要一致。
本轮未安装或打开其他应用、删除 Idea、保存开发版本或修改共享条目。

构建日志：`.codex-runs/ui-handoff/detail-device-build/20261018-193540-build-655125/raw.log`。
消费工作区设备证据：`detail-inventory-before.json`、`detail-layout-update-live.json`；
`detail-inventory-after.json` 确认新固件健康、崩溃计数 0。

- `open-label-en-ready.png`、`open-label-zh.png`：实机按钮分别为 Open / 打开，
  覆盖未保存开发版本的警告仍在；取消后回到详情页，未触发应用安装。
- `detail-layout-local-description.png`、`detail-layout-local-scrolled.png`：使用设备中
  Idea 200046 的已保存原生显示资源，版本 1.12.2、2508 KiB、ID 分行显示。
  滚动前后逐像素比较确认顶部 228 px 和底部操作区不动，只有正文视口变化。
- 云端输入 200046 后的加载页与取消返回已验证；本轮取消了仍在加载中的云端请求。
  云端成功、失败和离线显示的共用布局与控制流程由主机回归覆盖。

最终恢复本轮安装后观察到的简体中文设置，停留在 TimeTiles 简介页，滚动位置复位到顶部。
最终截图为 `detail-layout-final-zh.png`，`detail-final-status.json` 确认目标 ELF、HEALTHY 和崩溃计数 0。
消费工作区的 pinned utils 诊断补丁仅用于这次 launcher 验证，已还原，工作树保持干净。
下文保留首轮 UI 与正常应用往返验证的历史结果。

## 首轮固件与设备

- Device ID：`4553502d49524953010030eda0f40c10`；MAC：`30:ed:a0:f4:0c:10`。
- ESP-IDF：`7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe`，保留环境中已有的 camera 修改。
- 增量构建及 `recovery-self-update-bundle` 通过，0 warnings。
- BIN：1,681,264 bytes；1.75 MiB 固定槽剩余 153,744 bytes。
- BIN SHA-256：`568986c113a848765780b4fb9a4779ed31cda674c31907f39dfcb595424f33fa`。
- ELF SHA-256：`8e4ca253abb7d3cee292f1035de88fce0043be21366b205091a2d94ab94f1911`。

所有设备读写、输入和截图均通过消费工作区 `esp-mosaico-vibe/mosaico.py`。
使用 `projects/submodule_check_blank` 的 Gateway 作为连接入口，目标由 Device ID 明确指定；
设备原应用是 `submodule_check_ui`，没有安装 Gateway 工程的应用。

## 安装与数据保留

默认自更新包首先因完整分区表摘要不匹配，在写入前被拒绝。核对原应用 ELF 与实时设备身份一致，
并确认该应用分区表的保留前缀与本次 Vibe Mode 构建完全一致后，使用官方制包器的
`--target-layout` 指向原应用已有的 `build/partition_table/partition-table.bin`，
生成适用于设备现有布局的单组件包。设备再次核对摘要并通过预检；未修改或写入分区表。

成功更新操作：

- 安装新 UI：`519e39fa-25c0-4a24-bdf9-81f0c2ecc7f1`。
- 在新 UI 中验证真实更新进度：`3261f844-1a98-4545-820a-980711a744be`。

两次操作均只写入 `recovery` 组件，返回 `succeeded`、`healthy: true` 和一致的 ELF 身份。
第二次使用同一镜像，用于捕获新字体在设备真实传输期间的画面。
bootloader 摘要 `94b651146fc9c381900dfb4e7c96df124945d9bf9ebec52c2b82cee1b19cc2c4`、
分区表摘要 `3c26b245870110b955f490e07fe047309b44b098c5adce18a449b1010bb2fd12` 和 layout 5
在更新前后均一致。工具自动留存了设备原有的 19,008-byte Core Dump；当前运行的崩溃计数为 0。

原有 3 个 Ideas 和 3 个共享条目在验证后仍可读取，密码仍遮蔽显示。
未执行应用删除、恢复出厂、应用安装或保存开发版本；Wi-Fi 密码输入和新增共享条目均取消退出。
中英文切换后恢复原来的英文。已有 Wi-Fi 在两次自更新及应用往返启动后自动连接。

## UI 交互结果

通过 USB 指针 RPC `0x1001/1` 输入真实设备 UI 队列，并使用 launcher 的 `iris screenshot`
抓取设备帧。以下文件名相对于消费工作区的 `.codex-runs/ui-handoff-device/`。

| 检查 | 结果与代表性证据 |
| --- | --- |
| 首页、创客动画、首页/Ideas 往返 | 页面和动画正常；读取应用库后显示真实数量 3；`home-after-grid.png`、`final-home.png` |
| 开发页与指南 | 显示真实 Device connected / Agent ready；指南可打开和返回；`develop-en.png`、`develop-guide.png` |
| 设置滚动、显示设置 | 上下滚动和返回正常，原亮度/熄屏/触感设置保持；`settings-scrolled.png`、`display-scrolled.png` |
| 中英文切换 | 中文首页、设置与选中状态正常，恢复英文并跨重启保留；`home-zh.png`、`settings-zh.png` |
| Wi-Fi、密码键盘 | 扫描显示 15 个网络；进入加密网络密码页、按键预览和取消正常，原连接保留；`wifi-scan-complete.png`、`password-page.png`、`password-key-preview.png` |
| 共享信息 | 列表、详情、帮助、新键草稿、按键预览和两页符号键盘正常；取消后条目不变；`shared-detail.png`、`shared-help.png`、`keyboard-key-preview.png`、`keyboard-symbols-page2.png`、`shared-after-reboot.png` |
| Ideas 管理和网格 | 原有 3 个应用可读，元数据与存储统计更新，网格可返回首页；`manage-settled.png`、`ideas-grid.png` |
| 打开确认 | 正确提示未保存开发版本将被替换，取消后仍在 Vibe Mode；`open-warning.png` |
| 下载入口、帮助、关于和许可证 | 页面打开、滚动、返回正常；`download-page.png`、`download-help-page.png`、`about-en.png`、`licenses-en.png` |
| 真实更新百分比 | 自更新接收时捕获 4%，主百分比使用 Doto Black 88 px 点阵字形，未裁切；`device-progress-00.png` |

应用库沿用按需发现策略：首次打开库之前首页数量为 `--`，发现完成后显示实际数量。
进度捕获期间，擦写/重启窗口内部分截图请求不可用；没有重试写入，原更新操作最终完成。
动作原始记录在 `actions.jsonl`，截图旁的 JSON 保存设备身份、操作 ID 和图片元数据。
个别探索截图的文件名不对应最终页面；上表仅列已打开核对的正确证据。

## 首轮启动与完成状态

| 阶段 | Boot ID | 状态 |
| --- | --- | --- |
| 更新前原应用 | `8845428293361575171` | normal / `submodule_check_ui` / HEALTHY |
| 更新前 Vibe Mode | `12968040962255561314` | recovery / HEALTHY |
| 新 UI 第一次启动 | `15896449763278088103` | recovery / 目标 ELF / HEALTHY |
| 进度验证后启动 | `7923860043253190511` | recovery / 目标 ELF / HEALTHY |
| 再次启动原应用 | `12481116710031992937` | normal / 原 ELF / HEALTHY |
| 返回新 Vibe Mode | `7158579697574644018` | recovery / 目标 ELF / HEALTHY |

原应用 ELF 始终为 `1f9d43e82d9393396e9e991c28d9e184c807a5433643b732c78ace847c51f16a`，
`normal-after-update.png` 确认其 Hello World 界面。首轮完成时设备停留在英文 Vibe Mode 首页，
崩溃计数 0，无待处理崩溃恢复，最后一次读取内部空闲内存 163,251 bytes、PSRAM 空闲 11,456,244 bytes。

主要结构化证据：`update-current-layout.json`、`update-progress-validation.json`、
`restart-normal.json`、`normal-after-update-status.json`、`return-vibe.json`、
`final-status.json`、`final-network.json`、`final-memory.json`、`final-logs.jsonl`。
构建日志在 utils 的
`.codex-runs/ui-handoff/device-build/20261018-190151-build-642581/raw.log`。

## 验证边界

首轮与详情布局轮主要为设备端自动化验证；标题修复另有上文记录的人工验收。
USB 注入不能替代人工手指快速滑动、物理按键、振动体感、
IMU 旋转或直接观察面板的验收。没有执行清空数据/恢复出厂、删除、云端下载写入或 NAND 应用替换。
在线固件更新专用的 32 px 百分比布局保留主机测试与构建验证，本轮实机捕获的是通用更新页的 88 px 百分比。
多 Wi-Fi、应用置顶、共享默认条目继续不在本次范围内。
