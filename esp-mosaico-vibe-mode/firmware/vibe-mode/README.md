# Vibe Mode 0.2

本工程支持通过高速 USB console 执行 `idf.py -p PORT flash monitor`。
`CONFIG_ESP_IRIS_USB_AUTO_DOWNLOAD=y` 在 CDC0 识别下载握手；CDC1 保持 Iris data
用途。VID/PID 保持 `303A:4002`。项目 `idf_ext.py` 显式加载 Iris 扩展，构建完成后
跟踪同一 USB 位置、核对 ROM USB 序列号中的 MAC，调用原有 flash 目标，再等待同一设备的 console
并以 `monitor --no-reset` 附着。原有 `flash`/`app-flash` 写入范围和参数保持不变。

升级已有构建目录时先执行 `idf.py reconfigure build`。设备上旧固件没有下载握手时，
首次仍需手动进入 Boot/ROM 或通过消费工作区 `mosaico.py recover --source current`
安装；`prebuilt/recovery` 不随源码修改更新。之后选择 **console** 端口运行组合命令。
多板连接必须用 `-p` 指定设备；关闭占用该板 console/data 的 Gateway 或 monitor。
扩展持有两路接口的主机锁，避免烧录和 monitor 交接期间被 Gateway 重连占用。
连续烧录请先退出 monitor，再运行组合命令；monitor 内置重烧录快捷键的父子进程
锁交接尚未适配。
ROM 身份不符、端口不唯一或重枚举超时会停止，不尝试其它板子。
身份校验只读取 USB 描述符，不提前打开 ROM 串口：在 S31 上单独执行
`esptool read-mac` 后关闭串口可能使设备退出 ROM，即使指定 `--after=no-reset`，
随后原生 flash 就会报端口不存在。
2026-10-15 已在 S31 高速 USB 实机完成全量烧录、连续烧录与 monitor 状态检查，
详见 [USB 烧录验证记录](../../docs/validation-0.2.md#原生高速-usb-烧录修复2026-10-15)。

当前源码使用 ESP-Iris 0.2、Recovery ABI 2 和 `mosaico-retained-test-2m-v2`
布局。`vibe_mode` 为 `app/test`，`main_app` 为 `app/ota_0`。固件工程名为
`vibe-mode`，新构建产物为 `vibe-mode.bin`、`vibe-mode.elf`，候选包的
manifest 使用 `project: vibe-mode`。`prebuilt/recovery` 已于 2026-10-16 更新为
源码提交 `ca0ecf7` 的 `vibe-mode.bin`，版本保持 0.2.0，镜像大小为 1,488,608 字节。
默认 Ideas 地址为 `https://mosaico.ai`，包含固件清单尾部 JSON
空白解析修复及“打开失败”提示。主程序与 ESP32-S31 实机自更新、重启后 ELF 哈希
核对及用户手动验收通过的镜像字节一致；bootloader、分区表及初始 OTA 数据保持原评审包
字节不变。完整包通过 manifest、布局、大小和 SHA-256 校验，S31 产品统一使用 QIO。

目录改名后须使用新的构建目录重新配置，避免复用包含旧绝对路径的 CMake 缓存。
`recover` launcher 已使用新的 `build-mosaico-vibe-mode` 构建目录。
`recover` 命令、Recovery 构建目标、协议字段、ABI 和分区布局保持兼容。
`recovery-current-bundle` 使用新产物名称；`recovery-image` 按评审包 manifest
读取清单中的产物名称。改名不会自动重建或发布评审包。

POSIX 平台的 `mosaico.py recover` 通过同一原生扩展执行 ROM 身份校验，
Gateway 交出该设备的控制和数据接口；IDF 子进程校验并复用 host worker
继承的设备锁。Windows 保留原有恢复目标，本轮未验证 Windows 烧录。

- [启动契约与迁移](../../docs/migration-0.2.md)：NVS 一次性维护意图、基础安装
  otadata 标记、保护范围与从第三方固件恢复。
- [应用集成](../../../mosaico-tools/docs/application-integration.md)：原生
  `idf.py flash monitor` 与默认 Mosaico 更新流程。
- [软件规格](../../docs/esp-mosaico-vibe-mode-software-spec-zh.md)：完整功能和验收边界。
- [EMVM-73078 使用、管理与开发流程](../../docs/vibe-mode-workflows.md)：本轮实现、
  本地保存、共享信息 API、恢复出厂范围及验证记录。

保留 Wi-Fi、Download Ideas、HTTPS Bridge 维护后端、NAND、文件、截图、输入及维护自更新。
高速 USB 使用 console/data 双 CDC；TCP 使用独立端口；控制链支持按需截图。
历史 Bridge 维护后端必须使用 control protocol 2 的独立写入授权，产品 profile 变更为
`iris-s31-test-layout-v2`；需要配套远程服务，不回退到旧授权路径。
Download Ideas 页面使用下文的六位编号直接下载契约。
System Update 清单使用 `esp-iris-system-update/0.2`，基础包 manifest 使用 schema 3。

### 启动与后台存储（EMVM-75843）

启动先注册 NAND 卷描述及维护服务，再启动 USB/Iris；此路径不访问 NAND。
LCD 在临时任务中初始化，主任务利用面板等待时间初始化 Wi-Fi、解压 UI 资源；
等待显示任务成功后才挂接截图、启动触摸和渲染。显示失败不发布 HEALTHY，
USB 维护仍可用。Wi-Fi 和 UI 初始化后，既有 Ideas worker 后台挂载 NAND 并读取 NOR 应用描述，不扫描
本地固件库。挂载前文件请求返回 BUSY，挂载失败返回 IO；成功后同一 Iris 连接
即可使用文件服务，无须重新枚举。挂载失败可从 My Ideas 页面重试。

进入 My Ideas 或应用网格时才扫描本地包、处理未完成事务并加载列表资源。
云端查询不等待整个库扫描。容量统计不在挂载日志或列表扫描中执行，进入
My Ideas/About 时按需统计并缓存；列表刷新使缓存失效，统计期间显示扫描中或未知容量。
安装、下载空间检查和中断事务的完整校验仍保留。Wi-Fi 开机直接连接已保存网络，
热点列表扫描由配网页面发起。

诊断时启用 INFO 日志，`boot_perf` 的 `iris_ready_us`、`ui_started_us`、
`network_started_us` 为自 `app_main` 入口起的耗时；`nand_driver_us` 和
`nand_mount_us` 分别记录驱动初始化、LittleFS 挂载耗时。它们不等同于主机观察的
USB 枚举时间或 LCD 完成显示时间。`HEALTHY` 的自更新验收语义保持不变。
`startup_summary` 在 UI 启动后重复早期里程碑，便于主机枚举后收集。
`display_init_us`、`ui_assets_us` 是单项耗时；`keys_ready_at_us`、
`brightness_ready_at_us`、`first_frame_submitted_at_us`、`peripherals_ready_at_us`
使用 `esp_timer_get_time()` 的时钟基准，不应直接与 IDF 日志括号中的时间戳混算。
首帧标记仅表示提交成功，不表示 LCD 已完成物理扫描。外设标记表示可选初始化结束，
各外设是否成功仍以设备 snapshot 的 availability/valid 字段为准。
本次不替换评审预构建包。

2026-10-16 在 ESP32-S31 实机、保留现有 NAND（6 个 Ideas 目录）条件下，通过
消费工作区 `mosaico.py iris system-update` 安装 INFO 诊断构建，固件哈希和
重启后的 HEALTHY 验证通过。连续 3 次从应用切入 Vibe Mode，自主机发出切换请求
至 USB 节点出现为 2.299–2.310 秒，至 Iris 重连为 2.606–2.612 秒。
设备端 UI 初始化耗时约 1.806 秒（自 `app_main` 起），随后 NAND 驱动初始化
16–20 毫秒、挂载 122–129 毫秒。管理页面按需容量统计实测 6.84 秒，已从
启动路径移除。以上为软件重启结果，未进行同条件旧固件 A/B 或断电冷启动对比，
不能据此计算整体启动提速比例。USB 节点、Iris 重连和 UI 初始化使用不同测量边界。
另一次启动确认 Iris 重连后的早期目录请求返回 `file_busy`；挂载后在同一 Boot ID
下成功列出 NAND 根目录。管理页面容量显示、已保存 Wi-Fi 重连和云端版本查询通过。

第二轮将实体按键、USB 状态及亮度控制与电池/IMU 初始化解耦；后台任务分阶段
发布外设可用状态，初始化失败或无法创建后台任务时控制仍可用。BSP 的电池 seal
等待保留在后台，不在产品层修改电池配置校验。显示仍使用锁定 BSP 的底层
`bsp_display_new()`；禁用的是 BSP LVGL 路径，并非移除了 BSP。此轮保留面板命令后
两次 600 ms 等待，通过并行初始化利用等待时间，UI 初始化约 1.74–1.79 秒。

随后按 EMVM-75843 实机调优要求，将 Sleep Out (`0x11`) 和 Display On (`0x29`)
后等待各改为 **100 ms**。`cmake/bsp_display_timing.cmake` 从锁定 BSP 生成仅替换
这两处等待的显示源文件，并使 BSP target 编译此副本；不修改下载的 managed component。
原始命令或编译源路径不匹配时配置失败，升级 BSP 时必须重新审查此覆盖。
原有硬件复位等待保持不变，评审预构建包不随诊断构建更新。

100 ms 诊断构建实机自更新通过哈希和 HEALTHY 验收，随后连续 3 次从应用切入
Vibe Mode 成功。捕获的 UI 初始化耗时为 0.802–0.824 秒（自 `app_main` 起），
对比第二轮 600 ms 构建的 1.741–1.791 秒。主机从发出切换请求到截图返回为
3.399–3.424 秒（包含协议及图像传输）；USB 节点出现仍约 2.30 秒。
完整页面重绘后截图正常，用户同时确认实体屏幕显示正常。此次验证覆盖软件重启，
尚未覆盖断电冷启动或其他面板批次。设备保留 100 ms 诊断构建，固件 ELF SHA-256
为 `c481510c51401f44dd602dc71e67ed4736059a546f7b2502bfe069f82aecc592`。

### Ideas 下载与开机应用（当前源码）

Download Ideas 输入六位应用编号（字符串，保留前导零），点击 **View app** 查询当前设备的最新兼容发布。
先展示应用名称、图标、描述、版本与包大小；再次点击 **Download** 才保存到 NAND。
不注册配对会话，也不后台预取配对码。编号必须由云端永久、唯一地映射到 application ID。
这部分源码使用下面的新云端契约；现有 Bridge 服务是否实现该契约仍需联调确认。

- **下载**：将固件、清单及展示资源写入 `/nand/ideas/<code>/current/`，不写应用 NOR、
  不切换启动目标、不重启。每个编号只保留一个完整版本，最多 16 个 Ideas。
- **检查更新**：以编号和设备能力获取最新兼容发布，再与本地 application ID、release ID 比较。已是最新版本时直接提示；
  有更新时进入同一确认页。“Download latest” 也先展示信息，确认后替换 NAND 当前版本，
  保持 NOR 中原应用及其开机标记。
- **打开**：完全离线，核对清单后从 NAND 单次读取并写入 NOR、验证实际读回内容、
  提交分区表和启动目标，随后自动重启。相同版本也核对 NOR 中的应用、布局和资源，
  完全一致才跳过写入。未保存开发内容时只提供“放弃并打开 / 取消”。
- **管理**：本地列表加载且联网后检查更新，支持逐个确认更新和后台批量更新；更新只替换 NAND 副本，
  下次点击“打开”才写入运行槽。删除须长按确认，四秒内可以撤销；当前 Idea 不允许删除。
- 开机应用身份保存在保留的 `sysmeta` 分区独立 `ideas` 命名空间；显式验证时核对
  实际 NOR 应用与分区表的 SHA-256。它不依赖 NAND 中当前版本是否等于 NOR 版本。
- 新包先在 `.incoming` 完整下载并校验，再通过目录重命名替换 `current`；`.previous`
  只用于中断恢复，并在替换完成或下次扫描时删除，不提供历史版本列表。

开机和列表刷新只读取应用描述以显示开发应用名称、版本，不再全量读取或计算 NOR 主程序、资源分区的哈希。保存状态在没有校验依据时按未确认处理，保留覆盖前的提醒。完整校验仅在用户主动保存、运行或打开应用时执行。

预览阶段一次请求取得根清单、固件清单、展示 JSON、字形子集和图标，经大小及 SHA-256 校验后暂存在内存，不创建 `.incoming`，
不下载应用镜像或分区表。解析出的 release ID、清单和文件路径在确认期间固定；
确认不会再次查询最新发布，已缓存展示资源直接复用。取消清理预览，迟到响应不能启动下载。
等待确认期间不占用 NAND 租约或下载任务；重复确认及失效请求编号会被拒绝。
若本地同 release ID 的全部文件校验通过，确认页显示已下载，按钮改为 **My Ideas**。

安装仍遵循单应用槽的断电边界：不是 A/B 回滚。擦写前设置 Vibe Mode 启动意图，
成功后清除；失败可返回 Vibe Mode 重试。Ideas 不允许更新 bootloader 或保留的
Vibe Mode 镜像。分区表改变后，普通 USB/TCP OTA 写入须等重启重新加载分区描述；
Ideas 可继续使用经过验证的新分区表描述重新安装。

正常安装不再预先遍历 NAND 固件计算 SHA-256，也不在写入流中重复计算；应用和资源
以 NOR 回读 SHA-256 对照清单，并验证应用镜像后才能提交启动。小型分区表在使用前
仍检查暂存内容的哈希和布局，提交时核对 NOR 实际内容。未完成的下载事务仍走完整
恢复检查。大于 4 KiB 的 Ideas 组件使用两个 4 KiB 内部缓冲，让下一块 NAND 读取与
当前块 NOR 写入重叠；额外缓冲或读取任务分配失败时退回串行。擦除、最终校验和提交
仍按顺序执行。界面只展示安装写入进度，写入到 100% 后仍须等待校验完成。

#### 云端契约 `mosaico-ideas/device-release/1`

完整字段与流程以[文件发布协议](../../../mosaico-tools/docs/ideas-file-release/README.zh-CN.md)
和[单响应预览格式](../../../mosaico-tools/docs/ideas-file-release/preview-response.zh-CN.md)为准。
不支持旧 download/1 或 V0.1，不调用 POST resolve。

设备使用 CONFIG_IRIS_FACTORY_IDEAS_SERVER_URL 配置的 HTTPS origin，默认值为
`https://mosaico.ai`（不带末尾 `/`），独立于 Bridge 地址。验证 TLS，
直接 GET `/api/v1/ideas/{code}/manifest.json`，携带 profile、chip_id、flash_size、
recovery_version、当前系统 locale，以及 `Accept: multipart/related`。服务器一次
返回最新兼容发布的根清单、固件清单及当前语言 display、字体、图标的原始字节。
预览阶段没有额外文件 GET。服务器仍提供原始根 JSON 表示供诊断和固定发布读取。

设备有界流式解析，HTTP ETag 校验整个 multipart 响应；单独计算根清单原始字节
摘要，并按引用校验每个部分的长度与 SHA-256。根清单最多 64 KiB，固件清单
3072 字节，当前语言资源合计 1 MiB，整个响应最多 1121280 字节。缺失、重复、
错误路径/语言/长度、截断或摘要不符直接拒绝，不追加请求修补。

确认后复用预览缓存，仅通过 `/api/v1/ideas/files/{sha256}` 获取固件组件和另一
语言资源。根清单落盘保持原始 JSON，固件位于 firmware/，展示位于 device/zh-CN/
与 device/en/。完整集合最多 32 MiB/32 文件，全部通过才提交 current。

#### 固定文字、图标及字体子集

各语言 display.json 使用 `mosaico-ideas/display/1`（最多 32 KiB），直接引用最多
两个 MIFF 字形文件和可选图标。图标固定为 64×64、8-bit RGBA、非交错静态 PNG，
最多 64 KiB。每语言展示集合最多 1 MiB，双语合计最多 2 MiB。资源须在根清单
该语言的 presentations 声明中唯一出现，按目录解析 basename，并通过内容校验。
名称、描述、语言和图标引用只由 display 定义，没有外层重复文案。
详见[自定义展示资源格式](../../docs/ideas-display-format.md)。

发布端进行排版、换行和字形子集化，保存字形 ID、灰度位图及每个字形的位置，
使阿拉伯文等语言的组合字形和书写方向得以保留。设备只解码所需字形；
`ideas_font.c` 和 `ideas_presentation.c` 不依赖 GSP 或 ESP-IDF。
`vibe_ui.c` 是设备和 PC 共用的控制器；项目内的轻量绘制器显示字形像素与 PNG 图标。
页面位置、颜色、按钮和点击行为属于固件，未来换 UI 框架可复用相同下载资源。

预览阶段获取并校验展示 JSON、字体和图标，成功后才启用 Download；确认后将
这些已校验的缓存资源与固件一并保存到 NAND，不重复解析版本或下载资源。
离线 View info 使用同一解析器，只提供返回操作，关闭时释放预览资源。
云端 Idea 的列表和详情标题复用已下载的字形像素，中文名称无需将整套字体编入固件。
本地开发副本使用内置字体；完整云端名称和描述可在信息页查看。

展示资源生成实现位于仓库的 `shared/ideas/`，本目录的脚本只处理参数和输出文件。
发布端安装 `tools/requirements-presentation.txt` 及系统 libraqm >= 0.9，运行：

```sh
python tools/prepare_idea_presentation.py \
  --code 042731 --name '天气站' --description '今日天气与预报' \
  --locale zh-CN --font /path/to/NotoSansCJK-Regular.ttc \
  --icon /path/to/icon.png --output /path/to/published-assets
```

输出 `display.json`、`title.mif`、`body.mif`、64×64 `icon.png` 和
`presentation.json`（供云端构造下载清单的元数据；不需要传给设备）。不再需要
`--gspc`。云端把前四个文件加入 `/1` 清单，使用新 release ID 发布。
旧 GSPB 清单及临时 `/2` 开发包不属于当前契约；已有这类 NAND 测试包需要输入
六位编号重新下载，完整下载后按现有事务替换。不会自动迁移或清空测试数据。
不要直接修改已有不可变 release 的内容。

2026-10-13 自定义资源初版（清单标识统一前）完成 ESP32-S31 真机验收：中文与阿拉伯文字形显示、
预览及取消不写 NAND、损坏字体在确认前拒绝、确认后复用全部展示资源、四个资源
落盘摘要一致、重启后在资源服务不可用时离线查看且不产生 HTTP 请求。
下载前后普通开机应用的固件摘要相同，bootloader 和分区表摘要也相同。
候选 Vibe 固件为 1,799,552 字节，小于现有 1,835,008 字节分区，设备崩溃计数为 0。
主机相关回归 560 项、原生界面回归 15 项通过；未纳入已有的
`test_project_workflow.py`（导入不存在的 `mosaico_cli.scaffold.create_project`）。未替换发布预构建包。

同日将开发契约统一回 `download/1` 并移除兼容分支后，561 项主机回归和
ESP32-S31 构建通过（仍排除上述已有导入失败测试）；候选固件为 1,799,520 字节。
此次清单契约调整未重新刷写测试设备。

以下为 2026-10-12 旧 GSPB 流程的历史验收记录：

2026-10-12 使用本地 HTTPS 解析/文件服务及真实 ESP32-S31、NAND/NOR 完成端到端
验证：六位编号直接下载、两个 Ideas 共存、中文名称/描述及字形子集显示、独立列表
图标、按 application/release ID 检查更新、损坏文件拒绝且保留旧版、单版本替换。
关闭服务后连续安装 `000001 -> 000002 -> 000001`，安装期间保持 Vibe Mode；
下载 `1.0.1` 后重启仍运行 NOR 中的 `1.0.0`，离线安装后才启动 `1.0.1`，
通过应用身份、固件摘要和健康 RPC 验证。所有设备操作均经过消费 workspace 的
`mosaico.py`；本地测试 URL 和 CA 只用于候选构建，没有修改生产默认值。

同日对两阶段下载补充真机验收：中文确认页预取卡片但不创建 NAND 目录；
取消不保存，输入编号保留；预览 `1.0.0` 后将云端切至 `1.0.1`，连续确认仍只下载一次
`1.0.0`，不重复解析或获取卡片。新版检查、卡片损坏拒绝、同版本免下载、单版本替换
及新旧卡片离线查看均覆盖。取消发生在本地文件校验期间时，结束后再次检查请求编号，
避免继续获取卡片；对应[原生界面回归](../../tests/test_recovery_ui_host.py)覆盖取消、重复确认和已下载分支。

此前真机验证修正了旧 GSP 1.4 的 S31 PNG 解码标志和列表图标槽问题。
当前轻量 UI 使用独立的有界 PNG 解码器，见[图片回归](../../tests/test_ideas_presentation_host.py)；
旧 GSP 真机记录不能代替本轮轻量 UI 的设备验收。
新展示资源使用 Vibe 原生页面，实体触摸和 USB 输入直接交由同一个界面处理。
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

构建使用锁定 ESP-IDF 和 Vibe 自有轻量 UI，不再需要 GSP/GSPC。`recovery-current-bundle`
仅生成未评审候选；完成设备身份、各启动入口、更新、回退及原生工具实测前，
不得运行 `update-recovery-prebuilt` 发布候选。产品原生应用 flash 不写维护镜像；
本维护工程自身的 flash 清单明确将 `vibe-mode.bin` 写入 `vibe_mode`。

## 固件与恢复入口

Vibe Mode 是保留维护固件；ROM Download Mode 是芯片底层烧录模式，不运行 ESP-Iris。
从正常应用进入 Vibe Mode 使用 `iris test enter-recovery`；`recover` 通过 ROM 路径重建基础固件。
AI 键（GPIO7）选择 Vibe Mode，Boot 键（GPIO61）用于 ROM 下载入口。
固件版本以 `sdkconfig.recovery.defaults` 为准，预编译包身份以 `prebuilt/recovery/manifest.json` 为准。

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
python -m pip install -r firmware/vibe-mode/tools/requirements.txt
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
- Vibe Mode 默认启用 `CONFIG_ESP_IRIS_REDIRECT_STDIO`，ESP-Iris 启动后通过 Iris 输出应用日志。
  `iris logs` 先显示保留日志，再持续跟随；按 `Ctrl+C` 正常结束。
  如需原生 UART0 日志（115200 波特率），可关闭该选项；USB/TCP 调试协议仍可使用。

Vibe Mode 屏幕在 OTA 更新期间同时显示当前阶段、已传输/总容量、百分比与速率。
Bridge 使用 `Download`，USB/TCP 使用 `Receive`，NAND 使用 `Write`；速率单位
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

### 联网检查 Vibe Mode 更新

Wi-Fi 获取 IP 后，后台读取独立清单
`https://mosaico-vibe-rec-ver.esp-claw.com/esp-mosaico-vibe-mode-stable.json`。
通过 `CONFIG_IRIS_FACTORY_UPDATE_MANIFEST_URL` 可更换地址，空值关闭检查；
`CONFIG_IRIS_FACTORY_UPDATE_PRODUCT` 默认是 `esp-mosaico-vibe-mode`。
它沿用 Settings 的 `schema_version: 1` JSON 格式，不使用普通应用的版本源。
正式发布需使用 `tools/prepare_online_release.py` 从已验证 bin 生成包含文件大小和哈希的清单。

发现更高版本后，在首页显示一次独立的 **Update Vibe Mode / 更新 Vibe Mode** 提示页及版本号；处于配网、
下载或安装页面时延后提示，同一版本每次启动只提示一次。用户可选择“开始更新”或“跳过”。
开始更新会从 `https://dl.espressif.com/mosaico/` 取得 `firmware.file`，校验大小、SHA-256、项目、版本及镜像后，
通过共享后端原地更新 Vibe Mode 并重启。确认页和更新页持续提示“请勿断电或关机”。
旧清单没有 `firmware` 元数据时仅提示版本，不允许开始安装。
完整命名规则、清单生成和发布验收流程见 [在线固件更新](../../docs/online-firmware-update.zh-CN.md)。

请求启用 HTTPS 证书校验，超时 10 秒，响应最多 4 KiB，不跟随重定向。
失败仅记录日志，不弹出错误；网络重连可触发新检查，请求间隔至少 60 秒，
已有请求执行期间不重复启动。尚未上传清单时的 HTTP 404 不影响正常使用。

### Vibe Mode 自更新（接受 ROM 兜底）

维护者可从当前 Vibe Mode 构建生成只含一个 `recovery` 组件的专用包：

```sh
idf.py -C firmware/vibe-mode build recovery-self-update-bundle
python mosaico.py iris system-update \
  --bundle firmware/vibe-mode/build/vibe-mode-recovery-update.irisfw
```

当前制包器把 `vibe-mode.bin` 以 `0xff` 补齐至整个 1.75 MiB `vibe_mode` 槽。分区表不作为
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
<https://mosaico.ai/> 并选择适合设备的应用。没有保存 Wi-Fi
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
触摸输入使用 Gateway 的 `0x1001/1` pointer RPC，交由 Vibe 轻量 UI 处理按下、移动和抬起；TCP 会话不能通过此接口操作 Vibe Mode 界面。

```sh
python mosaico.py iris test recovery-wifi --ssid SSID
python mosaico.py iris test bridge-code --device-id DEVICE_ID --timeout 60
```

USB 命令打开同一个页面并等待配对码；重复打开不换码。控制服务 `0x1202` 的
方法 4 打开，方法 5 只查询状态，旧方法 3 已移除。服务地址、板型缺失时提示
重新配置构建。两个方法均拒绝 TCP，不返回或记录设备 token。

设备端实现位于 [app_bridge 组件](components/app_bridge/README.md)。
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


## 运行时组件架构

`main` 只编译入口，按“硬件静音与 sysmeta → provider/文件卷注册 → Iris USB →
Ideas/UI/输入 → 自动更新及网络 → HEALTHY”的顺序组装组件。UI/输入启动失败不发布
健康事件，已启动的 USB 保留；网络和 NAND 媒体故障不关闭 USB。bootloader 是独立构建边界。

| 组件 | 职责 | 应用组件依赖 |
| --- | --- | --- |
| [app_system](components/app_system/README.md) | 启动安全与保留元数据 | 无 |
| [app_storage](components/app_storage/README.md) | NAND 文件系统与访问控制 | 无 |
| [app_http_client](components/app_http_client/README.md) | 业务 HTTP 公共部分 | 无 |
| [app_network](components/app_network/README.md) | Wi-Fi 与发现 | app_system、app_update_check |
| [app_system_update](components/app_system_update/README.md) | 系统更新策略与事务 | app_system |
| [app_nand_update](components/app_nand_update/README.md) | 离线系统更新来源 | app_storage、app_system_update、app_system |
| [app_ideas](components/app_ideas/README.md) | Ideas 库与展示资源 | app_network、app_storage、app_nand_update、app_system |
| [app_bridge](components/app_bridge/README.md) | HTTPS 维护 Bridge | app_network、app_system_update、app_system |
| [app_update_check](components/app_update_check/README.md) | 在线版本检查与固件安装 | app_system_update |
| [app_ui](components/app_ui/README.md) | 显示、输入与可移植控制器 | app_network、app_ideas、app_nand_update、app_system_update、app_bridge |
| [app_iris](components/app_iris/README.md) | Iris 产品服务适配 | app_system、app_storage、app_network、app_system_update、app_nand_update、app_bridge |

依赖方向为入口到服务、服务到存储/系统/更新后端；更新核心不注册来源，Iris 控制通过
入口注入的回调操作 UI。`app_update` 是 SDK 的 OTA 组件，产品更新后端使用
`app_system_update`，不能覆盖 SDK 原名。公共接口位于各组件 `include/`，可变状态在私有
上下文中；组件之间只交换请求、快照和资源副本。组件 README 提供最小用法和并发约束。

PC 后端从公共控制器接口创建实例并读取无凭据状态，不能访问 `vibe_ui_t` 内部成员。
[第三方依赖说明](third_party/README.md)记录版本和补丁流程。架构回归
`tests/test_firmware_component_boundaries.py`检查组件图、包含边界及入口职责。

## Vibe Mode 界面与构建

用户可见固件为 **Vibe Mode**；工程为 `vibe-mode`。Recovery 命令、协议标识、
ABI 2 和布局版本 5 不变。固件及原生模拟器使用同一套 C 控制器和轻量绘制代码，
不链接 GSP、LVGL 或其他 UI 框架。应用工程仍可自行选择 GSP，本改动只涉及维护固件。

- `components/app_ui/src/vibe_ui.c`、`vibe_product.c` 保留业务状态机。
- `components/app_ui/light/vui.c` 负责 RGB565 绘制、文字、圆角、列表、键盘、
  GIF 预解码帧、触摸及计时器。资源和帧缓冲位于 PSRAM。
- `vui_decode.c` 验证 PNG CRC、尺寸、颜色类型、解压长度和行滤波；支持 PNG 的合法 1/2/4/8/16 位颜色组合与 Adam7 交错图，尺寸上限 256×256。
  Idea 服务原有的 96×96 图标上限不变。
- BSP、`esp_lcd_touch` 和固定版本 `esp_display_present` 1.1.1 仅作为硬件传输驱动。
  绘制、排版、输入均由 Vibe 实现。旋转先等待在途显示完成，再同步显示与触摸坐标。
- Iris 请求通过队列进入渲染任务，USB 注入触摸走同一路径；截图继续观察 RGB565 提交。

页面结构保存在 `ui/main.json`，由 `tools/build_vibe_ui.py` 从作者布局生成。
`tools/compile_vui.py` 将原版字体、图标、动画和页面编译为固定节点、预烘焙字形、
RGB565+A8 图片，并无损压缩到 `components/app_ui/light/vui_assets.bin`。
普通构建直接嵌入已提交的资源包，只用 Python 标准库验证来源哈希，不下载图形工具。
修改页面或素材后，在 authoring Python 环境中运行：

```sh
python -m pip install -r tools/requirements.txt
python tools/build_vibe_ui.py
python tools/compile_vui.py
```

内置字体保持 Inter/Archivo 与 Noto Sans SC 字形，许可证见 `ui/fonts/OFL-*.txt`。
下载的 MIFF 字形和 PNG 图标仍通过原公开展示契约进入固定页面，不接收远端布局或动作。
键盘支持大小写、符号、删除、确认、64 字节 Wi-Fi 密码上限和显示切换；离开页面清除密码。
恢复出厂设置保留 NAND、设备身份和当前应用。下载先写 NAND，用户明确打开后才写 NOR 并重启。

在此固件目录启动模拟器（主机需 C 编译器、CMake、zlib 开发库与 Pillow）：

```sh
cmake -S pc -B build-sim -DCMAKE_BUILD_TYPE=Release
cmake --build build-sim --config Release
python pc/simulator.py --library build-sim/libvibe_ui.so --directory /tmp/vibe-preview --port 3223
```

macOS 使用 `libvibe_ui.dylib`；Windows 使用 MinGW CMake generator 和生成的 DLL。
打开 `http://localhost:3223/`。浏览器只显示 C 绘制的帧并转发触摸，默认无示例 App，
`--scenario` 仅为测试注入模型数据。主机测试不再需要 GSPC 或 GSP 模拟器环境变量。
安装 `../../tests/requirements-ui.txt` 后，从 utils 根目录运行：

```sh
python -m pytest mosaico-tools/tests esp-mosaico-vibe-mode/tests
IRIS_HOST_SANITIZERS=1 python -m pytest esp-mosaico-vibe-mode/tests/test_vui_memory.py
```

二维码仍是提交的静态图片，按原资源更新工具生成后重新编译轻量资源包。
模拟器检查页面和交互，不能替代面板 TE/DMA、实体触摸、自动旋转及实际更新的设备验收。
固定槽保持 `0x20000` / `0x1c0000`，构建执行镜像大小及 USB/Iris 内部 BSS 检查。
当前评审包已更新；构建哈希、设备结果及验证限制见
[2026-10-15 预编译包更新记录](../../docs/validation-0.2.md#预编译包更新2026-10-15)。
