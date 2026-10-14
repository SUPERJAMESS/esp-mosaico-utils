# Vibe Mode GSP 验证记录（2026-09-20）

状态：候选验证中，真机端到端验收未通过，原评审预编译包保持不变。
工作分支：workspace、utils 和 BSP 均为 `codex/vibe-mode-gsp`。

## 已完成

- ESP-GSP 1.2.0 / GSPC 0.3.0，同一场景和 C 控制器运行在设备与 PC 后端。
- ESP-IDF v6.2-dev-2221-g7b9cc1ac79f，ESP32-S31；全新
  `build-vibe-validation` 构建通过，构建组件列表不包含 LVGL 或 esp_lvgl_adapter。
- 固定 factory 槽 0x20000 / 0x1c0000 未更改；增加专用构建检查，
  超过该槽时直接构建失败，避免 IDF 只检查较大的 ota_0 槽。
- 最终 Recovery 主机套件 277 项通过（116.47 秒），其中原生 UI 扩展套件 6 项通过。
  包括密码 64 字节上限、隐藏/显示与退出清理、下载配网续接及取消、
  普通 Wi-Fi 入口、重复打开配对会话、连接失败重试、空列表、
  NAND 选择确认、更新进度及成功/失败返回。服务由 PC 模拟。
- Recovery 镜像源码通过屏幕缓冲生命周期主机测试；恢复契约 5 项通过。
- 原生 sim_bridge 截图由测试直接保存，未用浏览器页面替代设备 UI。

## 首次真机候选

Device ID：`4553502d49524953010030eda0f4518e`。
旧 Recovery Boot ID：`3661398145707442803`。
当前普通应用布局区域 SHA-256：
`4f8786d12001684a3c02385f31ef167247827bd73b744801494a8c5851bc7231`。
该哈希来自 Gateway 实时 inventory，等于 Hello World 分区表补齐至 4096
字节后的哈希。自更新包明确使用该布局前置条件，只更新 factory 组件。

首次候选 1,763,936 字节，经 `mosaico.py iris system-update` 完成传输和
读回校验，操作 `1b78d20d-6fe1-4c6c-bf39-9c32e862b1b2`。
设备重启后 USB 反复断开，45 秒内未达到健康状态，Gateway 结果为
`outcome_unknown`，不能视为安装验收成功。
Gateway 在写入前保留了已有 core dump；它不是此次启动故障的诊断证据。

源码检查发现解压便捷函数会在调用栈上放置约 11 KiB 解码状态，
超过 Recovery 主任务 3.5 KiB 栈。已改为使用 PSRAM 上的解码状态与
ROM 流式 API；UI 初始化返回错误时保持 USB 维护服务运行，并不报告健康。
修正版及完整候选包已构建并通过 manifest 校验，镜像 1,764,160 字节，
槽位余量 70,848 字节。修正版于 9 月 20 日经 ROM 恢复成功，随后
Recovery 自更新也成功，见下文。

## ROM 恢复与 Recovery OTA

- 用户完成手动 ROM 进入后，经 `mosaico.py recover --source current
  --hardware-mac 30:ed:a0:f4:51:8e` 安装修正版；同一 Device ID 恢复健康，
  Boot ID 为 `15927827128776812734`。未擦除整片 flash，已保存的 Wi-Fi
  配置保留，设备取得 `192.168.1.241`。
- 通过 `mosaico.py iris system-update --bundle .../factory-recovery-update.irisfw`
  验证仅更新 factory 的 OTA 路径。操作
  `e49b648f-fd62-4b52-8326-6ea981e7f877` 成功，健康状态为 true，
  新 Boot ID 为 `9270987682915392839`。
- ROM 恢复后的基础分区表区域 SHA-256 为
  `6f0d33a90336cf8a5361aadf787d819ab53e741d8f3c6614f0b8d40d28000fff`；
  后续 factory OTA 保持该表和 bootloader 不变。恢复普通应用时须用
  system-update 安装应用布局及资源，不能把基础表视为原应用布局。
- Gateway Web 工作台已显示设备镜像和成功更新记录；完整的普通应用
  往返及各阶段 CLI/Web Boot ID 对照仍待完成。

## Wi-Fi 列表真机反馈与修复

用户在实体屏幕发现列表堆叠。原列表高度 202 像素而每项 76 像素，
第三张卡片被裁切，相邻卡片没有间距。现使用 216 像素列表、72 像素
行距及 64 像素卡片，完整显示三项并留出 8 像素间隔；底部说明移至
列表之外。移除页面层内重复的全屏黑色背景，仅更新切换涉及的页面
可见性，并让 PC 与设备使用同一 RGB565 编译配置。

- 增加原生截图像素回归，检查标题、返回、Forget、Rescan 字形、
  卡片间隔和底部边界，以及反复导航和扫描。UI 套件 **7 项通过**
  （93.57 秒）；较早完整 Recovery 套件为 277 项通过。
- 真机 OTA 操作 `f8a5ff63-5fd6-4363-b235-ec9751027f3e` 成功，
  Boot ID `2721141969125079751` → `17611170269542912448`，healthy=true。
  ELF SHA-256 为 `f261ca616d3640e4fd96cfeb45ec3f8b644be46850a2971e0e115f88fe0b68d8`。
  镜像 1,763,040 字节，固定槽余量 71,968 字节。
- Gateway 输入完成五轮首页/Wi-Fi/密码页/返回，及上下滚动、重新扫描。
  十张循环截图和最终截图中的文字、卡片间隔均完整；设备留在 Wi-Fi 页。
  实体屏幕复验反馈仍待用户确认。
- 更新后的初期两张截图曾出现暂时缺字，后续稳定截图和上述循环未复现。
  尚不能将其归因于某个 GSP 编译选项，也不能以这些截图宣称已排除
  所有瞬态刷新问题；最终发布前应继续核验冷启动和实体屏幕。

证据：`.agents/vibe-gsp/wifi-refresh-regression-tests.log`、
`wifi-refresh-fix-device-update.log`、`wifi-real-cycle-*.png`、
`wifi-real-return-*.png`、`wifi-fixed-final.png`。

## 主页字号调整

ESP-MOSAICO 从 24px 调整为 20px，Vibe Mode 从 40px 调整为 32px，
保留居中布局与原有控件位置。原生 sim_bridge 预览和设备候选构建通过；
镜像 1,762,208 字节，槽内余量 72,800 字节。
OTA 操作 `ea85e679-09d4-4ae8-944e-8ace92ef5b6c` 成功，healthy=true，
Boot ID `17611170269542912448` → `6018478589450562622`。
ELF SHA-256：`91d46d976c894888d7adce6596ce5eb11e72415a573ef6828198a596b0e10b13`。
证据：`.agents/vibe-gsp/home-type-preview/home.png`、
`home-type-build.log`、`home-type-device-update.log`。

## BSP 默认 LVGL 构建检查

`examples/hmi_demo` 保持 `BSP_DISPLAY_LVGL_ENABLE=y`，BSP 源码编译通过；
完整示例构建被 esp_lvgl_adapter 对新 LVGL 旧头文件的引用阻塞，
触发 `-Werror=cpp`。这是本次解析到的依赖组合问题，尚不能宣称完整
默认 LVGL 示例构建通过；未为此修改示例或压制诊断。

## 尚未完成的真机验收

- 实体触摸与键盘完整复验、USB 快速输入、TCP 输入拒绝；Gateway
  镜像和基本 USB 导航已验证。
- 真实 Wi-Fi / Bridge 配对、联网下载与更新、NAND 流程。
- 同一设备 normal → Vibe Mode → normal，新 Boot ID 和 CLI/Web 操作记录一致。
- 验收后才运行 `update-recovery-prebuilt`，整体校验并发布默认包。

宿主证据位于 `.agents/vibe-gsp/` 与 `.codex-runs/`；它们不是发布包内容。
