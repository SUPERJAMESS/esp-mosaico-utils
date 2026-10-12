# Vibe Mode 0.2

当前源码使用 ESP-Iris 0.2、Recovery ABI 2 和 `mosaico-retained-test-2m-v2`
布局。`vibe_mode` 为 `app/test`，`main_app` 为 `app/ota_0`；二进制产物名称
仍是 `factory.bin`。`prebuilt/recovery` 已整体更新为经过实机验证的 0.2.0
基础包，S31 产品统一使用 QIO。具体覆盖范围与未验证项目见
[0.2 验证记录](../../docs/validation-0.2.md)，不以基础包更新代表全部平台验收。

- [启动契约与迁移](../../docs/migration-0.2.md)：NVS 一次性维护意图、基础安装
  otadata 标记、保护范围与从第三方固件恢复。
- [应用集成](../../../mosaico-tools/docs/application-integration.md)：原生
  `idf.py flash monitor` 与默认 Mosaico 更新流程。
- [软件规格](../../docs/esp-mosaico-recovery-software-spec-zh.md)：完整功能和验收边界。

保留 Wi-Fi、Download Ideas、HTTPS Bridge 维护后端、NAND、文件、截图、输入及维护自更新。
高速 USB 使用 console/data 双 CDC；TCP 使用独立端口；控制链支持按需截图。
历史 Bridge 维护后端必须使用 control protocol 2 的独立写入授权，产品 profile 变更为
`iris-s31-test-layout-v2`；需要配套远程服务，不回退到旧授权路径。
Download Ideas 页面使用下文的六位编号直接下载契约。
System Update 清单使用 `esp-iris-system-update/0.2`，基础包 manifest 使用 schema 3。

### Ideas 下载与开机应用（当前源码）

Download Ideas 输入六位应用编号（字符串，保留前导零），直接解析对应应用的最新发布。
不注册配对会话，也不后台预取配对码。编号必须由云端永久、唯一地映射到 application ID。
这部分源码使用下面的新云端契约；现有 Bridge 服务是否实现该契约仍需联调确认。

- **下载**：将固件、清单及展示资源写入 `/nand/ideas/<code>/current/`，不写应用 NOR、
  不切换启动目标、不重启。每个编号只保留一个完整版本，最多 16 个 Ideas。
- **检查更新**：以已下载的 application ID、release ID 查询最新发布，只报告是否有新版。
  “Download latest” 替换 NAND 当前版本，保持 NOR 中原应用及其开机标记。
- **设为开机应用**：完全离线，从 NAND 重新校验所有文件，然后写 NOR、验证读回、
  提交分区表和下次启动目标。完成后仍停留在 Vibe Mode，可继续选择另一个 Idea。
  “Restart now” 启动当前已选的开机应用。界面不显示“已安装”状态。
- 开机应用身份保存在保留的 `sysmeta` 分区独立 `ideas` 命名空间；标记同时核对
  实际 NOR 应用与分区表的 SHA-256。它不依赖 NAND 中当前版本是否等于 NOR 版本。
- 新包先在 `.incoming` 完整下载并校验，再通过目录重命名替换 `current`；`.previous`
  只用于中断恢复，并在替换完成或下次扫描时删除，不提供历史版本列表。

安装仍遵循单应用槽的断电边界：不是 A/B 回滚。擦写前设置 Vibe Mode 启动意图，
成功后清除；失败可返回 Vibe Mode 重试。Ideas 不允许更新 bootloader 或保留的
Vibe Mode 镜像。分区表改变后，普通 USB/TCP OTA 写入须等重启重新加载分区描述；
Ideas 可继续使用经过验证的新分区表描述重新安装。

#### 云端契约 `mosaico-ideas/download/1`

沿用 `CONFIG_IRIS_FACTORY_BRIDGE_SERVER_URL` 的 HTTPS origin 和
`CONFIG_IRIS_FACTORY_BRIDGE_BOARD_ID`；语言由 `CONFIG_IRIS_FACTORY_IDEAS_LOCALE`
指定（默认 `en`）。证书验证开启，不跟随重定向。

`POST /api/v1/ideas/resolve` 请求示例：

```json
{
  "code": "042731",
  "board_id": "esp-mosaico",
  "locale": "zh-CN",
  "recovery_abi": 2,
  "layout_version": 5,
  "flash_size": 16777216,
  "application_id": "weather-station",
  "current_release_id": "weather-20261012-1"
}
```

首次下载不发送后两个字段。成功始终返回 HTTP 200 和**完整最新发布描述**；
无此编号返回 404。设备按 release ID 判断更新，不按版本字符串排序。
云端按 board、ABI、layout、Flash 容量过滤发布，选择当前语言或明确的回退语言。
响应结构如下（SHA-256 字段应填写实际 64 位小写十六进制摘要）：

```json
{
  "schema": "mosaico-ideas/download/1",
  "application": {
    "code": "042731", "id": "weather-station",
    "release_id": "weather-20261012-2", "version": "1.2.0",
    "name": "天气站", "description": "今日天气与预报", "locale": "zh-CN"
  },
  "system_manifest": {
    "schema": "esp-iris-system-update/0.2", "release": "1.2.0",
    "target": {"chip_id": 32, "flash_size": 16777216},
    "target_layout_sha256": "<partition-table SHA-256>",
    "components": [
      {"id": 1, "kind": "partition_table", "target_offset": 32768,
       "size": 4096, "sha256": "<partition-table SHA-256>", "file": "partition-table.bin"},
      {"id": 2, "kind": "application", "target_offset": 2162688,
       "size": 1048576, "sha256": "<application SHA-256>", "file": "app.bin"}
    ]
  },
  "files": [
    {"name": "partition-table.bin", "path": "/api/v1/ideas/files/table-blob", "size": 4096, "sha256": "<partition-table SHA-256>"},
    {"name": "app.bin", "path": "/api/v1/ideas/files/app-blob", "size": 1048576, "sha256": "<application SHA-256>"},
    {"name": "card.gspb", "path": "/api/v1/ideas/files/card-blob", "size": 21648, "sha256": "<presentation SHA-256>"},
    {"name": "icon.png", "path": "/api/v1/ideas/files/icon-blob", "size": 6105, "sha256": "<icon SHA-256>"}
  ],
  "presentation": "card.gspb", "icon": "icon.png"
}
```

`GET files[].path` 返回不可变原始文件。只允许同 origin 的
`/api/v1/ideas/files/<单段标识>`，不接受外部 URL、查询参数或路径穿越。
固件文件名、长度和摘要必须与 `system_manifest.components` 一一对应；分区表必须
是第一个组件，且只能有一个 `0x210000` 应用。额外数据组件仍受 Recovery Flash
策略验证。描述最多 16 KiB，内嵌 system manifest 最多 3 KiB，最多 8 个固件组件、
10 个文件，总计最多 32 MiB。未被固件或展示字段引用的文件会被拒绝。

#### 固定文字、图标及字体子集

`presentation` 是可选的单场景 deployable GSPB，使用同一个固定卡片模板，包含
图标、发布时确定的本地化名称/描述及编译后的字形子集，最多 1 MiB。
“View info” 从 NAND 校验并加载该卡片，左上角 72×72 区域返回 Vibe Mode。
卡片没有可执行设备操作；离开后释放资源，保留 Vibe Mode 原界面。
列表图标使用独立 PNG（最多 64 KiB、96×96），缺省使用内置图标。
Vibe 自身不携带完整多语种字体；列表中超出内置字符集的名称显示编号，
完整本地化名称与描述在卡片中显示。

发布端使用 `tools/prepare_idea_presentation.py`，提供 `--code`、`--name`、
`--description`、`--locale`、`--font`、`--icon`、`--output` 和 `--gspc`。
字体须覆盖该语言。工具用 GSPC 0.5.0 生成 `card.gspb`、64×64 `icon.png` 和
`presentation.json`（展示元数据及摘要）；云端将这些文件加入发布描述，
不必下载字体全集。设备重启后仍能从 NAND 查看它们。

2026-10-12 使用本地 HTTPS 解析/文件服务及真实 ESP32-S31、NAND/NOR 完成端到端
验证：六位编号直接下载、两个 Ideas 共存、中文名称/描述及字形子集显示、独立列表
图标、按 application/release ID 检查更新、损坏文件拒绝且保留旧版、单版本替换。
关闭服务后连续安装 `000001 -> 000002 -> 000001`，安装期间保持 Vibe Mode；
下载 `1.0.1` 后重启仍运行 NOR 中的 `1.0.0`，离线安装后才启动 `1.0.1`，
通过应用身份、固件摘要和健康 RPC 验证。所有设备操作均经过消费 workspace 的
`mosaico.py`；本地测试 URL 和 CA 只用于候选构建，没有修改生产默认值。

真机验证修正了 GSP 1.4 在 S31 ROM 上解码 PNG 时缺少线性输出标志的问题，
并为多行列表显式配置图标槽。卡片预览期间，实体触摸与 USB 输入统一进入卡片，
避免向暂停的主界面注入事件；见[对应原生回归](../../tests/test_ideas_presentation_host.py)。
此功能不替换已评审的 `prebuilt/recovery`。生产云端联调和实际断电恢复仍需另行验收。

Vibe Mode 关闭 Wi-Fi modem sleep，优先保证维护传输和控制响应；普通应用自行
选择省电策略。固定 S31 SDK 曾在 `pm_process_tim` 路径发生看门狗复位，原始
Core Dump 和采用此策略后的更新结果保存在实机验收证据中。

bootloader 仅读取 `sysmeta` 中的启动意图，不修改 NVS。其专用读取器复用锁定
SDK 的私有 NVS 页、条目和 CRC 校验接口，按运行时相同的页序号、条目顺序
查找命名空间及 U32 值；支持运行时可加载的多 ACTIVE 页和重复命名空间记录。
读失败、类型不符或无法确定的回收状态选择 Vibe Mode。升级 SDK 时须重新核对
该私有接口和运行时查找规则，并运行 `tests/test_mosaico_boot.py` 及启动往返实测。

S31 bootloader 在加载应用 RAM 前停止上一个固件遗留的高速 USB DMA，覆盖
panic/watchdog 跳过正常 shutdown 的路径；Iris 在正常软件重启前也停止其 USB
控制器。锁定 SDK 仅复位 CPU 时会保留旧 DMA 地址，旧应用的 SETUP 缓冲区
可能与新应用代码重叠。Vibe Mode 的 panic 处理器放入内部 RAM，以保留
Flash/cache 故障的诊断能力。USB Serial/JTAG 控制器不受此处理影响。

构建仍使用锁定 ESP-IDF、GSP 1.4.0 与 GSPC 0.5.0。`recovery-current-bundle`
仅生成未评审候选；完成设备身份、各启动入口、更新、回退及原生工具实测前，
不得运行 `update-recovery-prebuilt` 发布候选。产品原生应用 flash 不写维护镜像；
本维护工程自身的 flash 清单明确将 `factory.bin` 写入 `vibe_mode`。

## 以下为 0.1 发布记录和历史操作资料

以下内容保留旧版本的真实测试结果，不能用来证明 0.2 已验收。
旧布局、manifest、ABI、版本号和旧命令入口均以各段原发布环境为准；
新工程按上面的 0.2 文档迁移，不使用历史包与新工具混搭。

# ESP-Mosaico Vibe Mode

**Vibe Mode** 是 ESP-Mosaico 内置的应用安装与设备维护模式，运行 ESP-Iris，
提供应用更新、Wi-Fi 配置和 Download Ideas。**ROM Download Mode（ROM 下载模式）**
是芯片底层烧录模式，不运行 Vibe Mode 或 ESP-Iris。

从可达的正常应用进入 Vibe Mode 使用 `iris test enter-recovery`，不写入固件；
`recover` 则通过 ROM 路径重建基础固件，再验证 Vibe Mode 就绪。
有效的配套 bootloader 通过 AI 键（GPIO7）选择 Vibe Mode；
Boot 键（GPIO61）用于 ROM 下载入口，仅在恢复流程要求时手动进入。

`Recovery` 是保留固件的技术名称；目录、命令、`firmware_mode=recovery` 和 ABI 保持不变。
`factory` 是 `esp-mosaico-recovery` 内置的 Vibe Mode 保留固件，
其源码和评审 bundle 与 `mosaico.py recover` 一同维护。普通应用从宿主
workspace 的 `projects/hello_world` 创建，不应将本工程作为应用安装到 `ota_0`。

当前源码构建的 Vibe Mode 固件版本为 `0.1.4`，由
`sdkconfig.recovery.defaults` 中的 `CONFIG_APP_PROJECT_VER` 定义。
`0.1.4` 保持 ESP-GSP `1.4.0` 和 GSPC `0.5.0`，源码构建自动准备
独立 GSP 编译器。应用模板的 GSP 升级不改变 Vibe Mode 的依赖版本；
已评审的预编译包版本见下文及包 manifest。
`0.1.3` 将 USB/TCP System Update 与 NAND 本地安装的组件上限从 4 提高到 8，
manifest 上限同步提高到 3 KiB；标准应用包可包含分区表、应用和六个资源镜像。
使用新容量的包需要先升级 Vibe Mode；Bridge 的独立组件上限保持不变。
`prebuilt/recovery` 基础包使用 `0.1.4`，其 manifest 记录各镜像的大小与 SHA-256。
2026-09-28 的 ESP-174 优化在 ESP32-S31 实机通过 USB OTA、System Update、
Vibe Mode 自更新及应用健康验收：OTA 按镜像大小预擦除，System Update 数据块
提高到 3968 字节；Gateway 在成功握手后重置重连退避。相同 515,312 字节应用的
OTA 传输约从 68 KiB/s 提升到 236–255 KiB/s，完整操作从 15.85 秒缩短到
11.87–12.09 秒。协议、版本线、Recovery ABI 和分区布局不变。

当前包以 `43cd13e28ea47f6bb2ae39f584a2d2ac980f45d2` 加本次修改构建，manifest
如实记录源码 dirty 状态。`factory.bin` 为 1,817,040 字节，固定槽位剩余
17,968 字节，ELF SHA-256 为
`4b9c3296d2e218977d0a88d205f69f328ebb0a5867cd3fbde0e15a5fc6afc55b`。
整包通过 `prepare_recovery.py` 重新生成、校验并原子替换；保留以下历史包的
bootloader、基础分区表和初始 OTA 数据字节，纳入实机验证过的 Vibe Mode 镜像。
测量口径、操作 ID、哈希及验证边界见
[USB 更新吞吐验证](../../docs/usb-update-throughput-validation.zh-CN.md)。

<details>
<summary>2026-09-24 基础包的历史验收记录</summary>

此前包来自源码提交 `0b0d9bc1af83f42471fd2c7e879290cb9837361f`（源码工作树干净），
基于已合入的 `0.1.3`，保持 ESP-GSP `1.4.0` 和 GSPC `0.5.0`。
以下为已评审包的历史验证记录，其中 Recovery 指 Vibe Mode：

2026-09-24 在 ESP32-S31（Base MAC `30:ed:a0:f4:60:56`）上通过应用工作区的
`mosaico.py recover --source current` 完成整包 ROM 烧录及哈希校验，重连确认
Recovery `0.1.4`、相同硬件身份、OTA/System Update 能力及匹配的 ELF SHA-256。
操作 ID 为 `75d799e2-99c0-4412-833b-759b76d1d07b`，首次 Boot ID 为
`17470610007971261482`；正式预编译包与这次实测包逐字节一致。
通过 System Update 安装 `tests/firmware/iris_acceptance` 后，验证应用健康状态、
ELF 哈希和 RPC 计数，并完成 normal → Recovery → normal 往返；每次重启
保持相同 Device ID，产生新的 Boot ID，bootloader 与分区表哈希始终一致。
最终停留在 Recovery `0.1.4`，Boot ID 为 `12340589603743011809`。
Recovery ELF SHA-256 为
`8cb8d4e40d30667884da1c892a2eff5503acabf979710a2d2dc6151f1481b5db`。
8 个组件和 3 KiB manifest 的接收边界通过宿主 C 运行时与制包回归测试；
本次真机验收使用两个组件的应用包，未执行 8 组件应用更新。
源码来源与 dirty 状态以 `prebuilt/recovery/manifest.json` 为准。
本次使用固定的 ESP-IDF `7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe` 和仓库默认配置完整构建；
构建环境保留已有 DVP camera 修改，manifest 中 IDF 描述如实记录 `-dirt`。
BSP 固定于组件 manifest 中的 `05e067de0613a0aa3fe2af42e75616804decbf4b`：
`factory.bin` 为 1,817,440 字节，固定槽位剩余 17,568 字节；INFO 日志
`bootloader.bin` 为 24,432 字节，距离分区表剩余 144 字节。
默认 `recover` 使用该包，`recover --source current` 使用当前源码重新构建。
Recovery ABI 与分区布局由工程配置和包 manifest 约束。

</details>

System Update 版本检查将 `0.1.4` 解析为同一语义版本线，只接受同一主版本线、且
最低版本要求不高于当前 Vibe Mode 的更新包。Vibe Mode 自更新拒绝降级或
跨主版本线的镜像。

## 静态开机 Logo

保留 Vibe Mode 的二级 bootloader 在选择应用分区前，通过 SPI2 QSPI 初始化
CO5300，绘制黑底橙色点阵 `mosaico`。Logo 使用紧凑的字形数据逐像素生成，
不依赖 LVGL、GSP、PSRAM 或外部 UI 资源。支持 eFuse 标识的 v1.0、v1.1 和
v1.2 板；未知板型或 SPI 传输失败时跳过 Logo，继续正常启动与 Vibe Mode。

确认硬件版本后，bootloader 会先打开 VCC_3V3 并触发约 60 ms 的 GPIO8 短震，
再继续屏幕初始化和 Logo 绘制。短震复用了原有屏幕复位等待时间，没有额外延长
启动流程；失败路径会再次确保马达关闭，未知板型不会驱动马达。

Logo 完整写入后通过 LP STORE15 发布一次性屏幕交接标记。支持该协议的 BSP
会在应用启动时消费标记，跳过重复面板 reset、Sleep Out 和 Display On 延迟，
保留 Logo 直至应用首帧覆盖。Logo 初始化或传输失败时不发布标记，BSP 自动
回到原来的完整初始化流程。

bootloader 默认启用 INFO 串口日志，输出启动信息、分区选择、Logo 成功信息及
警告/错误；使用默认 UART0、115200 波特率。此阶段 ESP-Iris 尚未启动，
`iris logs` 不会回放这些早期串口日志。

Logo 的 SPI2 总线固定为 40 MHz、半双工只写，通过 ESP-IDF LL 配置发送，
不引入通用 SPI HAL 的 RX、DMA 与动态事务配置。板型读取使用 ESP32-S31
USER_DATA 的低 16 位只读寄存器，虚拟 eFuse 构建保留原字段读取 API。
单字节屏幕初始化指令使用紧凑表；字模、像素、短震、失败回退和屏幕交接保持不变。
ESP-IDF `7b9cc1ac79f8` 实测 INFO bootloader 为 24,432 字节，比优化前同级
日志的 26,720 字节缩小 2,288 字节，在 `0x2000` 到 `0x8000` 的 24 KiB
固定空间内剩余 144 字节。后续修改仍须通过 bootloader 容量检查。

分区布局、OTA 选择、Vibe Mode 的 AI 按键（GPIO7）和恢复协议不变。维护者必须
在更新源码后重新生成并校验 `prebuilt/recovery` 的完整包，不能只替换其中一个
镜像；普通应用更新仍使用宿主 workspace 的 `mosaico.py iris app-update`。

## 用户命令

从源码构建前，在已激活的 ESP-IDF Python 环境中安装资源编码依赖：

```sh
python -m pip install -r firmware/recovery/tools/requirements.txt
```

该工具依赖 Python 3.10+，仅用于离线 Zopfli 压缩；设备继续使用 ROM 的 zlib/Deflate
解码，场景、字号和字形内容不变。CI 在构建 Python 环境中安装同一固定版本。

在仓库根目录运行：

```sh
python mosaico.py iris list
python mosaico.py recover
python mosaico.py iris app-update --project projects/<project>
python mosaico.py iris logs
```

- `iris list` 实时发现设备，并区分在线连接与离线缓存。
- `recover` 初始化或恢复设备，默认使用仓库内经过评审的基础包；实时显示基础包
  校验、设备检测、ESP-IDF 构建/烧录、镜像哈希校验、重连和 Vibe Mode 就绪验证。
- `iris app-update` 构建并通过 ESP-Iris 安装普通应用；不会自动执行 `recover`。
- `iris app-update` 默认实时显示构建、Vibe Mode 切换、传输进度、重连和固件校验阶段；
  `--json` 模式保持稳定机器输出，详细过程仍保存在运行日志中。
- `iris logs` 先显示保留日志，再持续跟随；按 `Ctrl+C` 正常结束。

Vibe Mode 屏幕在 OTA 更新期间同时显示当前阶段、已传输/总容量、百分比与速率。
Bridge 使用 `Download`，USB/TCP 使用 `Receive`，NAND 使用 `Read`；速率单位
为 KiB/s 或 MiB/s，按约一秒内写入后端接受的有效负载计算，不包含传输协议开销。
首次采样、组件切换、重试及非传输阶段显示 `--`，停滞一个完整窗口后显示零速率。

System Update 的主进度按整包字节数加权，并另列当前组件进度及已校验组件数。
总大小未知时百分比显示 `--`。传输达到 100% 后仍需完成校验与提交；普通 OTA
成功后提示重启，失败页显示错误和最后传输进度，不能把传输完成当成应用健康。

Vibe Mode 在应用 OTA 完成全部校验并成功选择启动分区后，自行安排约一秒后的
重启，给完成响应留出发送时间。该定时器在开放 USB 服务之前预先创建，不依赖
Gateway 补发重启、会话持续连接或 Iris 服务线程继续轮询；失败或取消的 OTA
不会触发它。Gateway 仍负责重连并验证新应用的身份和健康状态。

UI 初始化等待上限为五秒；USB 打开 Download Ideas 的请求（含串行排队）
最多等待两秒。超时保留 USB 维护通道，过期的待处理请求不会随后打开页面，
迟到的响应也不会被后续请求误用。Wi-Fi 初始化失败会释放本次创建的驱动、
事件处理器和网络接口，保留共享事件循环、状态锁及已保存凭据；重新扫描或
通过 USB 配网会再次尝试初始化，无需为此重启设备。

### Download Ideas 的后台配对码

本次 Vibe Mode 启动首次连接 Wi-Fi 并取得 IP 后，会后台注册一个 Bridge 会话。
首页保持可操作，后台只获取/缓存配对码，不轮询、上报 inventory 或执行下载更新。
打开 Download Ideas 后复用有效码并启用下载轮询；返回首页暂停轮询并保留会话，
再次进入继续使用同一码。明确点击 Cancel 或忘记 Wi-Fi 时会取消会话。

后台未配对的码按现有约 10 分钟期限失效后结束，不无限注册续期；之后点击下载页
重新申请。下载页仍打开时会自动续期未配对的码。注册失败最多尝试三次，保留现有
退避和服务端 Retry-After；失败后可退出页面再进入重试。自动预取每次启动只触发一次，
忘记 Wi-Fi 后重新配置可再次触发。

<a id="recovery-自更新接受-rom-兜底"></a>

### Vibe Mode 自更新（接受 ROM 兜底）

维护者可从当前 Vibe Mode 构建生成只含一个 `recovery` 组件的专用包：

```sh
idf.py -C firmware/recovery build recovery-self-update-bundle
python mosaico.py iris system-update \
  --bundle firmware/recovery/build/factory-recovery-update.irisfw
```

制包器把 `factory.bin` 以 `0xff` 补齐至整个 1.75 MiB `factory` 槽。分区表不作为
组件进入包；制包时只把它的 SHA-256 写入顶层 `target_layout_sha256`，作为明确的
设备布局前置条件。设备先确认当前分区表哈希满足该条件，再在 PSRAM 中分配连续的
完整槽位、接收并校验 SHA-256 和 ESP32-S31 镜像。只有全部预检通过后，才会临时
关闭 dangerous write protection，原地擦写 `factory`，恢复保护，再执行整槽读回
SHA-256 和 `esp_image_verify()`。成功后将下一次启动明确指向 `factory`，保存
operation receipt，并延迟重启；Gateway 必须观察到相同 Device ID、新 Boot ID、
`recovery` mode、目标 project/ELF SHA-256 和 HEALTHY 才报告成功。

该路径不改 bootloader 或 Flash 布局，也不与 normal application/data 更新混包。
它仍是单副本原地更新：从开始擦除到完成校验之间掉电，可能导致 Vibe Mode 无法启动，
此时需按仓库规定进入 ROM download mode；开发期间运行
`python mosaico.py recover --source current` 恢复本分支构建，发布后则使用已评审的
Vibe Mode 包。不具备自更新能力的设备必须走 ROM/`recover` 路径。

<a id="recovery-https-bridge"></a>

### Vibe Mode HTTPS Bridge

Vibe Mode 主动通过 HTTPS 连接 Bridge 服务，支持 `partitions`、`layout`、
`factory`，并可在构建时启用 `system_update`。本仓库的 Vibe Mode 源码构建默认连接
`https://iris-bridge.esp-claw.com`，并使用 `esp-mosaico` board ID。部署到其他
环境时，通过 `CONFIG_IRIS_FACTORY_BRIDGE_SERVER_URL`（无末尾斜杠的 HTTPS
Origin）和 `CONFIG_IRIS_FACTORY_BRIDGE_BOARD_ID` 覆盖这两个值；任一值为空都不注册。

首页主按钮为 **Download Ideas**，引导用户访问
<https://mosaico-ideas.espressif.com/> 并选择适合设备的应用。没有保存 Wi-Fi
配置时，点击主按钮先进入配网页；连接取得 IP 后自动继续下载流程。返回首页
会取消这次续接。普通 **Wi-Fi** 入口不会自动开启下载。

进入下载页面后，设备等待 Wi-Fi IP、注册并显示服务器配对码。
扫描页面二维码打开上述网站，在网页输入二维码下方的配对码。网址使用小字，
常规配对码使用 40px 大字；较长或包含其他 ASCII 字符的码使用 24px 完整显示。
网站可能要求在浏览器完成人机验证后才能配对。
网页配对并上传后，设备拉取任务、验证并写入；一次会话仅烧录一次。
退出会请求异步安全停止，完成或失败后重新进入页面才能再次配对。

Bridge 注册响应的 `control_protocol: 2` 启用独立授权：设备向 `/authorize`
提交 `PRECHECK` 获取计划及 `write_authorization_ttl_ms`，向同一接口提交
`COMMITTING` 获取明确的 `authorized: true` 后才提交。设备使用单调时钟检查
写入授权期限；普通 `/progress` 上报不授予写权限。授权响应不确定时不会自动重放。
旧服务继续使用原有 `/progress` 协议，在最终提交边界补齐必要阶段确认。

下载期间，PSRAM 栈后台任务合并组件进度，最多每秒发送一次；没有新进度时每
5 秒查询取消状态。暂时性网络/5xx 失败仅退避重试，不中断组件接收；明确取消、
401/404/410 或授权过期仍停止更新。重试从请求结束时计时并遵守 `Retry-After`。
退出或提交前会等待后台任务释放连接，避免旧请求跨越提交边界。
控制主任务、后台任务和文件接收各自拥有 HTTP 客户端；更换路径时更新 URL，完整
消费成功响应后复用 HTTPS 连接，错误、不完整响应或服务端关闭连接时丢弃句柄。

更新后端先将操作 ID 和结果持久化到 `sysmeta`，再保存待启动目标。最终结果由
只做网络操作的 PSRAM 栈任务回报，主任务最多等待 **2 秒**便重启；DNS/TLS 或
响应体阻塞不会延长等待，不强删持有 TLS 锁的任务。启动意图写入失败会更新本地
失败结果。下次 inventory 仍能提供 `last_operation_id/last_operation_state`；
云端最终响应丢失不触发自动重烧，也不等同于目标应用已经健康启动。

应用镜像按实际长度向上对齐 4 KiB 扇区后擦除，不再擦除整个应用分区；其余尾部
字节保留，镜像及读回校验不变。提供镜像的数据分区仍完整擦除，保持文件系统尾部语义。

远程布局更新允许省略数据分区镜像（包括 SPIFFS/FAT/LittleFS 等），适用于未使用
或由应用自行初始化的分区；可变应用分区仍必须提供并校验镜像。省略的数据分区
不会新增擦除或写入操作，但布局改变或其他镜像复用原地址时，旧数据不保证有效或
保留。网页上传端应列出缺失的非 NVS 数据镜像，并由作者确认后继续。
该行为需要 Vibe Mode `0.1.2` 与配套 Bridge；旧设备固件仍可能拒绝更新。
Vibe Mode 的 TLS 动态分配使用 PSRAM，证书校验保持启用。批量下载期间临时关闭
Wi-Fi 省电，退出或完成时恢复原模式；TCP 接收窗口为 65,535 字节，接收邮箱为
48 项，避免小窗口限制公网 HTTPS 吞吐。实际速率仍包含同步 Flash 写入耗时。

Vibe Mode 的普通 `malloc` 优先使用 PSRAM，并保留 64 KiB 片内分配池。Wi-Fi/lwIP
可迁移的动态缓冲及 BSS、TLS、JSON、4 KiB 下载缓冲、旧布局模式的分区表缓冲、
更新组件计划、UI/NAND 快照与 ESP-Iris 日志环均使用 PSRAM；mDNS 和只读取消查询
任务也使用 PSRAM 栈。DMA 缓冲、RTOS 控制块及 Flash/NVS 写入任务栈仍留在片内，
不启用全局任务栈外移。Bridge 写入任务保留 20 KiB 片内栈：实测 16 KiB 栈仅剩
868 字节余量，因此增加 4 KiB 给 TLS 和异常路径。

日志记录各组件实际接收字节数、总耗时、HTTP 读取与后端写入累计耗时、最长单次
读取耗时及写入任务栈水位；总耗时还包括打开连接、擦除和校验，不能当作纯网络
速率。任务内存观察接口保持启用，可结合日志区分网络等待、Flash 写入和内存压力。

Vibe Mode 支持 Gateway Web 工作台截图，以及 USB 会话下的交互输入。
触摸输入使用 Gateway 的 `0x1001/1` pointer RPC，交由 GSP 输入处理按下、移动和抬起；TCP 会话不能通过此接口操作 Vibe Mode 界面。

```sh
python mosaico.py iris test recovery-wifi --ssid SSID
python mosaico.py iris test bridge-code --device-id DEVICE_ID --timeout 60
```

USB 命令打开同一个页面并等待配对码；重复打开不换码。控制服务 `0x1202` 的
方法 4 打开，方法 5 只查询状态，旧方法 3 已移除。服务地址、板型缺失时提示
重新配置构建。两个方法均拒绝 TCP，不返回或记录设备 token。

设备端实现位于 [iris_bridge 组件](components/iris_bridge/)。
旧本地 HTTP server、URL 下载 RPC、`http-update-code` 和 `--manifest-url`
已移除，USB bundle 和 NAND 更新继续使用共享写入后端。

<a id="recovery-从-nand-littlefs-读取系统更新"></a>

### Vibe Mode 从 NAND LittleFS 读取系统更新

ESP-Mosaico 的板载 NAND 与 `esp-mosaico-claw` 一致，使用 SPI NAND、wear-leveling
block device 和 LittleFS，挂载点为 `/nand`。Vibe Mode 读写挂载已有文件系统，并将
整个挂载点注册为 ESP-Iris 文件卷 `nand`；可通过 Gateway 列目录、读取、写入、删除、
建目录和重命名。写入先落到同目录临时文件，校验 SHA-256 后再原子替换目标文件。
挂载失败时不会格式化 NAND，也不会阻止 Vibe Mode USB 维护服务启动。将解包后的
bundle 放到同一目录，例如：

```text
/nand/system-update/manifest.json
/nand/system-update/ota_0.bin
/nand/system-update/bootloader.bin
/nand/system-update/partition-table.bin
```

Vibe Mode 首页底部提供 **NAND update**：进入后固件会异步扫描以下两种
catalog 布局，最多列出 8 个完整 bundle；点击条目可先核对 release、组件数、总
容量和 manifest 路径，再确认更新。

```text
/nand/system-update/manifest.json
/nand/system-update/<release>/manifest.json
```

每个 `manifest.json` 引用的组件必须与它位于同一目录。扫描阶段会过滤 manifest
格式错误、组件缺失或文件大小不符的条目；确认安装后，System Update backend
仍会重新执行完整 manifest、SHA-256、镜像和布局校验。也可以通过产品 CLI 直接
启动指定路径：

```sh
python mosaico.py iris system-update --device-id DEVICE_ID \
  --manifest-path /nand/system-update/manifest.json
```

Vibe Mode 逐块读取组件并复用与 USB、Bridge 相同的 manifest、SHA-256、镜像及
分区布局校验。v1 manifest 必须将 partition table 放在首个组件；Vibe Mode 验证
当前表和目标表中的五个不可变分区后，按照目标表流式写入 application 和 data。
bootloader 暂存到 PSRAM，全部验证完成后统一提交。三种来源共用一个 Flash writer owner，不能
并行执行。可通过 `CONFIG_IRIS_FACTORY_NAND_SYSTEM_UPDATE_AUTO_START=y` 配置固定
路径自动启动；默认关闭，避免误用 NAND 中遗留的旧 bundle。启动 NAND 更新后不要
再通过文件服务修改该 bundle；单文件上传是原子的，但一个 bundle 的多个文件不构成
同一事务。

## Flash 布局

16 MiB Flash 使用固定系统前缀和尾部可缩减的单应用槽：

| 分区 | Offset | Size | 用途 |
| --- | ---: | ---: | --- |
| `otadata` | `0x9000` | 8 KiB | ESP-IDF OTA 选择与回滚状态 |
| `phy_init` | `0xb000` | 4 KiB | PHY 初始化数据 |
| `sysmeta` | `0xc000` | 80 KiB | 系统专用 NVS |
| `factory` | `0x20000` | 1.75 MiB | 保留 Vibe Mode |
| `coredump` | `0x1e0000` | 128 KiB | 崩溃证据预留空间 |
| `nvs` | `0x200000` | 64 KiB | 应用 NVS |
| `ota_0` | `0x210000` | 13.94 MiB | 普通应用；后续布局可从尾部缩减 |

固定系统前缀从 Flash 起始到 `coredump` 结束恰好为 2 MiB，
`nvs` 和普通应用从 `0x200000` 之后开始。

`sysmeta` 中的 `esp_iris`、`wifi`、`iris_ota_demo` 和 `update` namespace
分别保存 TCP pairing token、Factory Wi-Fi、Vibe Mode OTA 状态和
最后一次系统更新结果。System Update v1 只要求 `otadata`、`phy_init`、
`sysmeta`、`factory` 和 `coredump` 的名称、类型、子类型、offset、size 与 flags
严格符合上表；`nvs`、`ota_0` 以及其他应用数据分区可由目标表调整。

常用选项可通过 `python mosaico.py <command> --help` 查看。自动化环境可加
`--json`；`recover` 在唯一识别到受支持设备后直接执行，无需二次确认。

## 工程维护者

普通用户不应直接调用底层构建或写入命令。Vibe Mode 基础包和内部写入 target
由 `mosaico.py recover` 管理；普通应用始终由 `mosaico.py iris app-update` 通过
ESP-Iris 安装。评审包包含完整哈希与布局约束，只有通过构建校验和真机验收后
才应发布。

Vibe Mode 工程直接使用 workspace 中的 `ESP-Iris`。BSP 依赖由组件
manifest 指向 `https://github.com/esp-mosaico/esp-mosaico-bsp`；集成到
ESP-Mosaico workspace 时，`mosaico.py recover` 会根据宿主 workspace 的
`.mosaico.json` 注入本地 `esp-mosaico-bsp`，并复用 workspace 中的 ESP-Iris，
保证 Vibe Mode、设备工具与普通应用使用同一 Iris 版本。


## Vibe Mode 界面与构建

用户可见名称为 **Vibe Mode**；工程 `factory`、协议角色 `recovery`、
`recover` 命令和 Recovery ABI 1 保持不变；当前源码固件版本为 `0.1.4`。
九个页面由 `ui/main.json` 和可移植的 `main/vibe_ui.c` 实现，PC 与设备共享
同一控制器及 `ui/profile.yaml` RGB565 编译配置。
`main/factory_ui.c` 适配网络、Bridge、NAND 和更新状态；
异步状态在 GSP 渲染上下文汇总，外部打开下载页的请求通过队列提交。

使用 Component Registry 的 **ESP-GSP 1.4.0**，BSP 开启硬件显示但关闭
`CONFIG_BSP_DISPLAY_LVGL_ENABLE`。该 BSP 选项默认开启，保留现有 LVGL
应用行为。构建必须使用包含该选项的 workspace BSP；旧版 BSP 不支持此模式。
GSP 在调用方同步创建、校验 UI，Vibe Mode 将
`CONFIG_ESP_MAIN_TASK_STACK_SIZE` 设为 20480；仅增加渲染任务栈不能覆盖该阶段。
启动日志记录 UI 初始化后的主任务栈余量，便于检查后续资源改动。

场景、字形与键盘图标全部嵌入 Vibe Mode，不依赖应用资源分区。
GSPB 在构建时无损 Deflate 压缩，启动时使用 ROM 解压器还原到 PSRAM；
GSP 继续执行原始 bundle 校验。界面仅使用预烘焙字形，不启用运行时字体；
通过 `CONFIG_ESP_GSP_ENABLE_JPEG=n` 排除 JPEG 解码器，
现有图标仍为编译后的 RGB565_A8。32 px 更新标题与百分比只打包对应状态文案
和数字所需的字形，网络名称与密码仍保留完整的既有字符集。
若增加运行时字体或 JPEG 图片，必须同步启用相应能力并重新验证大小和功能。
原生键盘支持大小写、符号、删除、确认、密码显示切换和 64 字节上限，
离开输入页会清理密码。

在宿主 workspace 中启动共享控制器的模拟器：

```sh
python3 tools/gsp-sim/run.py submodule/esp-mosaico-utils/esp-mosaico-recovery/firmware/recovery/ui/main.json --headless
```

运行 Vibe Mode 主机测试前，设置 `GSPC_EXECUTABLE`（0.5.0）和
`GSP_SIM_EXECUTABLE`（1.4.0），安装 `tests/requirements-ui.txt` 中的主机依赖。
原生模拟器测试使用真实键盘、列表和回调，
并检查 Wi-Fi 列表的截图像素，覆盖字形完整性、卡片间距和反复导航。
二维码由独立解码器直接从渲染截图验证；OTA 使用确定性的时间与字节数序列，
覆盖组件切换、停滞、重试、新任务、未知大小及传输完成后的提交失败。
服务数据由 PC 后端模拟；模拟通过不代表真机网络或更新验收通过。

网站二维码是提交到 `ui/assets/ideas-qr.png` 的静态资源，无设备端编码库。
需要重新生成时，在主机安装 `tools/requirements-qr.txt`，运行
`python tools/generate_ideas_qr.py`。脚本固定完整 HTTPS URL、QR version 3、
M 级纠错、四模块白边和 5 倍整数缩放，生成 185×185 PNG；普通构建直接使用
该文件，经现有 GSPB 与 Zopfli 压缩链路嵌入固件。

固定 Vibe Mode 槽仍为 `0x20000` / `0x1c0000`。须核对实际构建目录中的
`factory.bin` 大小；不能仅凭 IDF 构建成功判断镜像适合较小的 factory 槽。
预编译包只在真机验收后通过 `update-recovery-prebuilt` 整体更新。
