# feat/261020 合入 main 后的 Windows 回归

日期：2026-10-21。独立消费工作区为
`D:\tmp\mosaico-feat-261020-rebase-20261021`；所有设备操作均通过该工作区的
`mosaico.py`，并发等级按用户要求仅为单台和九台。

## 源码与产物

`feat/261020` 原远端为 `9e65e79962fb57bd8488de2fb9c2bc7bca22cb14`，
已 rebase 到 `main` 的 `c6b43a6be7fb8aba832fc4016d4076702f5414ae`。
原分支及第一次 rebase 后的分支均有本地备份。保留 feature 的设备端变更、
main 的每串口读取执行器及 Windows 主机修复；Ideas 域名随 main 更新。
最终九台设备回归的 Gateway 源码 revision 为
`32cd746c75a71d0875a80b64d84ac52f2b0e62a3`；后续文档提交不改变设备代码。

SDK 使用 ESP-IDF 6.2.0 源码，revision
`7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe`，目标 ESP32-S31。
Vibe Mode 及 A/A2/B/C 夹具均从源码构建、冻结 bin/ELF/map/配置及更新包。
没有替换发布目录中已审核的预编译固件。

| 样本 | 镜像字节数 | ELF SHA256 |
| --- | ---: | --- |
| Vibe Mode | 1,668,096 | `55344e2b08f8191c35837fe0c5db7f9366d05a2ea19cf3a2ac0f6f289890ad5f` |
| A | 388,992 | `041bd6a7775c2bd4d2342d8628d7957ce42f1d2b687f063ba562a45f29fc5805` |
| A2 | 388,992 | `be090a81260ee33fafda204e59114ae34d62c68dc45b88f41e62adde89115295` |
| B | 12,181,376 | `3ebc7c897886cd5ec76ceabf7ede11702ace65517b589f308e4e07ac30b56d6a` |
| C | 389,424 | `482622c07b22de088c3aa78cdea8fddefcac16a4de4f9e169ee6cb672dc61ecd` |

C 另含两个各 1 MiB 的资源分区，仅从测试应用空间划出；保持
`ui_apps@0xf00000` 不变。A/B 的完整分区表摘要为
`3c26b245870110b955f490e07fe047309b44b098c5adce18a449b1010bb2fd12`，
C 为 `53ef40109a5b3d69630a40e666406115b02daf9f34cee2b185649f5a6c3eb487`。
未擦除凭据、身份、NVS、NAND 或 Vibe Mode 数据。

## 本轮修复

- 合并后的 UI 包重新按 UTF-8/LF 生成，字体补齐所需中文字符；核对 58 项源文件摘要，
  最终资源包 355,707 字节。Windows 原生 UI 测试补齐本地 DLL 放置，浏览器测试等待路由清理。
- 对尚未发送 BEGIN 的失败操作，结果核对优先采用已确认的 writer/recovery Boot ID，
  避免把进入 Vibe Mode 前的 Boot ID 当作写入器身份。旧失败不重放。
- 设备事件只取得当前设备快照，避免每条事件重复序列化九台完整身份。
- 同组件传输计数同时受一秒和 1% 采样限制；阶段/组件变化和末字节立即保存。
  接纳、提交 receipt 和最终结果仍持久化。未改变协议窗口、分块大小或 Flash 校验。
- 控制台绑定使用有界的已提交 capture 范围缓存，避免扫描全部历史段。
  重启、裁剪、缓存淘汰及其他写入器导致序号不连续时回退到完整查询；
  淘汰后延续的 capture 不可冒充从 offset 0 开始的新 capture。

本机隔离 SQLite 进度写入测量：450 次提交共 18.561 秒，平均 41.25 ms，
中位数 33.93 ms。该路径同步占用 Gateway 主循环；小镜像百分比变化频繁，
九台同时更新时会放大停顿。另一个已确认开销是全设备快照的重复转换。
这些是主机实测瓶颈，不能据此认定剩余差距均由 Windows USB 驱动造成。

## 最终矩阵与仍未解决的故障

单台完成 Vibe Mode 自更新、A、纯代码 A2 OTA、B、C、恢复 A 和返回 Vibe Mode。
全部通过；另八台 Boot ID 不变。载荷、C 的两个完整资源、布局、receipt、目标身份、
新 Boot ID、HEALTHY 和崩溃状态均核对，逐次比较原 `ui_apps` 和 bootloader 摘要。

| 更新阶段 | 单台 | 九台 | 九台整轮耗时 |
| --- | --- | --- | ---: |
| Vibe Mode 自更新 | 1/1 | 8/9 | 121.301 s |
| 小应用 A | 1/1 | 8/9 | 51.314 s |
| 纯代码 A2 OTA | 1/1 | 8/9 | 57.097 s |
| 12.18 MB 应用 B | 1/1 | 9/9 | 218.235 s |
| C：应用、双资源、分区表 | 1/1 | 9/9 | 122.417 s |
| A：恢复原布局 | 1/1 | 9/9 | 55.168 s |

最终更新矩阵 **57/60 通过、3 次失败**；两次故障后的单台 A 恢复操作独立记录，
不替换原矩阵结果。九台自更新时，单台阶段的设备已是相同镜像，按产品逻辑可跳过
受保护槽的重复写入，因此不能把该轮称为九台均实际重写 Vibe Mode。

结束时九台均回到上述源码 Vibe Mode、原布局、零崩溃计数及空队列。
`ui_apps` 原摘要九台均为
`741c2606ff491682ad0cd15855d3b388a7ffc97f678f152ec2d1e318cf160577`。
最终事件写入器排空、storage 正常、本进程丢弃事件为零。
只结束本任务的保活客户端，Gateway 随后正常退出，公共 `iris status --all` 返回空列表。

三次已归档的失败都发生在传输完成后的重启/启动验收阶段：

| 阶段 | 设备尾号 | Operation ID | 结果 |
| --- | --- | --- | --- |
| Vibe Mode 自更新 | `30eda0f45b66` | `9ca9290b-a591-4228-bba4-1ac7a1ec685f` | 目标镜像、提交 receipt 匹配，但新启动 crash count 为 1 |
| 小应用 A | `30eda0f45214` | `f9894a5e-e9b0-48b0-a7c7-61b9ba37626c` | receipt 成功，仍为 Vibe Mode，未进入目标 normal 应用 |
| 纯代码 A2 OTA | `30eda0f45394` | `b6498ba2-38bd-4a47-ae35-2c821d43972a` | 数据收齐，重启后仍为 Vibe Mode |

三台均记录 `reset_reason=7`（看门狗复位）、`crash_count=1`。
保留的 Core Dump 均早于此次故障，内含 ELF 与当次镜像不匹配，不能用其中的调用栈
解释此次复位。持久化的失败 ELF/Boot ID 是上次启动身份，不是故障 PC；
若新镜像在早期身份探测前复位，字段仍可能指向 Vibe Mode。
用户确认 `30eda0f45b66` 未接 UART0，因此尚无本次有效早期串口日志。
后续成功的恢复或其他阶段不会覆盖这三次失败，不能称为全量回归通过。

较早一轮 C 操作 `c5a50ec5-9e04-41f0-9d37-d0ccf54b0f93` 的 BEGIN 前库存查询超时
单独保留。只读核对确认同一 writer Boot、空闲事务、接收字节为零及旧 receipt，
结果为 `observed_failure`。另一次较早自更新中丢弃的 1,368 条历史事件也保留为缺口，
后来的健康写入器不能恢复这些记录。

## 传输速度与统计边界

单位为十进制 kB/s。仅使用同一 APP 组件 `transferring` 阶段的实际字节差值和
进度时间戳，排除擦除、其他组件、读回、提交和重启；整轮耗时单列于上表。
用户明确 Mac 的约 300 KB/s 是每台设备的速度，本机尚未恢复到该水平。

| 场景 | 每台速度 |
| --- | ---: |
| 单台小应用 A | 165.8 kB/s |
| 单台纯代码 OTA | 179.4 kB/s |
| 单台大应用 B | 233.5 kB/s |
| 九台小应用 A，各自完整传输窗口 | 23.4–30.6 kB/s |
| 九台纯代码 OTA，各自完整传输窗口 | 27.3–144.9 kB/s，中位数 35.3 |
| 九台大应用 B，各自完整传输窗口 | 88.0–95.0 kB/s |
| 大应用 B 九台共同传输窗口内的采样 | 86.7–93.1 kB/s |

纯代码 OTA 中两台提前传输，为 121.2 和 144.9 kB/s，其余七台为 27.3–36.9 kB/s；
本轮九台 DATA 阶段没有共同重叠窗口，不能将两台的高值概括为九台并行速度。
B 的共同窗口长 111.41 秒，每台采用落在该窗口内的首末实际进度点计算，未插值。
大应用整轮耗时由较早相同 B 样本的 322.202 秒降至 218.235 秒，约缩短 32%。

这验证了主机开销的改善，仍不能解释全部 Mac/Windows 差距。
九台 B 采样中主线程活动样本约 11.775/20 秒，JSON 序列化、协议编码及事件处理仍有开销。
未改变 USB 参数、关闭看门狗或跳过校验来追求速率，也未证明剩余等待属于驱动、总线
或设备端某一条路径。不同平台的原始条件尚不足以构成严格的同包同接线对照。

B 测试穿插两轮九台只读状态查询，分别 8/9、7/9 成功；失败返回设备暂不可用或找不到
所选设备的端点，发生在 USB 切换两端的窗口，不能记成更新写入失败。
更新本身 9/9 完整验收通过；查询失败的原始输出仍保留。

当前完整九台进度点另存为 `evidence/transfer-progress-points.json`。
早期单台中间事件已被正常滚动保留裁剪，但裁剪前保存的首末计数和时间戳仍在
`transfer-rate-comparison-before-final-complete.json`，其来源在最终速率文件中标明。
不使用已不完整的旧 B 片段重新计算优化前的纯传输基线。

## 软件验证和证据

- Iris 最终全量：854 passed、10 skipped；最后缓存淘汰边界与相关路径定向复测：54 passed。
- `mypy` 74 个模块通过，Iris Ruff/Python 3.8 目标检查及资源预算通过。
- Tools/Vibe 联合全量原有 14 项失败的日志保留；修复 Windows 依赖和测试夹具后，
  四个受影响文件复测 128 passed、2 skipped。main 域名和 UI 合同复测 71 passed、2 skipped。
- Workbench：11 项单元测试及构建通过；最终 E2E 单 worker 为 22 passed、4 skipped。
  默认并行执行中的失败日志保留，不把单 worker 结果当作并行无波动的证明。

完整原始证据位于上述独立工作区的 `evidence/`：
`regression-final-state.json`、各阶段 `summary.json`/CLI 输出、
`final-self-failure-crash.json`、`final-small-failure-crash.json`、
`final-ota-failure-crash.json`、冻结产物索引 `artifacts/index.json`、
JUnit、构建日志、CPU profile 和传输时间点。
最终汇总为 `evidence/final-report.json` 及 `evidence/final-report.txt`。
最终设备约束和排空状态为 `evidence/final-invariants.json`、
`evidence/final-drained-health.json`、`evidence/final-after-release-status.json`。
