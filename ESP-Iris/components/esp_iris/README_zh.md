# ESP-Iris

[English](README.md)

ESP-Iris 减少嵌入式开发中不必要的编译和烧录时间，让设备的日志、
状态和操作结果更容易被开发者与 AI Agent 同时理解、控制和追踪。把
零散的串口调试，变成一个统一、结构化、可恢复的设备开发流程。

ESP-Iris 0.2 在标准 ESP-IDF 控制台上增加命令和有界、可打印的 API 记录。
不启动 Gateway，也能使用 `idf.py flash monitor`。UART 和 USB Serial/JTAG
使用 IDF 原生复位；S31 应用 USB 使用随组件提供的 Iris 扩展进入 ROM 并跟踪端口重枚举。
单个 worker 服务控制链路与可选的独立数据链路，后者承载连续媒体、文件和固件。
设备侧不运行
HTTP server、WebSocket server、JSON parser、framebuffer mirror，也不进行
媒体规模的内存分配。

多台设备可以通过独立的 USB 或 TCP 端点同时连接到一个 Python Developer
Gateway。Gateway 通过稳定的 `device_id` 识别设备，汇聚并持久化设备集群事件，
再将它们同时分发给 React Web Workbench、命令行客户端和外部 Agent。

当产品需要浏览器工程界面，但不希望在量产固件中嵌入 Web 服务器或调试 UI
时，可以使用 ESP-Iris。

```text
ESP32 A -- USB CDC0 ---------\
ESP32 B -- USB Serial/JTAG ---+--> Python Gateway + 持久化 --+--> Web Workbench
ESP32 C -- TCP/Wi-Fi --------/                                  +--> CLI 客户端
ESP32 D -- TCP/Ethernet -----/                                  +--> 外部 Agent
```

每台设备有一个 Gateway 所有者，控制和数据会话独立建立、断开。
Gateway 与原版 monitor 互斥占用控制台，无需切换设备模式。
一个 Gateway 可以并发监督多个设备端点。
Workbench、CLI 和 Agent 连接分别接收独立的事件流；修改设备的操作会按
设备串行化。

## 芯片与传输支持

组件声明 ESP-IDF >=5.5，这是依赖下限，不代表所有芯片和 SDK 组合均已验证。
当前 CI 使用固定的 ESP-IDF 6.2 开发提交，新增 `multichip` 构建组：

| 芯片 | 控制链路 | 测试工程 | 验证范围 |
| --- | --- | --- | --- |
| ESP32-C2 | UART | `services_c2` | 编译通过；现有控制链路 fixture 实机通过，74880 baud |
| ESP32-H2 | USB Serial/JTAG | `services_h2` | 编译通过；现有控制链路 fixture 实机通过 |
| ESP32 | UART | `services_uart`，显式指定目标 | 本地编译通过，尚无本轮实机验证 |
| ESP32-S31 | UART、USB Serial/JTAG、应用 USB、TCP | 既有 baseline/system 构建组 | 沿用既有验证，本轮未操作设备 |
| 其他 SDK 目标 | UART；具备外设时可选 USB Serial/JTAG | 尚未纳入构建矩阵 | 未验证 |

在仓库根目录初始化固定版本 IDF 后，执行 `bash tools/ci/build_firmware.sh multichip`。
各目标使用独立构建目录。前三项于 2026-10-16 在 IDF 提交
`7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe` 编译通过；该本地 SDK 另有摄像头驱动修改。
远程 CI 与实机验证需分别记录，不能以编译成功替代。

2026-10-16 已通过消费工作区的 `mosaico.py` 完成 C2/H2 实机验证，使用本分支主机端
连接设备已安装的 `fc63d43-dirty` service fixture，本次未烧录新固件。覆盖 HELLO、
状态/堆内存、二进制 RPC、控制链路截图、日志、异步任务、每板 80 次持续 RPC，以及
软件重启和硬件复位后的同一设备、新 Boot ID 确认。C2 现有固件需显式指定 74880 baud。
两块板当前固件均未启用任务内存观测；文件与持续媒体明确拒绝缺少数据链路的请求，
OTA 和 coredump 解析未做实机验证。固件身份及证据哈希见
[实机验证记录](test_apps/multichip-validation.json)。本次同时修复了串口不重新枚举时
无法识别重启的问题：持续探测 HELLO，身份未变化的回复不重复触发认证。


同日补充验证：同一 Gateway 同时连接 C2/H2，持续 180 秒、每板 377 次 RPC；
每板 3 次重启期间另一板持续响应。未知服务/方法、非法参数和 RPC 超时后的恢复、
64 条日志突发、任务取消、每板 3 次释放/重新接管均通过。H2 自动进入 ROM 读取身份
并恢复应用后的冒烟测试通过，无 Flash 写入。C2/H2 物理拔插与用户确认的完全断电冷启动均已通过：
自动重连后身份不变、Boot ID 更新，二进制 RPC 正常。

主机统一识别镜像芯片 ID，升级时要求镜像与设备实际目标一致，并在进入 Recovery 后
继续核对产品、板型、布局和 ABI 契约。未知 ID 或缺失目标不会默认成 S31。
coredump 根据归档镜像选择目标及 Xtensa/RISC-V GDB，保留 `ESP_IRIS_GDB` 显式覆盖。
UART 进入 ROM 下载的复位需要已知的实时目标；普通运行复位和 USB Serial/JTAG 复位
不依赖 HELLO 身份。

UART、USB Serial/JTAG 提供控制链路；文件、OTA、系统更新写入和持续媒体仍需要
固件启用对应服务，以及独立连接的应用 USB 或 TCP 数据链路。应用 USB CDC 驱动及
ROM USB 下载扩展仍限定 S31；新增芯片 ID 识别不等于开放该驱动。其他 USB 适配器、
IDF 版本和板级复位电路需另行验证。Mosaico 的 Recovery ABI 和布局规则仍由产品层提供。

## 包含内容

| 层级 | 能力 |
| --- | --- |
| 设备组件 | 单个有界 worker、二进制协议、日志、状态、RPC/Job、媒体、崩溃证据、配对和 OTA |
| Developer Gateway | 并发 USB/TCP 端点监督、设备集群会话管理、认证、持久化操作、制品存储和 REST/WebSocket API |
| Web Workbench | 设备概览、日志、RPC、屏幕/输入、媒体、固件、操作记录和系统设置 |
| 命令行客户端 | 面向开发者与 Agent 的可脚本化控制和稳定 JSON 输出 |

默认配置只启动核心链路以及日志/状态能力。RPC 表、媒体缓冲区、配对和 OTA
均由 Kconfig 限制，并且只在配置或实际使用时启用。

## 环境与兼容性

| 范围 | 要求 |
| --- | --- |
| ESP-IDF | 5.5 或更高版本 |
| TCP | 默认控制端口 19772、数据端口 19773；网络由应用负责 |
| Application USB | ESP32-S31；CDC0 文本控制台、CDC1 独立数据接口 |
| USB Serial/JTAG | 保留标准控制台、硬件复位和 JTAG；支持控制服务和按需截图 |
| UART | 标准文本控制台、RPC 和截图；保留 IDF 控制台波特率 |
| Gateway | Python 3.8 或更高版本；当前真实设备主要在 Linux 验证 |
| Workbench | 构建随组件发布的 React 源码需要当前 Node.js/npm 环境 |

每个固件可以编译一种或多种设备传输。启用多种传输时，物理连接按有界窗口
协商；合法 HELLO_ACK（TCP 配对时须认证）取得对应链路角色的所有权。
控制与数据链路校验相同设备、启动与所有者，允许数据先连接；一条链路断开
不关闭另一条。仅打开端口不会取得所有权。

## 快速开始

### 1. 添加组件

在应用的 `main/idf_component.yml` 中添加：

```yaml
dependencies:
  lisir233/esp_iris: "^0.2.0"
```

新增或修改 managed dependency 后运行 `idf.py reconfigure`。

### 2. 选择传输

运行 `idf.py menuconfig`，进入：

```text
Component config > ESP-Iris device link > Device transports
```

- **TCP** 提供可打印控制台及独立、经过认证的数据端口。
- **USB** 使用 CDC0 文本/控制接口和 CDC1 数据接口，配置
  `CONFIG_TINYUSB_CDC_COUNT=2`，通过接口描述符发现角色。
  S31 可启用 `CONFIG_ESP_IRIS_USB_AUTO_DOWNLOAD`，在 CDC0 识别下载握手；
  主机扩展加载与首次安装见 [minimal USB 烧录说明](examples/minimal/README.md#run-and-verify)。
- **USB Serial/JTAG / UART** 保留原生控制台和复位。Gateway 与 monitor、
  烧录工具互斥占用同一端口；没有数据链路也能 RPC、Job、输入及按需截图。
- 已有 ESP-IDF REPL 的产品启用 `CONFIG_ESP_IRIS_CONSOLE_EXTERNAL_INPUT`，
  使用 [esp_iris_console.h](include/esp_iris_console.h) 注册 `iris` 命令。
  产品保留输入驱动所有权，并配置至少容纳两条完整协议记录的接收队列。

可以选择任意兼容组合。启用多种传输时，候选连接受可配置握手超时约束；没有
候选正在协商或活动时，`esp_iris_status_t` 的 transport 为 `NONE`。

具体限制参见[示例索引](examples/README.md)。

### 3. 启动 ESP-Iris

```c
#include "esp_iris.h"

void app_main(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
    /* 在早期复位归因探测之后再初始化产品服务。 */
    ESP_ERROR_CHECK(esp_iris_start());
}
```

这个早期幂等探测会先记录正在运行的镜像，从而覆盖后续产品初始化期间的崩溃。
`esp_iris_start()` 会兜底调用它，但无法覆盖到达 `esp_iris_start()` 之前发生的
崩溃。panic、watchdog 和 CPU lockup 默认计入连续崩溃；brownout 和电源毛刺
默认不计，除非显式配置。普通应用稳定运行到设定时间，或调用
`esp_iris_mark_healthy()` 后清零计数。`esp_iris_start()` 是幂等的。
`esp_iris_stop()` 会释放 worker、传输、VFS 和
stdio 所有权，之后可以再次启动。

### 4. 构建固件

```bash
idf.py build
```

建议先使用 [`minimal`](examples/minimal/README.md) 示例验证 TCP、USB CDC0
或 USB Serial/JTAG，再集成到产品中。

### 5. 启动 Developer Gateway

在源码仓库中执行：

```bash
ESP_IRIS_COMPONENT_DIR=components/esp_iris
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r "$ESP_IRIS_COMPONENT_DIR/tools/requirements.lock"

cd "$ESP_IRIS_COMPONENT_DIR/tools/frontend"
npm ci
npm run build
cd -

python "$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py" web
```

单个锁文件使用 Python 版本条件，pip 会为当前激活解释器自动选择已验证的依赖组。
解释器 major/minor 变化时应重新创建隔离环境。Gateway 运行时与所选 ESP-IDF
版本要求的 Python 相互独立；ESP-IDF 工具环境仍须使用该版本所要求的解释器。

如果组件由 Component Manager 下载，将 `ESP_IRIS_COMPONENT_DIR` 设置为
`managed_components/lisir233__esp_iris`。Gateway 启动后打开
`http://127.0.0.1:8443/`。

无需硬件即可运行 Demo：

```bash
python "$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py" web --demo
```

USB/TCP 选择、认证、TLS、CLI、数据保留和开发命令参见
[Gateway 与 Workbench 指南](tools/README.md)。

## 功能与资源行为

| 功能 | 默认行为 | 设备侧上限 |
| --- | --- | --- |
| 链路和状态 | 启用 | 一个 worker 和固定协议缓冲区 |
| stdout/stderr 日志 | 启用 | `CONFIG_ESP_IRIS_LOG_RING_BYTES` |
| RPC handler | 由应用注册 | `CONFIG_ESP_IRIS_MAX_RPC_HANDLERS` 和 `CONFIG_ESP_IRIS_RPC_BODY_BYTES` |
| 保留 Job | 由应用创建 | `CONFIG_ESP_IRIS_MAX_JOBS` |
| 屏幕/图像/音频 | 主机开始流传输前保持空闲 | 每个活动 channel 一个 `CONFIG_ESP_IRIS_MEDIA_LATEST_BYTES` 缓冲区 |
| 崩溃证据 | 存在时只读提供 | 使用 `CONFIG_ESP_IRIS_CRASH_CHUNK_BYTES` 分块 |
| TCP 配对 | 默认关闭 | 一个 NVS token 和 challenge-HMAC 状态 |
| OTA writer | 可配置；默认允许跨项目更新 | 使用 `CONFIG_ESP_IRIS_OTA_CHUNK_BYTES` 分块；`CONFIG_ESP_IRIS_OTA_REQUIRE_PROJECT_NAME_MATCH` 可要求与当前项目名匹配 |
| System Inventory | 产品注册只读 provider 后启用 | 返回系统保护区实际哈希和上一笔提交结果，不含任何写回调 |
| System Update | recovery 注册产品 backend 后才启用 | 签名 manifest、组件/manifest/签名/分块上限，且不提供通用 raw-Flash API |
| 文件服务 | 应用注册逻辑卷后才启用 | 一个文件任务、一个流和每块 `CONFIG_ESP_IRIS_FILE_CHUNK_BYTES` |

媒体 channel 使用 credit 和 latest-chunk 策略，慢速主机不会在设备侧产生无界队列。

启用 PSRAM XIP 和外部协议缓冲的普通固件可使用
`CONFIG_ESP_IRIS_USB_FIFO_PSRAM`（满足依赖时默认启用）。仅 CDC 软件 FIFO 的字节
存储移入外部 BSS；容量、USB endpoint DMA 缓冲、流状态和 FreeRTOS 锁位置不变。
构建时对 TinyUSB 0.21.0~2 生成经过源哈希检查的适配副本，不修改 managed 源码；
未知 CDC 源码版本会明确使配置失败，需审查适配或关闭该选项。开启外部 BSS 时，
`CONFIG_ESP_IRIS_SERVICE_STATE_PSRAM` 也迁移固定大小的文件卷注册表。
写入固件仍使用独立的内部内存配置。

`esp_iris_schedule_restart(delay_ms)` 复用协议任务，不另建任务或定时器。
任务/RPC 调用方负责选择启动目标并记录计划重启。调度不依赖连接存活，等待当前
服务回调完成，并在指定延迟后为两条 TX 队列提供最多 100 ms 的排空宽限；显式
调用 `esp_iris_stop()` 会取消它。


应用必须在 `esp_iris_start()` 前只注册产品明确允许导出的目录：

```c
ESP_ERROR_CHECK(esp_iris_file_volume_register(
    &(esp_iris_file_volume_config_t) {
        .id = "cfg",
        .base_path = "/littlefs/export",
        .capabilities = ESP_IRIS_FILE_VOLUME_READ |
                        ESP_IRIS_FILE_VOLUME_LIST |
                        ESP_IRIS_FILE_VOLUME_MTIME |
                        ESP_IRIS_FILE_VOLUME_WRITE |
                        ESP_IRIS_FILE_VOLUME_DELETE |
                        ESP_IRIS_FILE_VOLUME_MKDIR |
                        ESP_IRIS_FILE_VOLUME_RENAME |
                        ESP_IRIS_FILE_VOLUME_ATOMIC_REPLACE,
    }));
ESP_ERROR_CHECK(esp_iris_start());
```

线上路径始终相对于注册根目录。Gateway 以流式方式上传和下载，不会把完整文件
缓存到内存，下载支持 HTTP Range。上传在目标同目录创建临时文件，使用严格
offset ACK、SHA-256、`fsync` 和 rename；只有底层 VFS 确实满足替换语义时才应
声明 `ATOMIC_REPLACE`。重命名不覆盖已有目标，删除只接受文件和空目录，不开放
递归删除或跨卷操作。

## 安全边界

- USB 依赖物理访问，不执行链路配对。
- 稳定 Device ID 由芯片出厂 eFuse Base MAC 派生，不依赖 NVS。Raw TCP 配对
  默认关闭；开启后 token 保存在 `CONFIG_ESP_IRIS_NVS_PARTITION_NAME` 选择的
  NVS 分区中（默认 `nvs`），链路
  通过随机 challenge 证明持有 token，token 本身不在链路上传输。
- Gateway 默认允许 loopback 免登录；非 loopback 客户端需要开发者登录或
  命名 Agent Token。Agent Token 的文件权限分为 `files.read`、`files.write`
  和 `files.delete`，新 token 默认只有 `files.read`。
- HTTP 会向本地网络暴露凭据和设备数据，只应在可信开发网络使用；其他环境
  应开启 Gateway TLS。
- Wi-Fi 密码、配对 token、TLS 私钥和 Agent Token 必须放在被忽略的本地配置
  或私有文件中。
- System Update 只应由保留的 factory recovery 同时注册只读 inventory provider
  和写 backend；正常固件仅注册 inventory provider，用于重启后的闭环校验。
  Gateway 和设备分别验证签名 manifest；产品 backend 必须固定信任公钥、保护
  系统固定区并逐项核对组件。Inventory 必须对当前 Flash 的完整保护区计算哈希，
  包含擦除态 `0xff` 填充，不能直接回传 sysmeta 中保存的期望值。

## 使用 mDNS 发现 TCP 设备

启用 TCP transport 的产品可在自行初始化 mDNS 并设置 hostname 后发布 DNS-SD
服务：

```c
ESP_ERROR_CHECK(mdns_init());
ESP_ERROR_CHECK(mdns_hostname_set("my-product-a1b2c3"));
ESP_ERROR_CHECK(esp_iris_mdns_register(NULL));
```

该 API 发布 `_esp-iris._tcp.local.`，实例名为唯一的
`ESP-Iris-<MAC 后缀>`。ESP-Iris 只拥有这项服务；产品调用 `mdns_free()` 前应先
调用 `esp_iris_mdns_unregister()`。mDNS 元数据只用于本地链路发现，不承担认证，
也不会广播 pairing token。

## 示例

所有公开示例都随组件发布，位于 [`examples/`](examples/README.md)。

| 示例 | 传输 | 适用场景 |
| --- | --- | --- |
| [`minimal`](examples/minimal/README.md) | TCP、USB CDC0 + CDC1、USB Serial/JTAG 或三者同时 | 身份、生命周期、状态、日志和传输仲裁 |
| [`tcp_wifi`](examples/tcp_wifi/README.md) | TCP | 应用管理 Wi-Fi 和 DHCP |
| [`tcp_pairing`](examples/tcp_pairing/README.md) | TCP | Challenge-HMAC 配对和 token 配置 |
| [`rpc_jobs`](examples/rpc_jobs/README.md) | USB CDC0 + CDC1 | RPC handler 和可取消 Job |
| [`display_input`](examples/display_input/README.md) | USB CDC0 + CDC1 | 截图、屏幕镜像和指针输入 |
| [`media_streams`](examples/media_streams/README.md) | USB CDC0 + CDC1 | 合成图像和 PCM 音频流 |
| [`file_transfer`](examples/file_transfer/README.md) | USB CDC0 + CDC1 | 流式文件上传/下载和元数据操作 |
| [`ota`](examples/ota/README.md) | USB CDC0 + CDC1 | Recovery-first/直接 OTA、验收和回滚 |
| [`file_service`](examples/file_service/README.md) | USB CDC0 + CDC1 | FATFS 逻辑卷和有界文件操作 |
| [`crash_recovery`](examples/crash_recovery/README.md) | USB CDC0 + CDC1 | 保留 Core Dump，并在连续崩溃后进入 factory recovery |
| [`lifecycle`](examples/lifecycle/README.md) | USB CDC0 + CDC1 | 停止、注销、重启和重连 |

硬件内部 fixture 保留在 `test_apps/`，不进入发布归档。

## 文档导航

- [公共 C API](include/esp_iris.h)
- [System Inventory provider API](include/esp_iris_system_inventory.h)
- [System Update backend API](include/esp_iris_system_update.h)
- [协议常量](include/esp_iris_protocol.h)
- [Wire protocol 0.2](protocol/spec.md)
- [0.1 → 0.2 迁移指南](protocol/migration-0.2.md)
- [Golden vectors](protocol/golden_vectors.json)
- [Gateway 与 Workbench](tools/README.md)
- [示例索引](examples/README.md)
- [工程架构](https://github.com/esp-mosaico/esp-mosaico-utils/blob/main/ESP-Iris/docs/esp-iris-architecture.md)
- [变更记录](CHANGELOG.md)

## 版本与许可证

当前源码目标版本为 `0.2.0`；发布前请使用本仓库源码。设备、Gateway 和主机
API 必须配套升级，不兼容 0.1。ESP-Iris 使用
[Apache-2.0](LICENSE) 许可证。
