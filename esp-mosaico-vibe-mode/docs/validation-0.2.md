# 0.2 实现与验证记录（2026-10-09）

ESP-Iris、mosaico-tools 和 Vibe Mode 使用 0.2 契约。评审基础包已整体更新为
0.2.0；设备、主机与更新包不兼容 0.1。以下结果限定于本次配置和设备，未测
项目单独列出，不将历史版本结果记作本版验收。

## 环境与发布制品

- Linux、Python 3.12；ESP-IDF 精确提交
  `7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe`。未修改 SDK 来实现本次修复；
  SDK 中既有的其他改动保留，因此镜像 IDF 描述含 `dirty`。
- utils 基准提交 `fc63d43f3ac72aa54e701b07b07fbed07a16b3ca`，本轮实现尚未提交；
  manifest 如实记录 `source.dirty=true`。
- S31 产品运行于 QIO/80 MHz；保留 SDK 为 ROM 加载阶段生成的 DIO 镜像参数。
- Recovery ABI 2、布局版本 5、`mosaico-retained-test-2m-v2`，基础包 schema 3。
- `vibe_mode` 为 test 分区，位于 `0x20000`，大小 `0x1c0000`；
  `main_app` 为 ota_0，从 `0x210000` 开始。原生应用清单不写 Vibe Mode 镜像。

| 最终制品 | 字节数 | SHA-256 |
| --- | ---: | --- |
| Vibe BIN | 1782864 | `1f1573f4f928283d4fc7a9a038f133a00b28b0db4b10cc055c8caf3a549d2d8d` |
| Vibe ELF | 16095920 | `c3e53fc7fdd421cfaca06dda37b50f815bd7d2e01d647c2e729dc9dec3bd7ef3` |
| bootloader BIN | 21984 | `b70aee04433bd1afa648816de9795169029b276448b18facedd64fcc94f40b0c` |
| 分区表 BIN | 3072 | `ff7ac35a4f2e51424150a6ba56172174299baa737230a09792dd46e8f5d0bb99` |

基础包还包含 8192 字节安装 otadata：偏移 32 处为 16 字节
`MOSAICO-BOOT-02!`，其余为 `0xff`，不附加 NUL。普通原生应用使用空白 otadata。
完整清单以 [manifest](../firmware/vibe-mode/prebuilt/recovery/manifest.json) 为准。

## 主机、构建与设备结果

Iris Python/C host 全套 568 项通过（含自动设备选择、重启生命周期及内存布局回归）；最终 USB shutdown 生命周期回归再次通过。
Workbench 单元 7 项、E2E 8 项及生产构建通过。Mosaico 主机套件覆盖 CLI、契约、
bootloader NVS 读取、Bridge/NAND 模拟后端及原生 GSP UI，发布后完整重跑
482 项通过（补测重跑 181.41 秒）。固件构建通过，bootloader 与 Vibe 均符合固定分区大小。

| 设备与链路 | 已验证内容 |
| --- | --- |
| S31 `30:ed:a0:f4:51:8e`，High-Speed USB | 双 CDC、设备/启动/会话绑定、RPC、截图、媒体、文件、OTA、断线与生命周期；原版 monitor 日志、status/help |
| 同一 S31，TCP | 独立双端口、认证、自动绑定、媒体/文件、令牌轮换、OTA 及重启 |
| H2 `60:55:f9:f7:2e:60`，USB Serial/JTAG | RPC、控制截图、日志、硬件复位和早期启动采集；原生 flash monitor；第三方 IDF Hello World 原源码/原分区表烧录 |
| C2 `e8:6b:ea:4c:69:24`，UART 74880 baud | RPC、控制截图、日志、硬件复位和早期启动采集；原生 flash monitor |

S31 产品流程通过：ROM 实时身份校验、基础安装、GPIO7 物理入口、System Update、
App Update、Vibe 自更新、同设备新 Boot ID/目标 ELF/健康校验，以及 Wi-Fi 配置后
跨重启自动连接。评审包更新后，省略候选参数的默认 App Update 再次成功，
Operation 为 `711540da-b67b-41ee-b80c-fb296118bb61`。

软件重启修复后连续 20 次「正常应用 → Vibe ready → App Update → 健康正常应用」
通过，每次均为新 Boot ID。该压力构建与最终构建目录不同，Vibe ELF 为
`62ad9109…`，不能当作最终 ELF 的 20 次实测。最终规范构建另行完成自更新、
bootloader System Update 及 5 项崩溃矩阵：计划重启、断言、非法访问、任务
看门狗、启动崩溃循环。四类故障均保存 Core Dump、匹配完整 ELF 并解码到源位置；
最后恢复健康正常应用。默认 App Update 后正常应用 ELF 为 `caccd730…`，
Boot ID 为 `1052455619224500027`，随后 stock monitor 再次确认实时身份和日志。

## 本轮修复

- S31 CPU 重启未清理 USB DMA，旧应用 SETUP 缓冲区与新 Vibe 的 IRAM 代码重叠。
  Iris shutdown 停止控制器；bootloader 在加载应用 RAM 前清理继承的 DMA，
  同时覆盖 panic/watchdog 路径。原故障 PC 与精确正常应用 ELF 的 DMA 缓冲区
  地址重合；原诊断 Vibe 缺少匹配 ELF，不宣称其完整源级解码已完成。
- 固定 SDK 的 bootloader 公共 NVS 读取器拒绝设备上运行时可加载的多 ACTIVE 页。
  产品只读解析器按运行时顺序解析页、命名空间和键，异常状态选择 Vibe；不擦除
  sysmeta，不修写 NVS。重复页产生原因未证实。
- 产品有效 QIO 配置检查覆盖既有 sdkconfig 和复用制品；修复原 DIO bootloader
  启动 QIO Vibe 后的 Flash 读取异常。ROM 分区表回读证明原表内容完整。
- Vibe 关闭 Wi-Fi modem sleep，处理精确 Core Dump 确认的 `pm_process_tim`
  看门狗问题；普通应用仍自行选择省电策略。
- Gateway 对握手前日志保留原事件 ID，设备关联后向 Follow 重放；断线恢复测试
  覆盖错过绑定事件的游标，未知 Boot ID 不伪造为后续启动。

## 补充覆盖与资源缺口（2026-10-09）

补测新增可复用的 [三设备验收脚本](../tests/iris_02_acceptance/README.md)，
7 项实机用例全部通过：慢 RPC 期间控制响应、超长响应拒绝、并发操作去重与
参数冲突、超时后的副作用不重复执行、三设备同时收发，以及 H2/C2 各自硬件
复位时的按设备过滤 Follow。慢 RPC 运行期间 30 次状态查询最大 22.855 ms；
H2/C2 各 40 次 256 字节回显最大分别为 76.065/153.588 ms。这是本次负载的
观测值，不是所有环境的响应上限。两个控制设备均收到 ROM 与二级 bootloader
日志，另外两台设备的 Boot ID 保持不变。

补测修复 Gateway 自动选择把同一 USB 设备的 control/data CDC 当成两台设备
而拒绝连接的问题。仅用 USB 物理位置合并选择候选，优先 control；任一接口
被其他会话占用时排除整组。串号相同但物理位置不同仍保持歧义，链路最终
归属仍须经过实时协议绑定。6 项单元回归和实机自动连接通过。

最终 Vibe ELF `c3e53fc7…` 完成 100 次 RPC、5 次完整截图、30 秒镜像负载，
收到 80 个完整 480×480 RGB565 帧。独立截图强制走控制链路；镜像占用期间，
显式控制截图返回忙错误，数据截图复用镜像帧；停流后控制截图再次成功。
整机内部 RAM/PSRAM 可用量在负载前后分别为 79,111/13,787,348 字节，恢复到
原值；历史 invalid_frames 为 2→2，crash_count 为 0→0，Boot ID 未变。
这项整机观察不等同于 Iris 自身内存预算验收。

Workbench 新增真实设备测试：只读 RPC、浏览器实际解码 PNG、镜像期间截图
复用、停止镜像和操作记录均通过；页面模拟 E2E 仍为 8 项通过，另 2 项硬件
测试默认显式跳过。前端单元 7 项和生产构建通过。NAND 真机仅完成页面进入、
挂载扫描、空结果显示与 Rescan；盘上无测试更新包，未宣称安装路径已通过。

[内存夹具](../tests/firmware/iris_internal_budget/README.md) 已迁移到 API v2
及新的 Vibe boot-intent 入口，使用公开 Hello World 模板生成测试应用。
仪表化 ELF `0d93c49a…` 的 100 次 1024 字节回显、截图和 70 个完整镜像帧
优化前功能检查通过，但 **25,000 字节内部内存门槛未通过**（历史基线保留如下）：

| 测量 | 字节数 |
| --- | ---: |
| 常驻归属堆，负载前/后 | 15,212 / 15,212 |
| 负载期间归属堆峰值 | 16,528 |
| 计入静态 DRAM（含对齐） | 12,520 |
| 负载总峰值 | 29,048 |
| 切入 Vibe 前归属堆峰值 | 17,400 |
| 含退出阶段的总峰值 | 29,920 |
| TinyUSB 最小剩余栈 | 1,368 |

分配追踪错误为 0；该次有限负载未显示持续泄漏；保留其预算失败记录，未提高
门槛或改写原始失败数据。静态开销中完整 TinyUSB 为 7,816 字节、Iris 为 3,824
字节；这些是归属分项，不足以单独证明新增开销的全部来源。夹具安装与恢复
均经过 System Update；恢复了原 `iris_acceptance` ELF `caccd730…` 及原分区表。

## 内存优化复测（2026-10-09）

普通应用 PSRAM 配置下，双 CDC 的 4,096 字节软件 FIFO 与 560 字节文件卷注册表
移入外部 BSS；DMA、锁、任务栈仍在内部 RAM。删除无用的传输序列号字段节省
288 字节，新重启状态占 4 字节。Mosaico 切入 Vibe 改用 Iris 通用延迟重启 API，
复用协议任务，取消原临时 2 KiB 任务；不降低缓冲容量、RPC 上限或任务栈配置。
TinyUSB 使用源哈希检查的构建副本，不改 managed 源码；未知版本明确拒绝适配。

最终仪表化 ELF 为 `b5c02b0cd872ff9f28517663d51886a1538542a1d45a8a7cb506f0a172ce7775`。
按原有实际地址、分配器开销、静态对齐口径重新测量，**含切入 Vibe 前退出阶段
的峰值为 24,108 字节，严格低于 25,000 字节**，余量 892 字节。

| 项目 | 优化前 | 优化后（字节） |
| --- | ---: | ---: |
| 静态内部 RAM，含对齐 | 12,520 | 7,580 |
| 负载前/后归属堆 | 15,212 / 15,212 | 15,212 / 15,212 |
| 负载归属堆峰值 | 16,528 | 16,528 |
| 退出采样的当前归属堆 | 17,400 | 15,212 |
| 含退出阶段归属堆峰值 | 17,400 | 16,528 |
| 总峰值 | 29,920 | 24,108 |
| TinyUSB 最小剩余栈 | 1,368 | 1,368 |

总峰值下降 5,812 字节（19.4%）；服务任务最小剩余栈 2,692 字节，追踪错误 0。
删除退出任务本身减少 2,188 字节瞬时分配，但总峰值收益为 872 字节，因为原有
16,528 字节的早期堆峰值仍然存在。该早期峰值的精确调用来源尚未归因。

最终镜像通过两轮连续测试，Boot ID 相同、无新增崩溃/无效帧、负载后堆恢复原值：

- 单设备：100 次 1,024 字节 RPC、5 次截图、30 秒镜像，70 个完整 480×480
  RGB565 帧，与原基线 30 秒的完整帧数量一致。
- 同一 Gateway 三设备：S31 同样的 RPC/截图及 30 秒镜像，57 个完整帧；H2
  USB Serial/JTAG 与 C2 UART 分别连续完成 335/178 次带设备标记的 256 字节
  RPC、控制截图和按设备过滤的 Follow。RPC 最大耗时约 216/290 ms。
- 连续画面仍在传输时，经 `mosaico.py iris test enter-recovery` 成功切入同一
  设备的 Vibe Mode；退出日志仍为 `current=15212 peak=16528 errors=0`。
  Vibe 的 RPC、控制截图、镜像复测通过，此项只作行为验证，不作 Iris 堆归属测量。

Iris 全套 568 项与 Mosaico 全套 482 项通过。增加了重启延迟边界、重复请求不
延期、双链路 TX 等待/有限宽限、断线保留与停服取消测试，以及外部卷表分支和
未知 TinyUSB 源码拒绝测试。一次从工作区根目录启动的全套测试因子进程无法导入
`iris_gateway` 失败 2 项；从组件文档规定的 tools 目录重跑全套通过，原失败日志保留。

S31 仪表化/普通应用、H2、C2 源码交叉编译均零警告。Vibe 源码构建通过，固定槽
剩余 52,176 字节，保留其容量接近上限提示。实机优化对象是 S31 普通应用；H2/C2
作为混合链路对端沿用已有 0.2 镜像。本轮未重新发布/写入 Vibe 基础包，实机保留
已验收的 `c3e53fc7…`。外部存储配置不适用于该写入固件的内部内存预算，也不把
本次普通应用有限负载结果推广为任意 RPC/文件/联网业务的全局上界。

优化测试结束后，通过 System Update 恢复原 `iris_acceptance` 应用/分区布局，
安装的是本轮新源码 ELF `ae5e71017b23aaf2044d2224f098a828439babd7487d847df8b04196e196c0ee`。
随后默认 App Update 再次完成正常应用 → Vibe → 正常应用，最终 Boot ID 为
`13080410305375929428`。恢复后 7 项三设备验收全通过，包含 H2/C2 硬件复位、
ROM/二级 bootloader 日志和其他设备 Boot ID 不变。三台最终 crash_count 与
invalid_frames 均为 0；本次设备占用已释放，Gateway 状态为空。

## PR 质量检查修复（2026-10-09）

修复首次 PR 检查发现的 16 处 mypy 错误：明确串口 DTR/RTS 接口和候选集合类型，
检查可空设备身份，在 TCP 自动数据连接入口使用既有十六进制令牌接口，并区分
截图几何信息与带传输路径的响应元数据。Gateway 复位路由通过适配器公开接口
查询当前设备身份，不再读取 Hub 的私有会话表；Demo 适配器明确拒绝硬件复位。
未增加类型忽略或关闭检查。

按职责提取控制帧编码/事件、按需截图控制、主机 OTA 事务；原有消息处理、
请求锁、超时恢复和取消清理逻辑保留。抽出的 C 函数体与原文一致，Python OTA
函数体经参数名称归一化后的 AST 比较一致。

| 原超限文件 | 修复前 | 修复后 | 原门槛 |
| --- | ---: | ---: | ---: |
| `src/esp_iris.c` | 1438 | 1008 | 1300 |
| `tools/iris_gateway/session.py` | 1430 | 1230 | 1300 |
| `src/esp_iris_media_control.inc` | 267 | 148 | 260 |

新增私有模块 `esp_iris_control_frames.inc`、`esp_iris_snapshot_control.inc`、
`ota_transport.py` 分别为 434、132、237 行，并新增 500、200、300 行预算。
原文件门槛及 25,000 字节内部内存门槛保持不变；22 项源文件行数预算全部通过。

本轮验证：mypy 检查 56 个源文件无错误，Ruff、仓库布局、Python 字节码编译及
差异空白检查通过。Iris 全套 578 项通过，包含新增的复位身份归属、HTTP 请求
去重及 TCP 配对令牌传递回归；C runtime/file lifecycle 的 ASan/UBSan 51 项通过。
Mosaico/Recovery 主机全套 483 项复测通过。
使用上述精确 ESP-IDF 提交构建 S31 services 夹具成功，零警告，BIN 为 494,672 字节。
本轮未重新烧录设备或替换已评审的 Vibe 基础包，前述内存/实机结果仍以原测量
制品为准，不把源码拆分后的固件构建视为重新完成实机验收。

## 限制与未验证项

- JTAG/OpenOCD 按用户要求跳过，本轮不作通过结论。
- 锁定 monitor 1.9.0 默认首次打开仅尝试一次，S31 HS 原生 `flash monitor`
  烧录成功后可能赶不上 USB 重枚举而退出。等端口出现后单独运行 `idf.py monitor`
  已通过，无 Iris 扩展或 Gateway。其直接 `idf_monitor.py --open-port-attempts`
  参数未由该版 `idf.py monitor` 转发，不把它描述为组合命令已修复。
- HS 应用 CDC 无法回收此前由 UART 输出的 bootloader 日志；UART/Serial-JTAG
  启动采集已实测，USB 消失期间的物理缺口仍需如实记录。
- 外部 Bridge 0.2 服务尚未联调；Bridge/NAND 的主机模拟测试不替代远程服务与
  全部 NAND 真机路径。完整断电矩阵、跨平台和全部性能预算未验收。
- 普通应用的当前仪表化负载预算已通过；余量 892 字节。新增任务、回调、实际
  TCP 联网或文件工作负载仍需按其配置重新验收，不能沿用本次数字。
- 仅有单应用槽与单维护镜像，不提供 A/B 无损回滚保证。

## 本地原始证据

证据保存在消费工作区的忽略目录，不进入发行包。路径相对于消费工作区：

- `.codex-runs/iris-02/hardware/acceptance-status.json`：本轮汇总和设备状态。
- `.codex-runs/iris-02/memory-optimization/`：内存优化的匹配制品、静态布局、
  主机测试、混合链路、退出阶段和默认更新记录。
- `.codex-runs/iris-02/supplemental/`：新增 7 项多设备验收、浏览器/NAND、
  测试与恢复操作、内存失败汇总和原始日志。
- `.codex-runs/mosaico/0.2/20261009T043453Z-iris-budget-test/`：Vibe 整机负载。
- `.codex-runs/mosaico/0.2/20261009T044109Z-iris-budget-test/`：仪表化内存失败原始证据。
- `.codex-runs/iris-02/hardware/usb-dma-release/`：最终 BIN/ELF/map、操作记录、
  截图、Wi-Fi 重启结果、默认更新和 stock monitor 原始日志。
- `.codex-runs/iris-02/hardware/usb-dma-fix/cycles/result.json`：20 次压力循环。
- `.codex-runs/iris-crash/20261009-114722/`：最终 5 项崩溃矩阵及恢复结果。
- `.codex-runs/iris-02/hardware/`：H2/C2/S31 各传输测试、原生烧录及原始 Flash 备份。

三个设备的原始 Flash 和旧 0.1.4 基础包均已备份，未全片擦除。Wi-Fi 密码未写入
跟踪文件。测试结束释放设备并关闭本任务 Gateway，普通应用继续运行。

## Workbench 连接与状态修复（2026-10-09）

实现提交 `6382d6a`、`23cc2f7`、`4c1ede5`；只修改主机与网页，未重刷设备。
设备页新增连接选择器、断开当前设备、UART 波特率和手动串口/TCP 地址、配对令牌。
被动枚举包含 USB Serial/JTAG、UART；默认自动选择仍仅限 Iris 设备。
`discovered` 明确表示「端口存在但未连接」；断开清除缓存中的控制/数据可用标志。
修复切换设备的过期状态响应、画面/弹窗串入、镜像与录音清理、离线截图/输入按钮，
以及模式切换失败后按钮卡住。USB 控制与数据接口明确标注，断开按 Device ID 释放全部接口。

- Iris Python/C host：**584 passed**；最后的拔出缓存状态修正另跑相关 **6 passed**。
- Workbench：**7** 个单元测试、生产构建通过；Playwright **13 passed**，
  默认跳过 **3** 个显式硬件用例，随后独立执行下述实机用例。
- mypy **56** 个文件、Python 3.8 目标 Ruff、差异检查通过；
  **22** 个源文件预算和 Workbench 目录体积预算通过，未放宽限制。
- 同一 Gateway 内，S31 高速 USB、H2 USB Serial/JTAG、C2 UART **74880 baud**
  全部通过网页「连接 → 断开 → 再连接」。断开后等待至少一个列表刷新周期，
  确认不自动抢回端口、无残余设备归属、缓存链路为空；S31 数据 CDC 自动重新绑定。
  重连前后 Boot ID 均相同，未触发硬件复位：
  S31 `12057626572984767234`、H2 `10763306005646118801`、C2 `11035779942514683312`。
- H2、C2 分别通过网页实时身份、只读 RPC、PNG 控制链路截图及操作记录测试，
  没有数据链路时镜像按钮禁用；浏览器未出现未捕获异常。CLI 另行核对三台实时身份。
- S31 当前 `iris_acceptance` 固件没有注册截图/镜像 provider；本轮对它验证连接与 RPC，
  不将预期的 provider 缺失算作截图成功，也未为此替换固件。
  连续镜像和录音的设备切换清理由浏览器模拟后端回归覆盖；本轮不新增实机连续媒体结论。

混合连接测试可通过 `ESP_IRIS_TEST_URL` 和 `ESP_IRIS_CONNECTION_TARGETS`（明确
`endpoint`、`deviceId`，UART 加 `baudrate`，双 CDC 加 `data: true`）运行
`frontend/tests/hardware-connections.spec.ts`。测试保留 `connection-evidence.json`
和截图；本地完整证据位于消费工作区 `.codex-runs/iris-02/web-connections/`。
运行中的 Gateway 后端为 `6382d6a`，静态页面更新到 `4c1ede5`；后两次提交未修改后端。

[设备页截图](../../ESP-Iris/docs/images/workbench-devices-0.2.png) ·
[连接选择器截图](../../ESP-Iris/docs/images/workbench-connections-0.2.png)

## Workbench 原生命令输入（2026-10-09）

实现提交 `7a41046`、`5e1b884`、`cba95a3`。日志面板新增单行命令输入，
Enter 发送、上下键访问最近 32 条历史；离线、Observe 和只有数据链路时禁用。
后端直接向控制 Console 写入文本，与协议记录共用写锁，删除未注册的
`console.execute` RPC 路径。发送完成不代表固件执行成功，结果从设备日志读取；
失败保留输入，不自动重发，不返回虚构的 Job ID。保留协议命令的带引号写法也会被拒绝。
同时修复 Ready 后仍每两秒重复发送发现命令的问题和输入行按钮换行问题。

- Iris Python/C host **603 passed**，mypy **57** 个文件、Ruff、生产构建通过。
  Workbench 单元测试 **7 passed**；Playwright **18 passed / 4 hardware opt-in skipped**。
  源文件预算 **22** 项和前端目录预算通过，未放宽限制。
- 同一 Gateway 的 S31 高速 USB、H2 USB Serial/JTAG、C2 UART **74880 baud**
  都通过网页输入 `iris help`、`iris status` 并收到本次发送之后的新日志；每条响应验证
  控制端点、操作记录及 `completion=unconfirmed`。Boot ID 与上一节相同，无重启或刷写。
- H2 旧固件在首次连续命令测试中出现一次 `ESP_ERR_TIMEOUT`：外部 REPL 单行队列
  与网页状态轮询竞争。后续三设备测试通过，**不代表旧固件的竞态已经消失**。
  固件源码改为注册的 REPL 回调最多等待一秒入队，不增设记录缓冲，也不重执行命令。
  自定义 `esp_iris_console_submit()` 仍非阻塞。C host 覆盖队列耗尽、超时、tick 回绕、
  停止/重启期间取消等待；H2 外部 REPL 固件在固定 ESP-IDF revision 下构建通过，零警告。
  **该固件修复尚未刷入 H2；本轮不宣称修复后的硬件并发验收已完成。**

运行中的主机代码更新到 `cba95a3`；完整原始证据保存在消费工作区
`.codex-runs/iris-02/web-console/`，包含首次失败、三设备成功复测、最终页面和 H2 构建日志。
实机用例是 `frontend/tests/hardware-console-input.spec.ts`，设备选择沿用
`ESP_IRIS_TEST_URL` / `ESP_IRIS_CONNECTION_TARGETS`，不会自动重刷设备。

[日志命令输入截图](../../ESP-Iris/docs/images/workbench-console-0.2.png)

### 空行显示跟进

`fde04d1` 默认隐藏日志视图中的纯空白记录，提供「显示空行」恢复显示；原始采集和
历史不删改，非空消息内部换行不变。原因是每条控制响应之前的 CR/LF 分隔符被展示为
独立 `I raw` 行；`ACCEPTANCE_ALIVE` 则是验收固件每 5 秒的正常日志。
相关 Console/日志浏览器回归 **6 passed**，生产构建和前端体积预算通过。
实时网页验证保留的 **872** 条空白记录默认不可见、开启显示后恢复，**276** 条非空
记录继续显示；同时核对发送按钮与输入框同行。未重启 Gateway 或设备、未更新固件。

[空行显示修复截图](../../ESP-Iris/docs/images/workbench-log-filter-0.2.png)
