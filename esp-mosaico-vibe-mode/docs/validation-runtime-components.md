# Vibe Mode 运行时组件重构验证

2026-10-14。本次实现九个 `app_*` 运行时组件及更新、Bridge、Ideas、UI 的内部职责拆分。
产品更新组件命名为 `app_system_update`，避免覆盖 ESP-IDF 自带的 `app_update`。
Recovery ABI 2、layout 5、兼容版本 0.2.0、协议、RPC、NVS 和分区布局保持不变。
bootloader 实现和已评审的 `prebuilt/recovery` 未修改；本记录不代表发布包替换。

## 架构与契约

- 构建依赖图无环，没有应用组件依赖 `main` 或其他组件私有头文件。
- 两种配置的 `compile_commands.json` 均确认 `main` 只编译 `main.c`。
- 19 个公共头文件分别通过 S31 编译器 `-Werror -fsyntax-only` 检查，移除了全部
  应用组件私有包含目录；没有可变业务状态通过跨模块 `extern` 交换。
- 更新后端不注册 NAND；`main` 注册来源和文件卷。Iris 通过复制的 `ctx` 回调操作 UI。
- PC 使用不透明控制器；设备控制器和大快照保留 PSRAM BSS，锁、原子变量和 RTOS
  控制对象留在内部 RAM。没有私有头文件的组件不声明空私有目录。
- 通过 `gcc-nm` 和链接 map 确认四个 `esp_iris_platform_*` OTA 钩子来自
  `recovery_ota_support.c` 的强符号实现。PNG inflate 与显示提交 wrapper 保持强链接。
- GSP 公共触摸适配器只在 `app_ui` 编译一次，通过 `-u,gsp_esp_touch_init` 提取；
  不修改托管源码。GSP 项目配置/DMA 对象仍由原第三方构建配置管理。

架构回归位于 `tests/test_firmware_component_boundaries.py`；组件契约和最小用法见
各组件 README 和 `include/`。第三方版本及适配见
[第三方说明](../firmware/vibe-mode/third_party/README.md)。

## 主机回归

| 范围 | 结果 |
| --- | --- |
| Recovery 与 mosaico-tools（全量，含 27 项工作流及 15 项原生 GSP UI 测试） | 634 passed |
| ESP-Iris Python tests（此前重构回归） | 603 passed |

以上共 1,237 项。实际 Bridge worker、更新后端、Ideas worker、存储事务、服务故障路径、字体/展示解析、
输入导航、进度和截图均有主机回归。硬件故障注入及云端联调不能由这些主机结果替代。

使用 GSPC 0.5.0、GSP simulator 1.4.0；测试命令从 utils 根目录运行：

```sh
python -m pytest -q esp-mosaico-vibe-mode/tests mosaico-tools/tests
cd ESP-Iris/components/esp_iris/tools
python -m pytest -q
```

测试前设置 `GSPC_EXECUTABLE`、`GSP_SIM_EXECUTABLE`，安装相应测试依赖。
最初 `test_project_workflow.py` 依赖另一分支的单项目接口
`mosaico_cli.scaffold.create_project`，导致收集失败。2026-10-14 按当前主线契约修复测试，
使用 `initialize_project(workspace, name)`、`project init`、`--workspace` 和 doctor 的
`checks`/`exit_code`。27 项场景覆盖 workspace 发现与选择优先级、模板创建后的安装命令、
dry-run、错误配置在工作开始前被拒绝，以及真实诊断逻辑在缺少 IDF/Gateway 时的 JSON/文本输出。
全部夹具在临时目录创建配置和模板，没有读取消费 workspace；工具实现未增加兼容接口。
修复后两个测试目录无排除项全量运行，634 项通过，耗时 240.76 秒。

### 无已发布 Ideas 时的功能验证

按用户 2026-10-14 的要求，使用本地生成的固定 release 做功能测试，不依赖正式发布的
六位编号。新增 `tests/test_ideas_worker_host.py` 的 **28 项**已包含在上方 634 项中；
额外启用 ASan/UBSan 复跑同一组 28 项，全部通过，不重复计数。

夹具独立编译 `app_ideas` 的全部九个源文件，通过公共 API 驱动真实 worker、HTTP 处理、
资源校验、渲染与目录事务。固定 `/nand` 路径映射至临时目录；云端响应、RTOS、NVS、NOR
和相邻组件边界由测试实现提供。所有请求均为本地模拟，没有访问或发布远端 Idea。

- 预览只获取描述和展示资源，不下载固件、不写 NAND；等待确认不持有存储租约。
- 排队阶段、解析响应阶段和展示资源读取阶段取消均丢弃结果、关闭 HTTP、允许再次请求。
  就绪预览取消、过期确认和重复确认不会启动额外下载。
- 确认使用预览时的固定 release；复用已验证的字体、图标和 display，不重复请求展示资源。
  完整同版本直接返回目录，损坏的同版本可以重新下载修复。
- 编号 404、重定向、清单损坏、Flash 容量不匹配、响应截断/长度错误/读取失败、SHA 不符，
  以及哈希正确但语义错误的字体、图标尺寸和展示布局均被拒绝。
- 新版本下载断流或哈希错误保留旧版本，重试成功后只保留一个版本；中断的目录重命名
  可通过服务扫描恢复。
- 断网时本地预览和安装协调无需 HTTP；损坏文件在进入安装器前拒绝，提交前身份保存失败
  不标记为开机应用。下载新版本不改变当前 NOR 身份，安装后才更新；NOR 哈希变化会使开机标记失效。
- 存储占用、断网、任务创建失败均能返回可恢复状态，并释放相应资源。

此夹具在 `factory_nand_install_idea` 边界模拟 NOR 写入；它验证 Ideas 安装协调与真实
提交前回调，不代表物理 Flash 安装验收。更新后端策略与提交门控由既有 backend 场景验证。
取消在确定的 IO 边界注入，不覆盖所有多核抢占时序。详细范围与复现命令见
[Ideas 功能夹具说明](../tests/host_ideas/README.md)。

## ESP-IDF 构建与候选包

ESP-IDF 为 `v6.2-dev-2221-g7b9cc1ac79f`，checkout
`7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe`；目标 ESP32-S31，使用当前 manifest 的 BSP revision。
使用项目默认 sdkconfig defaults，另复制配置关闭 System Update、产品后端、NAND System
Update、Bridge System Update 和 Bridge bootloader update；普通 USB OTA 保留。

| 配置 | factory.bin | 0x1c0000 固定槽余量 | 静态 DIRAM | PSRAM BSS |
| --- | ---: | ---: | ---: | ---: |
| 默认 | 1,798,784 B | 36,224 B | 240,368 B | 108,136 B |
| 系统更新关闭 | 1,718,960 B | 116,048 B | 239,464 B | 83,272 B |

静态内存统计来自 IDF `size`，不是运行时峰值。两个配置的 bootloader 和固定槽检查均通过。
SDK 自身仍有 `esp_wifi`/`wpa_supplicant` 私有包含目录提示；本轮没有修改 SDK 或托管源文件。

在 `firmware/vibe-mode` 中分别使用 `build-refactor`、`build-refactor-disabled` 构建。
`recovery-current-bundle` 校验 manifest/layout/hash；`recovery-self-update-bundle`
生成单一 Recovery 组件的 `.irisfw`。没有运行 `update-recovery-prebuilt`。

默认 `factory.bin` SHA-256：
`d7de6aea682075bc5a399c0466c2dd3453f8fb71afa162c450031d96db1b7288`。
设备握手的 ELF SHA-256：
`e0dd623cf72804d474d99ce4162cc9a5aa1d20bbac9045f4f298ccc17b5dc6fa`。
自更新包按原规则填充整个固定槽，其组件 SHA-256 与未填充 BIN 不同。

## 真机范围

全部设备操作通过消费 workspace 的 `mosaico.py`，使用独立验证配置；没有执行 ROM 写入。
在 S31 上使用 `iris system-update --bundle` 安装同版本 0.2.0 候选：现有版本策略允许
同版本替换，拒绝降级和跨主版本。USB 控制/数据端点分别建立会话后完成更新。

- 自更新操作 `f4a41a58-4888-4318-80d6-958810380bdb` 成功；重启后健康状态和 ELF 摘要匹配。
- 更新前后 bootloader SHA-256 均为
  `3aca7c68771490abe84ef13304b95cb5a0eaae9c97a4fc25d6fc12aa14d80a77`，
  分区表 SHA-256 均为 `8d28cdded305dd5f31c4a2aec315e4577e8e55f8ade887775d543306e6eb59a6`。
- `iris restart` 成功启动既有 `iris_acceptance` 1.0.1 应用，再通过
  `iris test enter-recovery` 返回本次候选；启动 ID 变化，未替换普通应用。
- 480×480 RGB 截图经 USB 数据通道保存；远程按下/抬起 RPC 回显一致，下载页和
  My Ideas 导航正常；维护 RPC `0x1202/4` 也通过注入回调打开下载页。NAND 扫描完成，当前显示 0 个 Ideas，无格式化操作。
- Wi-Fi 使用已有凭据自动连接；候选状态 `lifecycle_state=2`，`crash_count=0`。
  保留了设备原有崩溃证据，没有清除它。

本机证据保存在消费 workspace 的 `.codex-runs/runtime-components/`，原始操作日志在其
`.codex-runs/mosaico/0.2/`。截图没有包含 TCP pairing token。

TCP 真机验收按用户 2026-10-14 的要求暂缓，本轮不再要求提供 pairing token 文件。
用户确认暂无正式发布的 Ideas；下载、损坏包、取消/重复确认、资源复用及离线安装协调
已按上述受控数据完成主机功能验证，不再以提供测试编号为本轮阻塞条件。
真实云端下载至设备、物理 Flash 离线安装尚未执行，不把主机结果计为真机通过。
UI/网络/NAND 故障下保留维护通道目前由主机故障注入覆盖，未进行破坏性实机故障注入。
