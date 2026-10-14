# ESP-174：USB OTA 吞吐优化与实机验收

验证日期：2026-09-28。问题：[ESP-174](https://linear.app/loop233/issue/ESP-174)。

## 原因与修改

USB app-update 原先使用 `OTA_WITH_SEQUENTIAL_WRITES`，在 DATA 中逐扇区擦除。
约 3968 字节的一次请求几乎总会等待一次 4 KiB 扇区擦除。改为在服务任务的
BEGIN 中传入已校验的镜像长度，按镜像范围预擦除，让 Flash 驱动使用块擦除。
协议任务仍可处理状态和取消；取消在擦除返回后生效，主机沿用已有 120 秒
BEGIN 超时。擦除失败也会释放 IDF 已分配的 OTA handle，允许重试。

USB system-update 原先每 1024 字节等待一次写入确认；Vibe Mode 现在协商
3968 字节块。加上 24 字节 DATA 头是 3992 字节，仍小于协议 4000 字节上限。
可复用组件的默认值仍为 1024；旧固件继续通过 BEGIN/COMPONENT_BEGIN 协商
较小块长。保留单请求执行、顺序 offset、SHA-256、提交及重启验收语义。

首次优化实测还发现 Gateway 重连退避在成功握手后没有重置，多次正常重启会
累计到 5 秒等待。修复后，只有连续连接失败才指数退避；握手成功即重置。

Wi-Fi 配对码／网页上传由设备通过 HTTP 下载，绕过 Iris DATA 往返；不是
Iris TCP 与 USB 的物理带宽对比。本次没有重新测量 Wi-Fi 吞吐。

## 可比条件与结果

- 同一 ESP32-S31，Device ID `4553502d49524953010030eda0f46056`，
  MAC `30:ed:a0:f4:60:56`，USB Highspeed，endpoint `usb:location=1-8:1.0`。
- 原 Vibe Mode 为审核包 `0.1.4`；优化固件仍为 `0.1.4`，通过 ELF 哈希区分。
- 所有应用更新使用同一 `iris_acceptance` 镜像，515,312 字节（约 503 KiB）；
  System Update 加 4096 字节分区表，总有效字节 519,408。
- 固定 IDF `7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe`，Python 3.12.3。
  构建保留环境原有 DVP camera 修改，IDF 描述如实带 `-dirt`。
- 速率取 Gateway 持久化 operation 事件中同一组件首末 DATA 进度的字节差／
  时间差；排除组件切换、擦除与重启。总耗时取 started_ns 至 finished_ns，
  包含健康验收，排除编译和 CLI 前置检查。单位为 KiB/s。

| 更新路径 | DATA 速率：基线 → 最终 | 完整操作：基线 → 最终 |
| --- | --- | --- |
| app-update，normal → Vibe Mode → normal | 67.5 → 235.9、254.8 KiB/s | 15.849 → 12.085、11.872 秒 |
| system-update，从已就绪 Vibe Mode 开始 | 164.0 → 256.6 KiB/s | 9.409 → 8.462 秒 |

app-update 基线的“擦除 + 传输”为 0.125 + 7.341 = 7.466 秒；最终两轮为
1.746 + 2.104 = 3.850 秒、1.701 + 1.946 = 3.647 秒，实际写入阶段约减半。
这验证收益不仅是把擦除从速率统计中移走。整次更新仍有正常模式切换和启动开销。

仅替换设备固件、尚未修复主机退避时，DATA 已达 256.8 KiB/s（app-update）和
258.1 KiB/s（system-update），但全程分别为 17.810、10.227 秒。故最终验收
同时使用新固件和修复后的 Gateway；连续两次 app-update 复用同一 Gateway。
这些是单板、小样本实测，不代表所有固件大小和主机环境的稳定上限。

## 操作与健康证据

| 操作 | Gateway operation ID |
| --- | --- |
| 基线 system-update | `4f45b831-03e3-45b8-9405-432f36ced9ab` |
| 基线 app-update | `555843c7-d464-4c99-8723-eb62fc6e68a5` |
| Vibe Mode 自更新 | `a9c8186e-1c9a-4203-9287-8dbe6dfba8bc` |
| 最终 app-update 1 | `fdaf5eb5-3bf2-412a-b658-01e69e03f1ab` |
| 最终 app-update 2 | `31616002-0d89-4bc9-a33e-b9088364205c` |
| 最终 system-update | `b3220aa4-ae52-4083-9def-7b973461a8a8` |

所有更新返回 succeeded/healthy，通过原始镜像及启动后 ELF SHA-256 验收。
最终 app-update 第二轮 Boot ID 为 normal `18311654928069339571` →
Vibe Mode `16838444647060755778` → normal `16552332497843676374`。
最终 system-update 后 normal Boot ID 为 `2013456810880186480`；
2 秒服务 RPC 完成，随后状态 RPC 返回计数 `(1, 0, 1)`，实时内存查询成功。
CLI 与 Web 数据接口的 Device ID、Boot ID、固件哈希和操作记录一致。
收尾回到已就绪 Vibe Mode，Boot ID `16424758435461225350`，实时内存查询
成功，设备身份和优化固件 ELF 哈希再次匹配。

应用二进制 SHA-256：
`bbc335748217b667b26b1acc2abf28a32f96cd9855ec7fba4fec60da99620eac`；
应用 ELF SHA-256：
`455873128f421a401cee522c71bfddd27229b8a6aeeb038178f75a9283a8fae4`。

Vibe Mode 二进制 SHA-256：
`ce66912c5b161c81f9e147dbfc1788e05ae2335195e778a2950dd4b7391aa545`；
Vibe Mode ELF SHA-256：
`4b9c3296d2e218977d0a88d205f69f328ebb0a5867cd3fbde0e15a5fc6afc55b`。

自更新前后 bootloader inventory 哈希保持
`925d2541ed213aa29f552f15ee6881b899dd72c4dbc81343ae2b4c88caa72cc4`，
分区表 inventory 哈希保持
`6f0d33a90336cf8a5361aadf787d819ab53e741d8f3c6614f0b8d40d28000fff`。
这些是设备保护范围的哈希，与未填充的包文件哈希口径不同。

## 构建、回归和包验收

- Recovery 与验收应用均由 low-noise runner 使用固定 IDF 构建成功。
  Recovery 1,817,040 字节，槽位余 17,968 字节；唯一警告为应用分区接近满载。
- 104 项 Iris 测试通过：宿主 C 运行时各配置、session、system-update、link。
  新增覆盖擦除失败/取消后的资源清理与重试、擦除时协议响应、最大块/越界块/
  尾块以及执行器持有数据副本；未在实机上注入断电或 Flash 擦除失败。
- 34 项 Gateway/USB 重连测试通过；新增回归验证连续失败退避上限和成功握手
  后重置，避免多次 OTA 重启累积等待。
- 31 项 Recovery 测试通过：审核包完整性、保留分区/应用/产品契约、恢复韧性、
  内部内存预算与吞吐显示。Ruff 对改动 Python 文件无新增诊断，既有诊断未改写。
- 用 `prepare_recovery.py` 重新生成完整审核包，保留原审核包 bootloader、
  基础分区表及初始 OTA 数据，纳入实机已验证的 `factory.bin` 和匹配 manifest。
  保留版本 `0.1.4`，布局/ABI 不变；manifest 记录源码基线及 dirty 状态。
  本次走受保护的自更新，没有重新执行 ROM 整包写入。

在消费工作区通过以下命令复现（先遵守设备所有权和状态检查）：

```sh
python3 mosaico.py iris system-update --project submodule/esp-mosaico-utils/esp-mosaico-vibe-mode/tests/firmware/iris_acceptance --skip-build --json
python3 mosaico.py iris app-update --project submodule/esp-mosaico-utils/esp-mosaico-vibe-mode/tests/firmware/iris_acceptance --skip-build --json
python3 mosaico.py iris rpc 0x6a02 1 --project submodule/esp-mosaico-utils/esp-mosaico-vibe-mode/tests/firmware/iris_acceptance --json
python3 mosaico.py iris rpc 0x6a02 3 --project submodule/esp-mosaico-utils/esp-mosaico-vibe-mode/tests/firmware/iris_acceptance --json
```

原始操作 JSON、CLI 日志索引、Web 事件和 `performance-summary.json` 保存在
消费工作区 `.agents/analysis/ESP-174/`；构建完整日志位于各工程
`.codex-runs/idf-low-noise-build/`。工作区为 `esp-mosaico-vibe-esp174`，
分支 `feat/ESP-174`。
