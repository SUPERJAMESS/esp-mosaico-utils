# mosaico-tools 0.2 迁移

0.2 使用 Gateway API major 2、Iris wire version 2 与
`esp-iris-system-update/0.2` bundle。旧设备、旧 Gateway 和旧 bundle 均须使用
独立旧环境处理；没有兼容解析、协议切换或自动制品转换。

主机的项目会话、设备所有权和基础固件验证记录隔离到 `esp-mosaico/0.2` 状态目录。
Ideas 账户凭据和续传记录也在该目录下，须重新登录；旧目录保持原样，不自动导入。
工具不读取、搬迁或删除旧验证记录，不接管仍存活的旧 Gateway。
Python 依赖环境与可变设备状态分开；不再复用源码目录的旧 `.venv`。

使用工具所属目录的 `mosaico-tools/mosaico.py`，或工作区的对应 launcher。
旧 `esp-mosaico-vibe-mode/mosaico.py`、其 Python 转发包和旧构建脚本路径已移除。
应用接入 [公共集成入口](application-integration.md)，使用新的 test 分区契约后重建。
不能仅替换旧包的版本字符串。

工作区 `.mosaico.json` 使用 `schema_version: 2`，旧 schema 1 会直接报错。
根据当前路径重新创建配置；本仓库的配置已明确更新，不提供运行时转换器。
模板描述自身的 schema 与工作区 schema 独立；格式未变化的模板仍使用 schema 1。

更新自动化脚本中的命令路径，旧写法不再接受：

| 旧路径 | 0.2 路径 |
| --- | --- |
| `init` | `project init` |
| `install` | `iris app-update` |
| `system-update` | `iris system-update` |
| `list` | `iris list` |
| `monitor` | `iris logs` |
| `memory` / `crash` / `rpc` | `iris memory` / `iris crash` / `iris rpc` |
| `session run` / `session status` | `iris run` / `iris status` |
| `device claim` / `release` / `reconcile` | `iris claim` / `iris release` / `iris reconcile` |
| `enter-recovery` / `recovery-wifi` / `bridge-code` | `iris test enter-recovery` / `iris test recovery-wifi` / `iris test bridge-code` |

`doctor`、`recover` 等未改变的正式命令名继续使用。

默认仍通过 `iris system-update` 安装新工程、布局或资源，通过 `iris app-update`
更新完整分区表不变的应用代码。两者需要高速 USB 或 TCP 独立数据链路；
只有 UART／USB Serial/JTAG 时，日志、RPC、状态、按需截图可用，固件更新会明确失败。
该限制不影响原生 `idf.py flash monitor` 或 ROM 基础恢复。

本地工程的 System Update 在写入前核对 bundle 与构建镜像哈希，并将 BIN、ELF、
map 归档到项目 Gateway，以便后续按完整 ELF 哈希解码崩溃。仅提供 `--bundle`
的安装没有工程调试文件，不自动推断或借用其他构建的 ELF。

显式连接串口时使用 `iris run --endpoint PORT --baudrate 74880`，也可在
`iris claim`、`iris device-status` 等首次连接命令上指定这两个参数。
默认波特率为 115200，应与设备原生 console 配置一致。显式指定 USB Serial/JTAG
端口即允许连接该控制台；自动发现不会主动打开 Serial/JTAG 或通用 UART 桥。
握手后仍须验证实时 Device ID；端口路径和 USB 描述不能代替设备身份。

已有匹配 0.2 基础固件时，可关闭所属 Gateway，直接使用原版 monitor 或原生烧录。
空白设备、第三方完整烧录后的设备及旧布局设备先执行 recover，再核验实时身份、
模式与契约。迁移擦写范围和启动意图详见
[Vibe Mode 迁移指南](../../esp-mosaico-vibe-mode/docs/migration-0.2.md)。

仓库内评审基础包已更新为 0.2.0，普通 `recover` 和 `app-update` 默认使用它。
验证后续源码候选使用 `recover --source current`，通过验收后再整体替换评审包。
候选固件的代码更新验收使用 `iris app-update --recovery-source current`，明确以
上述命令生成的候选包做版本和兼容性校验；不替换评审目录，也不会在候选缺失时
回退到其他基础包。普通 `app-update` 仍默认校验评审包。
