# 文件发布的本地联调

本仓库提供可运行的协议测试服务器：单 ZIP 上传、异步内容校验、持久化幂等回执、
草稿版本 CAS、显式发布、能力选版、不可变 manifest 和内容寻址文件服务。
服务器只包含一个测试账户，不含生产账户、审核、撤销后台或部署代理。
本次真实 ZIP → 上传 → 发布 → 下载 → 离线双语 → 安装 → 启动结果见
[实机验收记录](acceptance.zh-CN.md)。

## 主机与服务器

从 esp-mosaico-utils 根目录安装依赖；Python 3.10+，生成字形另需 FreeType/libraqm：

```sh
python3 -m venv .venv-ideas
.venv-ideas/bin/pip install -r mosaico-tools/requirements-ideas.txt
.venv-ideas/bin/pip install -r ESP-Iris/components/esp_iris/tools/requirements.txt
```

创建测试凭据（不要提交生成的私钥、token、状态数据库）：

```sh
mkdir -p /tmp/ideas-test/tls
chmod 700 /tmp/ideas-test
openssl rand -hex 32 > /tmp/ideas-test/token
chmod 600 /tmp/ideas-test/token
openssl req -x509 -newkey rsa:2048 -nodes -days 14 -subj '/CN=Ideas Test CA' \
  -keyout /tmp/ideas-test/tls/ca.key -out /tmp/ideas-test/tls/ca.pem
openssl req -newkey rsa:2048 -nodes -subj '/CN=ideas-test' \
  -keyout /tmp/ideas-test/tls/server.key -out /tmp/ideas-test/tls/server.csr
# 将 192.168.1.231 改为实际 PC 局域网地址。
printf 'subjectAltName=IP:192.168.1.231,IP:127.0.0.1,DNS:localhost\n' > /tmp/ideas-test/tls/extensions
openssl x509 -req -in /tmp/ideas-test/tls/server.csr \
  -CA /tmp/ideas-test/tls/ca.pem -CAkey /tmp/ideas-test/tls/ca.key -CAcreateserial \
  -days 14 -extfile /tmp/ideas-test/tls/extensions -out /tmp/ideas-test/tls/server.pem
chmod 600 /tmp/ideas-test/tls/*.key
PYTHONPATH=mosaico-tools/tools .venv-ideas/bin/python -m mosaico_cli.ideas_test_server \
  --state /tmp/ideas-test/state serve --host 0.0.0.0 --port 18444 \
  --token-file /tmp/ideas-test/token --cert /tmp/ideas-test/tls/server.pem \
  --key /tmp/ideas-test/tls/server.key
```

仅主机测试可省略 TLS 并绑定 127.0.0.1；设备必须使用 HTTPS。服务器重启读取
同一 state 目录会恢复 validating 任务；token 轮换保持测试账户 ID 不变。
默认两个内容校验线程、四个上传接收槽。生产并发与反向代理预算须另行配置。

## 打包、上传与发布

在消费工作区使用自己的 mosaico.py。先构建普通应用的 system-update-bundle，
按 [CLI 示例](../../README.md#publish-a-mosaico-ideas-file-collection) 准备双语作者配置。

```sh
python mosaico.py project package --project projects/demo --output dist/release.zip
python mosaico.py project validate dist/release.zip --json
# 用环境传递测试 token，不将 token 放在命令参数中。
export MAKER_SPARK_TOKEN="$(cat /tmp/ideas-test/token)"
python mosaico.py project upload dist/release.zip --create --yes --json \
  --server https://127.0.0.1:18444 --ca-file /tmp/ideas-test/tls/ca.pem
unset MAKER_SPARK_TOKEN
```

上传成功仍为 draft，设备 GET 此时返回 404。记录 application_id、code、release_id
和 project_version。测试管理员显式发布（在 esp-mosaico-utils 根目录运行）：

```sh
PYTHONPATH=mosaico-tools/tools .venv-ideas/bin/python -m mosaico_cli.ideas_test_server \
  --state /tmp/ideas-test/state publish RELEASE_ID
```

创建同编号但不同身份的测试应用不能覆盖设备已有内容。选择独立测试编号；不要
为了联调删除用户的旧 NAND 目录。需要测试多个独立环境时，预先在测试数据库的
applications 序列划分编号区间，保持已分配编号不变。

## 设备流程

1. 为候选固件设置 CONFIG_IRIS_FACTORY_IDEAS_SERVER_URL 为 PC 的 HTTPS 源站，
   CONFIG_IRIS_FACTORY_IDEAS_CA_FILE 为 ca.pem 的绝对路径。CA 嵌入镜像；证书
   验证与主机名检查保持开启，设备与 PC 在同一局域网。
2. 使用消费工作区 mosaico.py 的既有部署方式安装候选固件；ROM Download Mode
   使用 recover。核对硬件 MAC、Device ID、Boot ID、候选固件摘要与 ABI 2/layout 5。
   本流程不发布 Vibe Mode 自更新服务，也不替换仓库内已审核 bundle。
3. 输入发布 code。网络日志应只有一次带 locale 和 Accept: multipart/related 的 manifest GET，
   响应同时包含根清单、固件清单、当前语言 display/字体/图标；预览前不得有文件 GET。
   预览应显示当前系统语言的名称、描述、图标、版本与大小；取消不得创建
   新目录。确认后下载同一固定 release 的固件和两套语言资源。
4. 检查 /nand/ideas/CODE/current 下的 manifest.json、firmware/ 和 device/zh-CN、
   device/en；原始字节、大小和摘要逐项等于服务器快照。提交日志 .roots 位于
   current 之外。下载完成不改变 NOR 或开机目标。
5. 停止测试服务器，离线切换 English/中文，检查系统界面、列表名称、本地详情。
   重启后语言偏好保持。只有另外确认“设置为开机应用”才安装到 NOR；再启动应用，
   用 mosaico.py 验证同一 Device ID、新 Boot ID、目标应用版本并返回 Vibe Mode。

设备数据、截图、触摸输入与安装都通过消费工作区 mosaico.py。对直接文件读取的
联调扩展也使用其启动器和 Iris 公共文件 API；不要直接操作串口或擦除用户分区。

## 自动测试与覆盖边界

```sh
python3 -m pytest mosaico-tools/tests
python3 -m pytest esp-mosaico-vibe-mode/tests --ignore=esp-mosaico-vibe-mode/tests/test_recovery_ui_host.py
# 在 Iris tools 目录运行自身测试
python3 -m pytest tests
python3 -m ruff check --target-version py38 iris_gateway tests
# 原生 UI 测试需先提供已构建的 esp-gsp 组件及固定版本 GSPC/模拟器
GSPC_EXECUTABLE=/path/to/gspc GSP_SIM_EXECUTABLE=/path/to/sim \
  python3 -m pytest esp-mosaico-vibe-mode/tests/test_recovery_ui_host.py
```

纯主机夹具位于 esp-mosaico-vibe-mode/tests/fixtures/ideas_release.py，不依赖
消费工作区，也不是可烧写的发布镜像。覆盖 ZIP/JSON/图片/字形/布局校验、HTTP
发布隔离、幂等与 CAS、重启恢复、目录故障恢复、预览取消、离线双语、重试上限和
列表点击。主机故障注入不能替代 100 MiB 并发压力、最大资源组合或真实断电耐久试验。

## 下载与安装的性能计时

P0/P1 及本轮 P2 的范围是分段计时、减少同一操作的重复读取、4 KiB 内部 RAM
NOR 写入缓冲。整镜像 PSRAM 缓存、多任务流水线、并行 HTTP 不在本轮范围。
下载仍先逐文件做一次落盘摘要检查，再做固件格式检查；后者仍单独读取应用镜像。

`io_perf` 以微秒输出，每个文件或操作一条摘要，不在每个块打印日志：

| 记录 | 时间含义 |
| --- | --- |
| `http` | `read_us` 为 HTTP 读取等待，`write_us` 为 NAND fwrite，`sync_us` 为 flush/fsync/close；重试的字节也计入 bytes |
| `nand_hash` | 实际 NAND 读取与 SHA-256 分开统计；total 还含打开/关闭、调度让步 |
| `prepare` | 完整预检及必要的目录恢复；供本次安装直接使用 |
| `nand_install` | read 为 NAND 读取、hash 为源哈希、write 为写入调用；begin 含组件初始化/擦除，end 含 NOR 回读及镜像检查 |
| `nor_hash` / `nor_image` | 分别测量 NOR 回读哈希、ESP 镜像格式验证 |
| `boot_nor_hash` | 启动/显式扫描配置的开机应用完整性 |
| `scan` / `operation` | 列表刷新/整个任务的墙钟时间；operation 名称为内部命令号/Idea 编号 |
| `memory` | 内部 RAM 剩余/最大连续块/本次启动最低余量，PSRAM 剩余，当前任务栈余量 |

buffer 字段记录实际指针所属内存及容量。total 包含子阶段，**不能将父子记录相加**；
网络重试、文件打开、目录操作、日志和调度时间不强行归入读写阶段。NOR 写入的
瞬时吞吐不能代替“点击安装至可启动”的端到端耗时。

稳定目录扫描不读 NAND 固件；未完成事务仍完整恢复。安装读取 NAND 固件两遍
（预检、安装），保留写入源哈希、NOR 回读哈希和镜像检查；成功后的刷新不再次
读取 NAND 固件或 NOR 镜像。内部 RAM 分配失败必须发生在该组件擦除之前。

真机 A/B 使用同一设备、同一个已下载 Idea 和相同的 P1 流程，只改变安装器的
4 KiB 缓冲所在内存。测试 PSRAM 变体仅为工作区临时构建，最终源码和设备必须
恢复内部 RAM 版本；保存两个 ELF、维护包、日志、Boot ID 和分段计时。
维护包的 target_layout_sha256 必须等于设备实读分区表；应用安装改变过分区表时，
使用已核对的应用分区表离线重新打包，不能关闭布局检查或改写设备分区表凑匹配。

## NAND 读取分层对照

存储组件增加 `nand_perf` 设备计数，字段及嵌套计时口径见
`esp-mosaico-vibe-mode/firmware/vibe-mode/components/app_storage/README.md`。
按以下配置构建候选，使用同一已下载文件对照，不更改 NAND 格式或 SPI 频率：

| 变体 | NAND_READ_CACHE_KIB | NAND_POLL_SHORT |
| --- | --- | --- |
| 分层计数基线 | 0 | n |
| 仅逻辑页缓存 | 64 | n |
| 缓存及短事务 | 64 | y |

表中选项均以 `CONFIG_IRIS_FACTORY_` 开头。默认交付使用 64 KiB PSRAM 页缓存和
短事务 polling；NAND 数据仍为 40 MHz SIO、DMA 读取。内部 RAM NOR 写入缓冲仍为 4 KiB。

可将 `esp-mosaico-vibe-mode/tests/firmware/nand_read_probe` 作为额外的测试组件加入
`EXTRA_COMPONENT_DIRS`。配置 `CONFIG_NAND_READ_PROBE_PATH` 为一个已有固件文件，
`CONFIG_NAND_READ_PROBE_EVICT_PATH` 为另一份大于缓存的已有文件。测试组件只读取文件，
不注册新 RPC、不写 NAND；在 UI 启动 60 秒后自动执行：

- mode 0 暂停渲染并等待显示传输完成，mode 1 正常界面，mode 2 每 100 ms 更新可见文字。
- 每种界面状态分别创建固定在 CPU 0/1 的读取任务，使用 4 KiB 内部 RAM 缓冲，重复两轮。
- 启用缓存时，先读取第二份文件排出前一轮文件页，再记录目标文件 fread 与 SHA 耗时。
- 每轮输出字节数、SHA-256 和错误码；主机必须核对全部 12 轮的长度/摘要，以及
  `nand_perf` 的逻辑读取错误和 SPI 错误。最后必须出现 complete，且 UI 已恢复。

测试组件的 CMake 仅替换 `factory_ui.c` 的 attach 调用，捕获 UI 句柄供暂停/恢复测试。
它不修改生产组件源码和 managed_components。最终必须移除该额外组件重新构建，
确认 ELF 不包含 `nand_probe_attach`，再验证正常 UI 的下载、安装、应用启动与数据保留。

不同运行状态下，SHA 和 NOR 阶段也可能显著波动。完整安装数字必须同时保留分段计时，
不可将不同状态的总耗时差全部归为缓存收益；固定任务和界面负载的只读对照另行报告。
主机缓存测试覆盖 LRU、越界、跨页、读错误、失败写入后的失效、erase/ioctl/sync、
并发读取和分配失败回退；SPI 测试覆盖句柄隔离、模式选择、READY 追踪和失败不重试。

### 下载阶段、验证去重与任务栈诊断

下载 UI 的验收顺序为传输 → 校验文件 → 检查镜像 → 保存 → 保存完成。
校验文件阶段检查字节与文件数持续推进、速率栏为空；检查镜像/保存不伪造百分比。
通过 `ideas_stage` 的 0/1/2 日志分别确认文件校验、镜像检查、保存开始，完成以
`operation name=1/<code> result=0` 为准。保存可能短于一次截图所需时间，使用日志
和模拟器共同验收，不把已进入完成页的截图当作保存中的截图。

主机测试覆盖阶段顺序、失败不推进，以及镜像或 NOR 摘要失败后不能选择启动分区。
一次安装应只有一条应用 `nor_image` 记录；提交选择 ota_0 后不再重复应用段检查。
NAND 文件 SHA、NOR 回读 SHA 和完整镜像校验都必须保留，跨事务不能使用验证标志。

可选探针的 `CONFIG_NAND_RUNTIME_PROBE=y` 改为任务栈对照，保留正常 UI：

- mode 0：默认内部堆，8 KiB 栈；mode 1：默认内部堆，16 KiB 栈。
- mode 2：16 KiB INTERNAL/8BIT/DMA 栈，强制 HP SRAM；mode 3：16 KiB
  INTERNAL/8BIT/RTCRAM 栈，强制 LP RAM。分别在 CPU 0、CPU 1 运行。
- 使用相同文件、4096 字节 HP SRAM 读取缓冲和 SHA 实现；先用另一文件驱逐页缓存。
- 另外从当前测试布局的 NOR 0x210000 读回相同长度，分别使用栈上 1024 字节缓冲
  和 HP SRAM 缓冲，记录读取、SHA、地址及摘要。此 NOR 地址只适用于该硬件夹具布局。
- 共 24 条结果应长度、摘要、结果码一致。该矩阵不写文件或 NOR。

如需对照实际安装路径，可单独启用 `CONFIG_NAND_IDEAS_DRAM_STACK` 或
`CONFIG_NAND_IDEAS_RTC_STACK`，并清空 `NAND_READ_PROBE_PATH` 禁用自动读探针。
CMake 只向 `ideas_worker.c` 注入测试头：使用同样的 16 KiB 栈大小、原优先级和
不绑核策略，改变栈的内存能力。任务通过 WithCaps 创建并常驻，分配失败不回退。
由 UI 正常触发同一已下载 Idea 的安装；这些选项只用于诊断，不可纳入交付。
最后移除测试组件，核对 ELF 无 `runtime_probe_sequence`/`nand_probe_attach`，
再通过消费工作区的 mosaico.py 恢复正式候选并核对设备身份、布局和健康状态。

### 常驻 HP SRAM 工作线程

生产 Ideas worker 在 Wi-Fi 初始化后预留 16 KiB INTERNAL/8BIT/DMA 栈并常驻，
长度为 1 的指针队列串行接收任务。不要为省空闲内存恢复逐任务创建，或在申请
失败时改用普通 INTERNAL：ESP32-S31 的普通内部堆可能包含慢速 RTC RAM。

`test_ideas_worker_host.py` 使用 pthread 阻塞队列运行真实 worker 循环。覆盖首次
任务/锁/队列/栈分配及首次入队失败的资源清理和重试、运行中入队失败的状态回滚、
30 轮预览/取消/扫描只创建一个工作线程，以及完成状态刚发布就提交下一项的交接。
37 个场景同时运行普通构建及 ASan/UBSan；只模拟 SDK 边界，不替换 Ideas 任务实现。

实机应在同一次启动中依次下载新发布、切换语言、预览、重复安装同一 Idea，核对
各次 `task_start` 地址相同且处于 HP SRAM。保留分段耗时、栈最低余量和空闲内存；
另外验证 Wi-Fi/TLS 下载、应用启动、返回 Vibe Mode 后的 USB 维护更新，确认常驻
栈不会挤占这些路径所需资源。故障注入的低内存行为由主机夹具验收，不能以空闲
设备一次安装成功代替资源不足路径或长时间压力验证。

同时运行 `test_lcd_budget_host.py`，确认 LCD 初始化限制不影响 NAND/其他 SPI 设备、
原配置与回调保持不变，SDK 初始化错误如实传回。核对最终 ELF 包含两个初始化
适配入口（LTO 构建需显式保留）。随后在下载、双语切换和重复安装中检查 `panel_io_spi_tx_color`、`TE ticket`、
`Display render failed` 的错误记录。模拟器不包含真实 LCD DMA 分配，不能替代
这一项。判断当前运行需按启动范围截取日志，Gateway 快照可能保留旧候选的错误。


## 单请求预览回归

传输定义见 [单响应预览格式](preview-response.zh-CN.md)。设备端 ideas_multipart.c
按部分流式填充 PSRAM 缓存，校验整个响应 ETag 和清单引用的各文件 SHA-256。
确认后只下载固件组件和另一语言，根清单以原始 JSON 字节保存。

协议主机测试用独立 MIME 解析器检查真实 HTTP 响应的中文/英文闭包、原始字节、
摘要、发布隔离和参数拒绝。设备 worker 测试覆盖请求数量、单字节分片、部分响应
重试、取消、缺失/重复/错误语言/路径/长度/摘要、超限头、尾随内容和离线双语。
同一次正常预览的 manifest 请求计数为 1，文件请求计数为 0。
