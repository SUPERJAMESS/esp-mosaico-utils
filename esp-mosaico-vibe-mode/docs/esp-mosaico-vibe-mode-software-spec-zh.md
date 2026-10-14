# esp-mosaico-vibe-mode 软件规格书

| 项目 | 内容 |
| --- | --- |
| 文档修订 | 第 2 版：0.2 启动契约定版 |
| 修订日期 | 2026-10-09 |
| 文档状态 | 0.2 契约已实现；评审包 0.2.0，验证边界见第 10 节 |
| 适用范围 | Vibe Mode 固件、产品启动与分区契约、应用 Recovery 适配、更新后端及评审基础包 |
| 协议代际 | 配套 ESP-Iris 0.2，与 0.1 不互通 |
| 版本边界 | Vibe Mode 固件版本、Recovery ABI、布局 ID、元数据格式与 Iris 协议分别管理 |
| 参考验证环境 | ESP32-S31；ESP-IDF `7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe` |

## 1. 目的、范围与模块边界

本规格承接原 ESP-Iris 规格中的 ESP-Mosaico 产品要求。通用控制、数据自动发现与绑定、控制链路截图、启动日志采集及硬件复位由 [ESP-Iris 规格](../../ESP-Iris/docs/esp-iris-software-spec-zh.md)定义；CLI、工程生成、主机编排和用户迁移流程由 [mosaico-tools 规格](../../mosaico-tools/docs/mosaico-tools-software-spec-zh.md)定义。

本模块拥有保留固件、产品启动选择、分区布局及权限、软件恢复入口、崩溃恢复、应用适配 ABI、设备更新后端和基础固件包。不得将这些产品策略下沉为通用 Iris 的固定要求。

“必须／不得”为验收要求，不表示所有硬件组合均已实测。0.2 已采用 test 维护分区、Recovery ABI 2 和布局版本 5；源码、集成指南与评审包使用相同契约。

## 2. 产品模式与核心要求

| 概念 | 定义 |
| --- | --- |
| 正常应用 | 位于产品应用区域的用户应用，使用公共 Recovery 适配 |
| Vibe Mode | 保留的应用安装与设备维护固件，运行 Iris |
| ROM Download Mode | 芯片内置下载程序，不运行 Iris 或 Vibe Mode |
| Recovery | 路径、组件、角色字段及 ABI 的技术名称；用户可见名称为 Vibe Mode |
| 原生 IDF 路径 | 通过标准烧录、monitor 和 OpenOCD/JTAG 使用工程 |
| 产品更新路径 | 经 mosaico-tools、Iris 与 Vibe Mode 后端编排的更新 |

| 编号 | 要求 |
| --- | --- |
| REC-BR-01 | Mosaico 工程默认使用产品更新流程，同时支持原版 `idf.py flash monitor` 和 OpenOCD/JTAG。 |
| REC-BR-02 | 正常应用与 Vibe Mode 均保留标准 console，不依赖 Iris 主机才能读日志和使用设备命令。 |
| REC-BR-03 | 普通应用更新的 Flash Writer 保留在 Vibe Mode，应用只负责进入维护模式与报告状态。 |
| REC-BR-04 | 保留 GPIO7、软件请求、崩溃循环及无有效应用时进入 Vibe Mode 的产品能力；GPIO61 是 ROM 下载入口。 |
| REC-BR-05 | Mosaico 原生烧录使用匹配的启动契约并保留 Vibe Mode；第三方自己的完整烧录不承担此保留承诺。 |
| REC-BR-06 | 保留现有 Wi-Fi、Download Ideas／HTTPS Bridge、NAND 安装、文件、截图、输入、诊断及专用自更新能力。 |
| REC-BR-07 | 产品固件、主机及更新制品使用配套 0.2 契约；不保留 0.1 协议、旧布局转换或兼容分支。 |

AI 键 GPIO7 选择 Vibe Mode 与 Boot 键 GPIO61 选择 ROM Download Mode 必须在文档、界面和测试中区分。自动进入 ROM 的可用性取决于板级 UART DTR/RTS 或 USB Serial/JTAG；CTS 不是通用复位输出。

## 3. 与 ESP-Iris 0.2 的集成

| 编号 | 规格 |
| --- | --- |
| REC-IRIS-01 | 正常应用和 Vibe Mode 使用同一硬件派生 `device_id`，每次启动产生新 `boot_id`；角色、固件及产品契约通过实时查询取得。 |
| REC-IRIS-02 | 不占用 JTAG，不屏蔽标准 Serial/JTAG 复位，不把 console 改成 Iris 私有二进制端点。 |
| REC-IRIS-03 | High-Speed USB 保留 console 与独立数据接口；TCP 使用独立端口。数据端点独立发现、握手和授权，不以控制请求为前置条件。 |
| REC-IRIS-04 | 数据连接可先建立，更新或媒体任务仍须通过产品授权并关联同一设备、启动、数据会话和任务，不能因自动绑定直接取得写权限。 |
| REC-IRIS-05 | 仅控制链路时支持已注册的按需截图和诊断；连续媒体、文件与固件的 Iris 传输仍需要数据链路。原生 ROM 烧录不受这一限制。 |
| REC-IRIS-06 | 保留启动与异常原生日志；应用 Iris 尚未运行时不影响 UART／USB Serial-JTAG 的 bootloader 日志采集。复位重枚举形成的缺口按 Iris 规格报告。 |
| REC-IRIS-07 | 数据或控制短暂掉线不自动重启设备；按更新阶段记录继续、暂停或失败，不重复提交，不将调试暂停当作崩溃。 |

Vibe Mode 默认使用 High-Speed USB 提供 Iris 服务。正常应用默认同样提供；产品因其他 USB 功能调整时，必须声明 console、Iris 维护和 ROM 恢复的可用接口。应用 USB CDC 不自动成为 ROM 下载端口。

## 4. 分区与启动契约

### 4.1 契约所有权

`product_contract.json` 是机器可读产品、板型、布局及最低固件要求的来源；公共 ABI 由 `include/mosaico_recovery_contract.h` 定义。模板、bootloader、正常应用、Vibe Mode、制包器和主机校验必须使用同一发布契约。

普通更新不能修改该契约的保留布局。匹配的原生 Mosaico 烧录可写入 bootloader、分区表及初始化 otadata，但必须保留有效维护入口、Vibe Mode 镜像以及未在清单中声明写入的产品数据。布局不变不等于 Flash 前缀的每个字节都禁止写入。

### 4.2 test 分区与参考布局

以下为 16 MiB Flash 带资源区的参考布局。已采用 test 维护分区；应用和资源的具体大小以完整清单为准，基础包的 main_app 使用剩余空间：

```csv
# Name,    Type, SubType, Offset,   Size,     Flags
otadata,   data, ota,     0x9000,   0x2000,
phy_init,  data, phy,     0xb000,   0x1000,
sysmeta,   data, nvs,     0xc000,   0x14000,
vibe_mode, app,  test,    0x20000,  0x1c0000,
coredump,  data, coredump,0x1e0000, 0x20000,
nvs,       data, nvs,     0x200000, 0x10000,
main_app,  app,  ota_0,   0x210000, 0xcf0000,
ui_apps,   data, 0x40,    0xf00000, 0x100000,
```

Vibe Mode 结束于 `0x1e0000`，固定系统区域结束于 `0x200000`，参考应用结束于 `0xf00000`；终点均不包含在区间中。应用与资源区域可按已发布的产品规则划分，`ui_apps` 不是所有应用的强制资源分区。

固定 IDF 的默认应用烧录选择查找 factory、ota_0 等 subtype，跳过 test。因此没有 factory 时，正常应用默认写入 main_app。决定选择的是 subtype 与构建产物，单独改标签没有此效果。

**仅把 factory 改为 test、ota_0 改名 main_app 不足以恢复整个开发流程。** 实现必须满足：

| 编号 | 规格 |
| --- | --- |
| REC-LAYOUT-01 | 发布新的布局契约标识和匹配基础包；不得沿用旧 factory 布局 ID，或让旧工具按旧含义继续操作。 |
| REC-LAYOUT-02 | 修改 bootloader 的 GPIO7 选择、无有效应用路径和维护固件验证；避免继续搜索 factory 或把普通应用当成 Vibe Mode。 |
| REC-LAYOUT-03 | 定义软件启动意图的保存、读取、消费及清除规则；不能简单调用 `esp_ota_set_boot_partition(test)` 替代旧 factory 入口。 |
| REC-LAYOUT-04 | 迁移崩溃计数、计划重启、健康确认和返回正常应用的启动选择；运行后连续崩溃与镜像校验失败分别处理。 |
| REC-LAYOUT-05 | 更新固件构建、标签引用、镜像角色、主机验证、资源清单、恢复基础包和原生 flash/app-flash 目标。 |
| REC-LAYOUT-06 | 检查完整 native flash 清单，包含 bootloader、分区表、otadata、应用及需要的资源；不能仅验证应用偏移。 |
| REC-LAYOUT-07 | 单个 ota_0 与 test 维护分区不等于 A/B 双应用回滚；故障恢复依靠有效的 Vibe Mode，保留 ROM 兜底。 |
| REC-LAYOUT-08 | 验证擦写范围、断电／重启、无效镜像及第三方恢复场景后才替换评审基础包。 |

### 4.3 启动与 Recovery ABI

| 编号 | 规格 |
| --- | --- |
| REC-BOOT-01 | 正常启动选择有效用户应用；明确的维护请求、GPIO7 或产品定义的故障条件进入有效 Vibe Mode。 |
| REC-BOOT-02 | 软件进入维护模式只改变启动意图并重启，不重写维护固件；重复请求、掉电和已消费意图不能造成永久启动循环。 |
| REC-BOOT-03 | 计划重启、调试停机、软件异常与连续崩溃分别处理；健康确认有明确条件，不能仅凭端口出现清零失败历史。 |
| REC-BOOT-04 | 两类固件共享硬件身份、启动意图、健康与更新结果的公开 ABI；可变状态的所有者和写入顺序必须明确。 |
| REC-BOOT-05 | ABI、元数据格式或布局语义发生不兼容变化时独立升版；0.2 代码只实现新的契约，不携带旧格式解析或自动迁移分支。 |
| REC-BOOT-06 | 未变化的硬件身份和存储格式可以继续使用，不能为了统一版本号而盲改字段；保留固件版本也不因工具升版而降级。 |

0.2 使用产品契约 `esp-mosaico/v2`、Recovery ABI 2、布局版本 5 与 `mosaico-retained-test-2m-v2`。64 字节操作结果记录版本为 2；NVS 容器格式保持不变。启动意图使用 `mosaico_boot_v2`，基础安装使用独立 otadata 标记，读取、消费和清除规则见[启动契约](migration-0.2.md)。

## 5. 正常应用与更新后端

### 5.1 应用集成

正常应用使用公共 `esp_mosaico_app_recovery`，调用 `iris_ota_support_start()`，启用 `CONFIG_ESP_IRIS_OTA_DEFAULT_VIA_RECOVERY=y`，在 `project()` 前接入 `mosaico_idf_project.cmake`。这些是当前入口的业务职责；0.2 必须为最终入口提供公共契约，移除的旧入口只在迁移文档说明，不保留转发别名。

适配层不依赖 GSP、显示或特定用户 UI，不把 Vibe Mode 工程用作应用模板。应用通过有效构建配置声明角色、板型、布局与 Recovery ABI，构建门禁检查实际配置，包括复用 sdkconfig 的情况。

S31 产品的 Vibe Mode、正常应用及各自 bootloader 统一使用 QIO（`CONFIG_ESPTOOLPY_FLASHMODE_QIO=y`）。源码默认值、有效配置检查与复用产物检查须一致，拒绝遗留 DIO 配置。固定 SDK 为 ROM 加载阶段生成的 bootloader `--flash-mode dio` 参数保持原样，QIO 由 bootloader 初始化启用；这不构成产品运行模式的 DIO 例外。

### 5.2 更新类型与提交规则

| 编号 | 规格 |
| --- | --- |
| REC-UPD-01 | 普通应用代码更新仅在完整分区表一致时执行；新工程、布局或外部资源变化使用系统更新。不得修改布局以绕过前置检查。 |
| REC-UPD-02 | 普通更新进入 Vibe Mode，由唯一 Flash Writer 执行；USB、TCP、HTTPS Bridge 和 NAND 来源共享写入仲裁及验证后端。 |
| REC-UPD-03 | 更新计划校验目标芯片、板型、产品契约、角色、布局、范围、镜像描述、长度及实际 SHA-256；声明签名能力时必须验证签名。 |
| REC-UPD-04 | 应用、资源、分区表及 bootloader 的授权范围分别定义；普通应用包不能顺带替换保留维护固件。 |
| REC-UPD-05 | 接收、校验、提交和待重启阶段可查询；取消仅在允许边界生效，取消请求不等于擦写已停止。 |
| REC-UPD-06 | 提交后持久化操作结果和启动意图；主机连接或云端回执丢失不自动触发重烧，未知结果按原操作查询。 |
| REC-UPD-07 | 成功要求相同设备、新启动、目标固件身份、产品契约及应用健康；上传完成或仅 Vibe Mode 可达不代表完成。 |
| REC-UPD-08 | 保留 Core Dump、崩溃原因、失败计数和必要原始日志；普通更新不得默认擦除身份、Wi-Fi、配对或故障证据。 |
| REC-UPD-09 | 写入错误、空间不足、断线或重启不得把未校验镜像标为可启动；单槽原地写入不能声称任意断电均可无损回滚。 |
| REC-UPD-10 | 本地清单、bundle 和远程服务只接受 0.2 发布契约，旧包重新构建；不内置旧 manifest、旧布局或旧服务回退逻辑。 |

当前后端的未签名计划不能描述为已提供签名认证。0.2 的完整性验证、来源认证和危险目标写权限必须分别声明。

### 5.3 Vibe Mode 自更新与基础恢复

保留专用 Vibe Mode 自更新能力：只在产品明确允许时使用专用包，完整预检、暂存、写后校验及操作记录；不得与普通应用／资源更新混包。其单副本原地更新可能因掉电失去维护固件，必须提供 ROM 恢复路径，不宣传为双副本安全升级。

跨代协议与布局迁移使用配套基础安装包。基础包包含匹配 bootloader、分区表、启动选择数据和 Vibe Mode 镜像，附完整哈希与写入清单；不得默认擦除整个 Flash。其他分区的保留或重建范围由迁移清单明确。评审包替换前必须完成构建、契约和实机验证。

## 6. 维护功能保留

| 能力 | 0.2 要求 |
| --- | --- |
| Wi-Fi | 扫描、配置、连接与忘记操作保持可用；初始化失败可重试，不阻止 USB 维护服务启动 |
| Download Ideas | 保留首页、配对码、下载进度、取消和结果；后台预取不能自动开始安装 |
| HTTPS Bridge | 保留设备主动连接、配对、拉取与更新；明确提交授权、期限、取消、重试与结果持久化，不回退到旧服务协议 |
| NAND 更新 | 保留目录扫描、清单预检、选择与安装；挂载失败不格式化介质，安装时重新验证组件及布局 |
| 文件服务 | 注册受限 NAND 卷，保留目录、读写、删除、建目录、重命名和支持时的原子替换；禁止路径逃逸 |
| 截图与输入 | 保留 Vibe Mode 截图及已有允许接口的输入；按需截图支持控制分块，输入权限按产品策略声明 |
| 观测与诊断 | 保留内存、栈、inventory、操作结果、日志与崩溃证据；早期输出不依赖 Iris Worker |
| 设备 UI | 保留维护首页、配网、下载和 NAND 更新流程，耗时操作异步推进，不阻塞基本状态和取消 |

HTTPS Bridge 是产品服务，与 Iris TCP 数据端口不同；服务自身的版本号不能直接当作 Iris 0.2 版本字段。一次 NAND 安装必须保证清单和组件在校验／读取期间的一致性；单文件原子替换不代表整个 bundle 原子更新。

## 7. 原生开发与第三方工程边界

对 **Mosaico 工程**，原生 flash 使用匹配的 bootloader、分区表和应用产物，写入后 Vibe Mode 仍有效且可从按键、软件和故障入口进入。不能仅删除现有 CMake 的 flash 禁止逻辑就宣称完成；生成清单、布局和启动契约需要一并验证。原版 monitor 与 Gateway 控制端口互斥，OpenOCD/JTAG 按硬件能力使用。

已有匹配基础固件时，原生应用烧录保留它；空白设备或基础契约不匹配时，使用 recover 安装基础包，或使用包含匹配基础固件的完整原生安装清单。只写入 main_app 不能建立不存在的 Vibe Mode，指南必须区分首次安装与应用重烧。

对 **第三方 ESP-IDF 工程**，允许保留其自身分区表并原生烧录，不要求接入 Iris 或通过 Mosaico 更新。其烧录可能替换产品 bootloader、分区表、Vibe Mode 和数据；test subtype 不能保护分区免于另一份烧录清单覆盖。

返回 Mosaico 通过 `mosaico.py recover` 重装基础固件；第三方不需要提供 Iris。被覆盖的数据不承诺恢复，硬件身份可通过 ROM 读取并在新固件启动后核对。CLI 细节归 mosaico-tools 规格。

## 8. 0.1 → 0.2 文档迁移

迁移指南必须给出以下顺序；0.2 程序不执行旧契约转换：

1. 在旧环境记录硬件身份、固件、布局及业务数据，按需用旧工具导出日志、Core Dump、项目和数据，然后释放连接。旧环境只在隔离归档中保留。
2. 准备配套 0.2 bootloader、布局、Vibe Mode、应用和工具。定版前先在参考设备验证基础恢复与维护入口，再发布基础包。
3. 为每个实际擦写区间列出会替换和会保留的内容。布局候选虽沿用地址，仍不能跳过清单审核；凭据和元数据未变化可保留，无法由新契约解释的内容应按文档备份并显式重建，不在 0.2 固件内转换。
4. 通过原生 ROM 入口及 mosaico-tools 恢复执行器安装新的基础包；不要求 0.2 Gateway 与旧固件通信，不默认全片擦除。
5. 验证相同硬件身份、Vibe Mode 新启动、0.2 服务及新产品契约，重新建立需要的配对和主机权限。
6. 使用 0.2 契约重新构建正常应用与资源，通过默认产品流程安装；验证正常应用 → Vibe Mode → 正常应用闭环，以及原生 monitor、flash 和 JTAG。
7. 保留旧资料供离线参考；旧工程不能仅改版本字段后提交。需要返回旧环境时使用成套旧包和文档定义的重新安装流程，不由 0.2 自动降级。

新布局、ABI 和元数据的最终标识及具体备份／重建命令属于发布前必须补齐的迁移附件，本规格不虚构已经存在的迁移命令。

## 9. 验收

| 编号 | 场景 | 通过条件 |
| --- | --- | --- |
| REC-AC-01 | 标准 console | 正常应用和 Vibe Mode 无主机扩展均可原生 monitor；未启动 Iris 时仍有平台支持的启动日志 |
| REC-AC-02 | Mosaico 原生 flash | 完整清单正确，正常应用可启动，Vibe Mode 和各入口保留，无误写受保护数据 |
| REC-AC-03 | 启动入口 | GPIO7、软件、连续崩溃、无效应用与返回应用分别实测；不以 test 分区存在代替验证 |
| REC-AC-04 | 默认更新 | 相同布局 app-update、布局／资源 system-update 均校验并健康启动，错误类型不扩大写入范围 |
| REC-AC-05 | 双链路 | 无控制前置请求的数据发现与认证、任务绑定、掉线策略通过，重启拒绝旧绑定 |
| REC-AC-06 | 弱链路 | 仅 UART／Serial-JTAG 的状态、RPC、截图可用；Iris 固件传输明确不可用，ROM 恢复仍可执行 |
| REC-AC-07 | 功能回归 | Wi-Fi、Download Ideas、Bridge、NAND、文件、输入、观测及自更新在声明配置中保留 |
| REC-AC-08 | 故障恢复 | 中断、取消、校验错误、内存不足及断电不产生错误成功；自更新失效可通过 ROM 恢复 |
| REC-AC-09 | 第三方接管 | 第三方不改分区表可原生 flash monitor；之后 recover 重建基础固件并通过身份和功能核验 |
| REC-AC-10 | 调试 | OpenOCD/JTAG 与适用的 monitor 可用，Gateway 不因暂停反复复位 |
| REC-AC-11 | 0.2 迁移 | 文档路径无需跨代 Iris 通信即可完成；旧包／旧契约明确拒绝，无兼容解析与转换分支 |
| REC-AC-12 | 资源与生命周期 | 混合负载符合发布预算；流、快照、网络和更新重复启动／取消无泄漏，Writer 始终单一 |

记录源码与 SDK revision、配置、BIN/ELF/分区表和基础包哈希、完整写入清单、硬件身份、Boot ID、原始日志与 Operation。主机测试与固件构建不能替代 GPIO、USB、烧录、断电及 JTAG 实机验证。

## 10. 实现与验证范围

test 分区、启动意图、ABI／布局、schema 3 基础包与 0.2 System Update 已定版。
默认 App Update、System Update、Vibe 自更新、物理 GPIO7 入口、连续 20 次软件
往返以及最终 5 项崩溃恢复矩阵已通过。S31 固件与 bootloader 统一 QIO。
Iris 正常重启及 bootloader 异常重启路径均停止残留 USB DMA。

评审包与证据见[0.2 验证记录](validation-0.2.md)。JTAG 按要求跳过；外部 Bridge
0.2 服务、完整断电矩阵及全部性能预算仍未完成设备验证。High-Speed USB 组合
flash monitor 的首次打开存在锁定 monitor 版本的重枚举时序限制，单独 monitor 已通过。

## 11. 参考资料

- [当前组件说明](../README.md)与[固件功能说明](../firmware/vibe-mode/README.md)
- [产品契约](../product_contract.json)与[Recovery ABI](../include/mosaico_recovery_contract.h)
- [当前 bootloader](../firmware/vibe-mode/bootloader_components/main/bootloader_start.c)
- [当前应用适配](../components/esp_mosaico_app_recovery/iris_ota_support.c)
- [公共应用集成](../../mosaico-tools/docs/application-integration.md)
- [原生烧录清单校验](../../mosaico-tools/cmake/system_update.cmake)
- [0.2 验证记录](validation-0.2.md)

以上资料保留其当前版本含义。实施 0.2 时同步更新拥有该契约的模块，不在 Iris 通用规格中复制产品细节。
