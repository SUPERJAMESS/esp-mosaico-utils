# ESP-Mosaico 九设备自更新问题记录

2026 年 10 月 20 日，从 utils `33383d620d13ba2e62f010d71575e79de9ed426f` 源码构建 Vibe Mode，以单台和九台并发方式测试自更新。单台 10 次全部通过；九台并发首轮 8 台通过，D06 在上传提交阶段被 Gateway 拒绝。用户手动让 D06 进入 Vibe Mode 后续跑至第 9 轮，发现 D09 崩溃计数增加，随后按用户要求停止测试。

## 停止时结果

单台 10 次和九台并发 9 轮共 91 次计划提交，其中 44 次通过完整验收、46 次被主机 HTTP 429 拒绝、1 次设备独立验收失败。九台并发第 10 至第 20 轮和普通应用更新测试未执行。停止时 Gateway 活动操作和主机写入器数量均为 0；仅结束本任务的前台保活客户端，保留设备固件和现场记录。

第 9 轮九台均已接纳更新，D09 的操作 `3a6a39e0-11e2-48f5-8ab5-7b2e1d2d67eb` 报告 `succeeded`、healthy 和目标 ELF 正确，但独立 live 检查发现 `crash_count` 从 0 变为 1、`reset_reason=7`，失败镜像摘要指向 V-A，失败分区为 `0x20000`。该轮按设备验收失败保留，不能算作稳定更新成功。具体崩溃原因尚未定位；停止后没有继续复现或解码。

此前由 Gateway 归档的 Core Dump 已复制到证据目录，其归档发生在第 9 轮写入前，不能未经解码便认定为本次新增崩溃的 Core Dump。设备上的后续崩溃证据未作清除或恢复操作。

## 构建和现场

- ESP-IDF revision：`7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe`，源码版本 6.2.0，目标 ESP32-S31。
- Vibe Mode 版本保持 `0.2.0`。两个样本使用同一源码和配置，通过重新编译应用描述的构建时间产生不同 ELF SHA，交替安装以避免相同镜像跳过实际写入。
- 源镜像均为 1,667,600 字节，约占固定槽的 90.9%；包内组件依产品契约补齐至 1,835,008 字节。
- 包仅含 `recovery` 组件，写入范围 `0x20000` 至 `0x1e0000`。使用消费应用分区 CSV 重新生成表文件，其摘要与九台实时读取值一致；该表只作为前置条件，没有放入写入组件。
- 测试使用消费工作区的独立 `mosaico.py` launcher 和配置，工具、Gateway 与被编译的 Iris 均来自上述 utils revision。未修改消费工作区原配置和其已有 utils 工作树。

| 样本 | ELF SHA256 |
| --- | --- |
| V-A | `e70cbc020c294c09b384135ed31675206d52be405295868906e6f67cec2ed17f` |
| V-B | `ad05e570b83112bb0eb937efa9cafc0097755333a0b69f7309b5b4362c3d7798` |

## 已确认的上传限流和错误分类问题

北京时间 16:11:36，九台同时发起 V-A 自更新。D06（USB serial `30eda0f4595e`）在约 3.65 秒后返回 CLI 错误，原始子进程输出为：

```text
HTTP 429
code: resource_limit
message: large_requests capacity is 2; retry after active work completes
resource: large_requests
limit: 2
```

产品 CLI 最终输出 `error=outcome_unknown`、退出码 5，`details.result={}`，没有 operation ID，也没有自动排队或重试。其余八台在约 40.8 至 42.2 秒内完成更新，通过操作结果、目标 ELF SHA、healthy、新 Boot ID 和 5 秒稳定性复查。

源码和现场证据表明，本次 D06 请求在 HTTP 接纳阶段被拒绝：

1. [Gateway 大请求预算](../../ESP-Iris/components/esp_iris/tools/iris_gateway/resource_limits.py)默认 `large_requests=2`。
2. [HTTP 资源中间件](../../ESP-Iris/components/esp_iris/tools/iris_gateway/gateway.py)在调用 System Update handler 前检查预算；超限返回 429 和 `Retry-After: 1`。该预算作用于 HTTP 大请求处理阶段，已接纳的设备更新操作可继续并发执行。
3. [Iris CLI HTTP 错误处理](../../ESP-Iris/components/esp_iris/tools/iris_gateway/cli.py)把错误作为文本异常输出；[产品 CLI 操作等待逻辑](../../mosaico-tools/tools/mosaico_cli/gateway.py)只从 stdout 解码结果，遇到非零返回且无状态时分类为 `outcome_unknown`，丢失了结构化的接纳拒绝原因。
4. 首次失败后，D06 的 Boot ID、旧固件 ELF SHA、设备更新 receipt 均保持原值，队列为空，设备仍可实时查询。

因此，本次需要重新发起命令的直接原因已定位到主机上传接纳和错误分类。这个结果不能作为设备 Flash 写入失败、死机或 USB 硬件故障的证据，也不足以解释客户报告的所有不稳定场景。

## 手动介入与继续测试

用户确认在首次现场保存后，手动让 D06 进入 Vibe Mode。随后观察到 D06 Boot ID 改变、`reset_reason=1`；这一变化按用户报告归入手动介入，不计为自发复位。D06 仍运行旧 Vibe Mode，原更新 receipt 未改变。

继续测试前，九台均重新核对为 live Vibe Mode，具备 OTA 和 System Update 能力，队列为空、崩溃计数为 0。从第 2 轮接续，不重放第 1 轮，也不把首次失败改记为成功。

续跑期间，对原始日志明确显示 HTTP 429 `large_requests` 的请求记录“提交前拒绝”，继续下一轮；对无法确认结果的请求仍冻结后续写入并核对原操作。不自动重试本轮被拒绝的命令。每台后续目标根据实际运行镜像在 V-A/V-B 之间交替，避免把未写入后的相同镜像提交当作一次实际烧写。

前八轮中，多轮仅接纳两至三台更新，无法形成完整的九台设备写入压力。北京时间 16:22:25，测试工作区的独立 launcher 增加实验用上传排队：只对向 Gateway 提交 `.irisfw` 的子命令持有文件锁，收到接纳回执就释放；设备传输、Flash 提交、重启和健康验证继续并发。该变化只影响随后启动的 CLI 子进程；生效边界、完整 launcher 和原始版本保存在运行目录。后续统计须分别报告原始提交方式与排队提交方式，并用实际操作时间证明写入重叠，不能仅凭九个 worker 宣称九路写入。当时产品工具和固件源码没有修改。

## 设备侧观察

首轮结束后的九台 live 快照均可读取，崩溃计数为 0、无 crash loop、`invalid_frames=0`，bootloader 和分区表摘要保持原值。八台成功设备的内部堆最低余量约 74 KiB，PSRAM 最低余量超过 11 MB；快照没有显示内存耗尽。这些是有限时间窗口内的观察，不能替代长时间运行或电气测量。

另保存两类待核对的设备日志：

| 观察 | 设备与证据 | 当前判断 |
| --- | --- | --- |
| LittleFS 目录元数据读取报错 | D03，`Corrupted dir pair at {0x0, 0x1}`，新 Vibe Mode 启动约 2530 ms 时出现 | 需要区分既有文件系统状态、几何参数和驱动读取差异。当前正常挂载路径先执行只读卷几何探测，失败即返回，并关闭自动格式化；没有进行修复或格式化。自更新 healthy 不代表 NAND 可用。 |
| 触摸报文与 I2C 超时 | 多台出现 `Malformed report header: first=0xab marker=0xab`，部分出现 `Report read failed: ESP_ERR_TIMEOUT`；旧固件启动日志也有异常报文 | 需要单独验证物理触摸和报文解析。目前没有建立它与 D06 上传拒绝之间的关联。 |

首轮未覆盖普通应用的大镜像、分区布局往返和多资源组件更新，不能将本轮自更新结果外推到这些场景。

## 后续原因分析与源码修复

用户随后要求分析并修复上一轮问题，恢复了本次定向回归。原始停止时统计保持不变。

### 主机提交前拒绝

根因是九个 CLI 同时上传与 Gateway 的两个大请求预算发生冲突，加上 CLI 丢失结构化 HTTP 错误，导致明确的提交前拒绝被误报为结果未知。保留原预算，避免通过提高并发上限放大内存占用；System Update 提交仅针对 `429/resource_limit/large_requests` 按 `Retry-After` 有界重试，使用同一个 operation intent ID 和同一份冻结包。收到接纳回执后不重发，连接中断、5xx 和其他资源错误不自动重试。超过接纳等待预算时，保留 HTTP 状态、资源原因以及 `write_started=false`。

相关实现：[Iris 提交处理](../../ESP-Iris/components/esp_iris/tools/iris_gateway/update_submission.py)、[产品 CLI 错误分类](../../mosaico-tools/tools/mosaico_cli/gateway.py)。新增九客户端真实 HTTP 中间件回归，验证每个 intent 只进入更新 handler 一次，以及不确定连接失败不重放。

### D09 看门狗复位与错误验收

已确认两次 `reset_reason=7` 的现场：原第 9 轮，以及从旧 V-B 首次安装修复版的迁移操作 `4ec9d429-6383-4e04-8520-6ac2adbd87d0`。后一次目标镜像和提交 receipt 匹配，但新启动记录再次出现崩溃计数 1；新验收逻辑正确返回失败。两次退出时运行的均为旧固件。设备保留的 Core Dump 为更早镜像 `4b8e057ec6e179cb8b8abd4dd032312649984cd64ca49d78c4329855aa89fa5b`，不匹配本次样本，因此不能用于推断这两次看门狗复位的调用栈。

源码发现两条独立重启路径：设备提交成功后创建 1800 ms 延迟任务直接调用 `esp_restart()`，Gateway 收到提交响应后又请求 Iris 在 250 ms 后重启。两条路径没有共同的重启状态；SDK 在关闭调度器前运行 shutdown handlers，存在重入关机过程的竞争窗口。修复将设备提交后的延迟重启接入已有的 `esp_iris_schedule_restart()`，与主机请求共享单一截止时间和执行路径，取消私有重启任务。协议、ABI、固件版本以及 Bridge、延迟提交的职责保持原契约。

这是已消除的重启竞争风险；现有日志没有当次有效调用栈，尚不能唯一证明它就是原看门狗事件的底层触发点，也不能排除启动早期的问题。须将源码判断和后续复现结果分别记录。

另一个已确认缺陷是 Gateway 只检查 HEALTHY、身份和目标摘要，没有检查崩溃状态，导致原第 9 轮报告成功。更新验收和只读结果核对现均检查崩溃计数、crash loop、recovery pending 和状态读取错误。只读核对不会把带有异常崩溃状态的启动提升为成功，也不会重放写入。

相关实现：[提交后重启](../firmware/vibe-mode/components/app_system_update/src/update_flash.c)、[验收](../../ESP-Iris/components/esp_iris/tools/iris_gateway/update_acceptance.py)、[只读核对](../../ESP-Iris/components/esp_iris/tools/iris_gateway/reconciliation.py)。

### 修复版实机回归

使用同一固定 ESP-IDF checkout，从上述修改后的源码构建两个新样本，版本仍为 `0.2.0`，镜像均为 1,667,584 字节。未使用或替换发布目录的预编译固件。新工作区 launcher 直接导入正式工具源码，没有上一轮实验用文件锁补丁。

| 修复样本 | ELF SHA256 |
| --- | --- |
| Fix-A | `368b618bb9c5a9f88c1c21b771053c7b6c104e83f217ad54b5463e61d1cef14f` |
| Fix-B | `27ba8645ab8624568717bc00ab35385051b5ec7b5669d4f164765eef8e032241` |

| 阶段 | 结果 |
| --- | --- |
| D09 旧 V-B → Fix-A | 目标和 receipt 匹配，但再次发生看门狗复位；新验收正确判失败。作为迁移诊断单独保留，不计入通过数。 |
| D09 首次由修复版执行更新 | Fix-A → Fix-B，通过，正常软件复位、崩溃计数 0。 |
| D09 单台三轮 | 3/3 通过，另外八台 Boot ID 不变。 |
| 九台并发第一轮 | 9/9 通过，其中八台从旧固件迁移；D09 已运行修复版。 |
| 九台均运行修复版后再并发五轮 | 45/45 通过。 |

正式定向回归合计 57/57 通过；加上修复版首次诊断更新，共 58 次完整验收通过。另保留上述 1 次旧固件迁移失败，整个过程没有新增人工介入。每次均在两个不同摘要间切换，核对 operation、目标 ELF、HEALTHY、新 Boot ID、持久化 receipt、未变化的 bootloader/分区表摘要，并在 5 秒后复查。全部 57 份回归重启后快照为 `reset_reason=3`、`crash_count=0`、`crash_state_error=0`。

六轮九台提交阶段的共同重叠窗口分别为 13.243、11.954、13.038、11.962、11.240、11.746 秒，来源是操作进度时间戳。该证据表明九台更新操作的提交阶段重叠；它不是电气层面 Flash 总线时序的测量。最终九台均完成实时检查，队列为空，仍在修复版 Vibe Mode；结束本任务的前台保活客户端时活动设备操作和主机写入器均为 0。

本次未重跑普通应用、多资源和分区布局往返，不能将自更新结果外推到这些场景。D03 文件系统和触摸/I2C 观察仍分别保留，不以本次通过结果宣称已经修复。

### 软件验证与使用边界

- ESP-Iris Python 全量：796 passed、1 skipped。首次广测有一项 takeover 时序测试失败，单独复跑及最终全量均通过。
- `mosaico-tools/tests` 与 `esp-mosaico-vibe-mode/tests` 联合全量：914 passed、146 skipped、120 subtests passed；5 项显示资源发布测试因本机缺少 `libraqm` 失败，错误均指向未修改的 `shared/ideas/font.py` 依赖检查，未将它们记为通过。
- macOS 下补齐隔离 OpenSSL 头文件与静态库后，直接编译执行实际 System Update 原生 C 后端 fixture，通过重启调度、Bridge 和延迟提交检查。Iris 全量同时包含已有的重复重启截止时间合并和发送刷新检查。
- 两次 ESP32-S31 源码构建、产物摘要和包内布局/大小检查通过；`git diff --check` 通过，修改后的 Iris 模块通过 Python 3.8 语法解析检查。

修复保存在当前 utils 工作树，九台已安装本次源码构建版本。消费工作区原有 launcher、配置和其已有 utils 引用未修改；此次验证使用证据目录中的独立 launcher。通过原消费 launcher 使用主机修复时，也需要同步这些 utils 源码修改。

### 提交后继续测试

用户要求提交修复并继续后续测试。修复已提交为 `c0fa866`；源码构建的应用压力夹具提交为 `3fdbcb9`。后续运行目录为 `.codex-runs/flash-repro/20261020T085849Z-continued-tests`，仍位于消费工作区。

九台自更新补跑第 7 至第 20 轮，126/126 次通过；与前六轮合计，完整接纳的九台自更新达到 20 轮、180/180 次通过，无新增介入。前述旧固件迁移异常和原始限流失败继续单独保留，不修改历史结果。

普通应用测试使用源码构建的 A、A2、B、C 四个冻结样本。A/A2 镜像分别为 388,992 和 389,008 字节；B 为 12,181,376 字节，占 `main_app` 的 89.79%；C 为 389,440 字节，另含两个各 1,048,576 字节的资源镜像。L1 沿用现场布局；L2 将 `main_app` 从 `0xcf0000` 缩为 `0xaf0000`，在原应用区内增加 `stress_a@0xd00000` 和 `stress_b@0xe00000`，保持 `ui_apps@0xf00000` 不变。所有包均不包含 NVS、`ui_apps`、bootloader 或 Vibe Mode 写入组件。

夹具在报告 HEALTHY 前验证编译进镜像的载荷和完整资源摘要；只读 RPC 返回样本身份及摘要，主机逐次比较原 `ui_apps` 摘要。B 构建中的槽位警告是该大应用不能装入较小的 `vibe_mode` 槽；实际包只定位 `main_app@0x210000`，并已通过其容量校验。原发布预编译固件未替换。

普通应用最初按单台 10 循环/40 次、九台并发 20 循环/720 次启动。用户随后要求改为批量优先：批量失败再回退到故障设备单台定位；批量通过后仅做一轮单台四步验证。调整前已通过一轮四步及第二轮前两步，共 6 次单台更新，另有 1 次初始化；正在进行的写入完成后才切换调度，未中断 Flash 写入。上述历史结果单独保留。新调度运行目录为 `.codex-runs/flash-repro/20261020T092620Z-batch-priority`，计划九台 20 循环/720 次及通过后单台 1 循环/4 次；实际结果以该目录的 `app-progress.json`、`app-ledger.jsonl`、`app-summary.json` 为准。

### 批量优先测试：D05 首次应用更新失败，转单台

九台同时初始化到源码应用 A 时，8 台完整验收通过，D05（MAC `30:ed:a0:f4:53:94`）失败。操作 `e65bb9d4-6157-4db2-9449-40a46b7225d3` 已接收全部 393,088 字节，持久化 receipt 为该操作且 `last_result=0`，但目标角色应为 normal，实际仍为 recovery。写入器 ELF 为 Fix-B `27ba8645…032241`，失败后仍是同一 Vibe Mode 镜像；实时状态为 `reset_reason=7`、`crash_count=1`，失败地址 `0x20000`，失败 Boot ID 与写入器 Boot ID `2574886067860866340` 匹配。主机正确判定失败，未将 receipt 成功等同于整次更新成功。

保存的 29,504 字节 Core Dump 摘要为 `46c7f4785c81c564add8ff3a892d2293802fe83fc476e5dcf55d773778d830d4`，内含 ELF `10af6e0d…afc037c`，与本次 Fix-B 不匹配；同一 Core 在此次写入前已被保存。因此，其中的 TLSF double-free 断言不能作为本次看门狗复位的根因证据。现有日志未给出本次匹配栈，根因继续待定位；前述自更新通过不能证明普通应用切换不存在问题。

运行目录 `.codex-runs/flash-repro/20261020T092620Z-batch-priority` 保留原始失败、状态、日志、Core 和八台通过记录。按用户要求冻结后续批量，转 D05 单台：保留原崩溃状态后重测相同 A，再执行 A2→B→C→A；单台结果独立记入 `fallback-ledger.jsonl`，不覆盖批量失败。计划中的 720 次普通应用并发尚未执行，不计为完成。

D05 单台重测相同 A 再次失败，操作 `888545d3-59c5-44d1-a650-df047dfe5e4c`；同样收齐 393,088 字节、receipt 成功，但 `reset_reason=7`，重新进入 Fix-B Vibe Mode，`crash_count=1`。该记录证明单台也会发生此现象，不能仅归因于九台主机并发。随后仅执行一次独立软件重启（`b7802fe6-8be8-41f6-a72f-d3c0a6d2bc80`），正常 `reset_reason=3`、新 Boot ID，既有崩溃状态保留；未再次写应用。普通重启成功也不足以排除更新后的重启/启动路径问题。

后续 A2/B/C/A 单台循环因 A 初始化重复失败未启动，后续批量不再继续写入，以保留复现现场。最终汇总为 `batch-fallback-conclusion.json`，九台只读结束快照为 `batch-fallback-final-states.json`。诊断脚本的首个尝试因证据目录命名冲突在提交前退出，0 次设备写入；第二次的失败记录对 operation ID 的继承错误以追加更正保留，原始命令输出和实际 terminal operation 分别存在，不修改原批量台账。

### 待定位：大固件更新速率下降

用户在后续测试中反馈：此前更新速率约 **300k**，当前大固件更新仅 **60k+**。按用户原始表述记录；具体单位、统计窗口、历史固件/工具版本及接线条件留待定位时核对，不把两个数字直接当作相同口径的测量。

当前关联样本为 B，应用镜像 12,181,376 字节；D01 操作 `04e07cc3-91eb-4c5b-ae3b-37c7e180e143` 完整验收耗时 243.673 秒，折算约 50,000 字节/秒。该时间包含模式切换、重启、校验和 5 秒稳定性复查，不能等同于纯传输速率。包、ELF、配置、操作进度时间戳、原始日志和逐次结果均保存在后续测试运行目录；另记入 `performance-issues.jsonl`。

此问题独立于更新成功率和先前 HTTP 接纳限流记录。本轮继续执行用户调整后的批量优先顺序，不更改窗口、分块、超时、日志或 USB 参数。后续定位再对照相同包和统计口径区分上传接纳、实际传输、Flash 写入/读回及重启验收耗时；当前不推断根因。

## 证据位置

消费工作区运行目录：

```text
/Users/ts/lsh/esp-mosaico-vibe/.codex-runs/flash-repro/20261020T075456Z-vibe-selfupdate
```

- `samples.json`、`inputs/`、`build-logs/`：源构建产物、包校验计划、完整编译日志和摘要。
- `first-round-issues.json`、`interventions.jsonl`：首轮问题与用户手动介入记录。
- `selfupdate-ledger.jsonl`：逐次原始结果；首次失败保留为失败。
- `selfupdate-parallel-summary.json.first-failure`：首轮冻结结果，8 成功、1 失败。
- `evidence/self-parallel-001-D06-update/`：失败调用、stdout/stderr 和返回码；关联原始日志为 `mosaico/20261020T081138Z-system-update-2/raw.log`，HTTP 429 位于第 23 行。
- `device-side-analysis.json`、`device-log-analysis.json`、`evidence/D*-final-frozen-*/`：首轮现场的实时状态、内存和日志。
- `resume-preflight.json`：用户介入后、继续第 2 轮前的九台身份和固件核对。
- `experiment-changes.jsonl`、`mosaico.py.before-upload-queue`、`mosaico.py`：上传提交条件的变化和两种 launcher。

运行目录内的继续测试摘要与逐次台账记录后续结果；本记录中的首轮结论不随续跑覆盖。

修复与定向回归运行目录：

```text
/Users/ts/lsh/esp-mosaico-vibe/.codex-runs/flash-repro/20261020T083043Z-fix-analysis
```

- `fix-validation-summary.json`、`selfupdate-ledger.jsonl`：定向回归汇总和逐次结果，诊断迁移失败单独记录。
- `samples.json`、`inputs/`、`build-logs/`：修复版源码构建样本、ELF/map、布局校验和完整日志。
- `source-state.json`、`source-snapshot/`、`source.patch`：源码 revision、工作树修改摘要和快照；补丁文件外的新文件也独立保存。
- `operation-stage-evidence.json`：每个操作的阶段时间边界，用于核对并发重叠。
- `D09-retained-core.bin`、`evidence/bootstrap-D09-*`、`evidence/fixed-runtime-D09-*`：旧 Core Dump、迁移异常和首次修复版执行更新的现场。
- `final-device-states.json`、`final-host-health.json`：最终九台实时状态和关闭任务保活客户端前的主机状态。
- `iris-tests.log`、`product-tests.log`：最终软件全量结果，包含上述五项环境依赖失败。`evidence/host-after-release/` 确认 Gateway 已退出。
