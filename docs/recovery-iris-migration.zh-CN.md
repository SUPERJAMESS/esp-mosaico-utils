<a id="给已有应用添加-recovery-和-iris-日志"></a>

# 给已有应用添加 Vibe Mode 和 Iris 日志

Vibe Mode 是运行 ESP-Iris 的保留固件，与芯片 ROM 下载模式不同；
技术名称及完整固件说明见[Vibe Mode 指南](../esp-mosaico-vibe-mode/firmware/vibe-mode/README.md)。

迁移可以分开做：**先修改分区表、添加 Vibe Mode 镜像；需要业务日志时，再接入
Iris 组件。** 保留原工程目录、业务代码、UI 框架和构建方式，无需先迁成示例工程。

<a id="第一部分只添加-recovery"></a>

## 第一部分：只添加 Vibe Mode

### 1. 在分区表中留出前 2 MiB

将 Flash 的 `[0x000000, 0x200000)` 留给 Vibe Mode 及其系统数据。下面是本仓库
[预置 Vibe Mode](../esp-mosaico-vibe-mode/firmware/vibe-mode/prebuilt/recovery/manifest.json)
配套的系统分区，可合入现有 `partitions.csv`：

```csv
# Name,    Type, SubType, Offset,   Size,     Flags
otadata,   data, ota,     0x9000,   0x2000,
phy_init,  data, phy,     0xb000,   0x1000,
sysmeta,   data, nvs,     0xc000,   0x14000,
factory,   app,  factory, 0x20000,  0x1c0000,
coredump,  data, coredump,0x1e0000, 0x20000,
```

Vibe Mode 镜像放在 `factory`，槽大小为 1.75 MiB；前面的空间还包含 bootloader、
分区表及系统数据。当前预置包适用于 ESP-Mosaico / ESP32-S31 的 16 MiB Flash。

**`0x200000` 之后继续按业务需要安排，无需复制示例的完整布局。** 例如原项目的
应用、NVS、文件系统、UI 资源仍可保留各自的大小与标签，只调整冲突的地址。
原业务若放在 `factory`，需改到 OTA 应用分区，为 Vibe Mode 留出 `factory`。

只核对三件事：

- 分区不重叠、不超过 Flash 容量，应用起址按 64 KiB 对齐，数据按 4 KiB 对齐。
- 当前 Vibe Mode 启动需要可初始化的 `nvs` 和 `sysmeta`；保留 `nvs`，不要只加一个
  `factory` 条目就删掉其他所需数据分区。
- 移动 NVS/文件系统地址不会自动搬数据；需要保留的内容先备份，再按新地址恢复。
  地址、大小和数据格式不变且未被写入覆盖的分区，可保持原样。

上面的前缀是现有预置固件的配套布局，不是要求统一所有应用分区。
单纯启动 Vibe Mode 和使用完整 System Update 是不同能力：后者目前会核对五个
系统分区的固定参数；要改变这些参数，需要一并调整 Vibe Mode 更新实现。

<a id="2-把-recovery-加入工程的安装镜像清单"></a>

### 2. 把 Vibe Mode 加入工程的安装镜像清单

保留原应用构建，将 Vibe Mode 镜像加入安装清单，并使用**自己工程的新分区表**：

| 镜像 | 写入位置 |
| --- | --- |
| 新分区表 `partition-table.bin` | `0x8000` |
| 预置 Vibe Mode 的 `factory.bin` | `0x20000` |
| 原业务应用及资源镜像 | 新分区表中各自的目标地址 |

还要确定启动入口：bootloader 必须能按新分区表选择 `factory` 和业务应用。
需要本仓库的 AI 键（GPIO7）进入 Vibe Mode 功能时，使用配套 bootloader
（`0x2000`）；沿用原 bootloader 时确认它已有相应入口。初始 `otadata` 决定
启动选择，预置 `ota_data_initial.bin` 会让设备先进入 Vibe Mode，不应在日常更新
时无条件重写。

这是镜像集成清单，不是要求业务固件先接入 Iris。实际设备写入通过消费 workspace
的 `mosaico.py` 执行。注意当前 `mosaico.py recover` 写的是**基础整包**，会带上
基础分区表，并没有“只加 Vibe Mode、保留自定义分区表”的选项；若采用上面的
自定义安装清单，需要在该 workspace 的安装流程中接入，不能直接用 `recover`
代替。已有兼容 Vibe Mode 的设备可跳过本步。

<a id="3-确认原应用和-recovery-都能启动"></a>

### 3. 确认原应用和 Vibe Mode 都能启动

验证能够进入 Vibe Mode，业务应用仍能按预期启动，原 NVS、文件和资源可正常
使用。只需要 Vibe Mode，到这里即可，不要求修改业务入口、添加 Iris 依赖或
设置产品身份字段。

## 第二部分：可选，给业务固件添加 Iris 日志

### 1. 加入 Iris 组件

在原项目 `CMakeLists.txt` 的 ESP-IDF `project.cmake` include 之前追加组件目录。
将示例路径换成项目实际引用的 utils checkout，其他构建逻辑保持原样：

```cmake
set(MOSAICO_UTILS_ROOT "/path/to/esp-mosaico-utils")
list(APPEND EXTRA_COMPONENT_DIRS
    "${MOSAICO_UTILS_ROOT}/ESP-Iris/components/esp_iris")
```

在使用 Iris 的业务组件 `idf_component_register()` 中追加 `REQUIRES esp_iris`；
若已有依赖列表，追加到原列表即可。不要再从其他来源引入第二份 Iris。

### 2. 选择连接方式

下面以 ESP32-S31 的 USB CDC0 为例，将配置合并到原 `sdkconfig.defaults`：

```ini
CONFIG_ESP_IRIS_ENABLE=y
CONFIG_ESP_IRIS_TRANSPORT_USB=y
# CONFIG_ESP_IRIS_TRANSPORT_TCP is not set
# CONFIG_ESP_IRIS_TRANSPORT_USB_SERIAL_JTAG is not set
# CONFIG_BSP_USB_CONSOLE is not set
CONFIG_ESP_CONSOLE_UART_DEFAULT=y
CONFIG_ESP_CONSOLE_SECONDARY_NONE=y
# CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG is not set
# CONFIG_ESP_IRIS_OTA is not set
```

Iris 独占 TinyUSB CDC0，原业务不能再次初始化同一 USB 控制器。日志接入不需要
应用 OTA writer，因此这里关闭它。已有 `sdkconfig` 会优先于 defaults，修改后
用 `menuconfig` 确认实际值并重建；板级 Flash/PSRAM 配置继续沿用原项目。

Iris 默认使用现有 `nvs` 保存状态；需要与 Vibe Mode 共用配对和崩溃状态时，再
设置 `CONFIG_ESP_IRIS_NVS_PARTITION_NAME="sysmeta"`。TCP 和其他传输方式见
[Iris 快速开始](../ESP-Iris/components/esp_iris/README_zh.md)。

### 3. 在业务初始化前启动

```c
#include "esp_iris.h"
#include "esp_log.h"

void app_main(void)
{
    ESP_ERROR_CHECK(esp_iris_start());
    ESP_LOGI("app", "Iris logging ready");

    /* 原有业务初始化与任务创建。 */
}
```

普通 `ESP_LOGx` 和 `stdout/stderr` 输出可通过 Iris 查看；已有自定义日志后端
或直接 UART 写入需单独接入。启动前及 ROM/bootloader 日志不会自动回放，日志
缓存满时可能丢弃。只做日志接入，不需要 Vibe Mode 适配组件、产品身份声明或
调整业务健康确认逻辑。

使用现有主机工具连接。已配置 Mosaico workspace 时可执行：

```sh
python mosaico.py iris logs
```

也可使用 [Iris Gateway/Workbench](../ESP-Iris/components/esp_iris/tools/README.md)
查看日志。确认业务任务的日志可见、断开连接后业务仍正常运行即可。

## 以后需要时再添加

| 需要的能力 | 再接入什么 |
| --- | --- |
| 从普通应用通过主机命令进入 Vibe Mode，并完成 Mosaico 自动更新/验收 | [应用 Vibe Mode 适配与产品配置](../mosaico-tools/docs/application-integration.md) |
| 崩溃现场和连续崩溃恢复 | [崩溃诊断示例](../ESP-Iris/components/esp_iris/examples/crash_recovery/README.md) |
| RPC、屏幕镜像/输入、文件服务 | [Iris 示例索引](../ESP-Iris/components/esp_iris/examples/README.md) |

这些能力按需增加；只放 Vibe Mode 或只看日志时，无需先完成整套工具链迁移。
