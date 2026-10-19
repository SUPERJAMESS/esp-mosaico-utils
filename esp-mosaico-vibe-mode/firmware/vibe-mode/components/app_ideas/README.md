# app_ideas：Ideas 库与展示资源

协调解析、预览确认、下载、目录事务、离线安装及开机身份。cloud/preview/library/worker 分别处理传输、缓存、目录与串行任务。

## 依赖与边界

app_network、app_storage、app_nand_update、app_system；HTTP/NVS/加密。字体和展示解析不依赖 RTOS/GSP。

公共头文件仅在 `include/`，内部头文件通过 `PRIV_INCLUDE_DIRS` 使用。其他组件不得包含 `private/` 或持有本组件内部结构。

## 最小用法

```c
factory_ideas_init();
factory_ideas_request(IDEAS_RESOLVE, "042731");
/* 读取 snapshot，用户确认后 factory_ideas_confirm(preview_id, true)。 */
```

实际调用者必须处理返回值，完整启动顺序由 `main` 组装。

`IDEAS_FORMAT_NAND` 仅用于用户明确确认后的 NAND 恢复，不接收 code。它在现有 worker
异步执行，拒绝与当前任务或待确认预览并行；取得存储和 NOR 读租约、停止 Iris 后才
格式化。格式化结束后尝试恢复 Iris、释放租约并报告结果；未完成服务排空时不擦除。
成功后清空列表/图标/标题缓存并重新发现开发版本，下一次下载或列表访问可正常使用。
快照中的 `storage_error` 表示初始化或底层读取错误，与扫描、网络和包校验错误分开。

## 约束

预览 ID 和 release 固定；取消后的迟到响应不能下载。等待确认不持有 NAND；副本由调用者 free。内部 package、pending、job 不跨组件暴露。


## 文件发布与双语

仅接收 mosaico-ideas/device-release/1，直接 GET 外层 manifest；IDEAS_RESOLVE
枚举名保留内部调用 ABI，不对应网络 resolve 请求。先验证根 ETag、原始固件清单
及当前语言预览；确认后下载所有语言与固件到 .incoming，完整校验后原子提交。
根清单不重复组件或展示元数据，目录外 .roots 记录根摘要和应用身份，阻止同编号
不同应用覆盖。current/.previous 恢复只选择完整已验证集合；旧格式不迁移或删除。

HTTP 仅使用 HTTPS，10 秒连接、30 秒清单/10 分钟文件总预算、60 秒无进展；
临时网络错误及 429/502/503/504 最多额外三次退避，遵守 Retry-After。
固件下载失败不切换 current，下载成功也不写 NOR；安装另行确认。

CONFIG_IRIS_FACTORY_IDEAS_SERVER_URL 选择源站。测试私有 CA 可通过
CONFIG_IRIS_FACTORY_IDEAS_CA_FILE 嵌入；没有关闭证书或主机名验证的开关。
系统语言由 app_system 的 factory_locale 管理；列表标题通过有界异步缓存渲染
name 字形块，中文名称不依赖系统固定字体。标题保留已排版字形，在设备侧重排为
有界单行 QOI 缓存，供列表、本地详情和预览标题按原比例绘制及横向滚动；正文仍为
固定 RGB565 块。display/1 与 MIFF/1 发布格式保持不变。详见仓库的文件发布协议和联调说明。

离线展示只读取已提交根清单、固件清单和所需的展示资源，并逐项校验摘要；
稳定目录的启动扫描、列表刷新、IDEAS_RELOCALIZE 不读取 NAND 应用二进制。
只有 .incoming/.previous/双根日志等未完成事务走完整恢复；安装的 prepare
返回完整校验结果，调用者在同一存储租约内复用。成功下载立即收敛日志并清理旧代，
避免下一次普通刷新重复读取固件。独立恢复、后续安装仍重新验证完整集合，
不保存跨操作的“已校验”标志。列表可展示之后损坏的副本，但安装预检必须拒绝它。

安装共读取 NAND 固件两遍：完整预检一遍，边读取边哈希并写 NOR 一遍。
NOR 回读 SHA-256、镜像检查和分区表最后提交保持不变。安装后只刷新元数据，
复用本次成功安装的 NOR 校验结果；下载/语言切换不重复计算 NOR 哈希。
启动和显式扫描仍核对配置的 NOR 哈希；开机记录变化使本次启动内的结果失效。
主机夹具以实际文件打开次数和 NOR 读取字节数约束这些路径。

管理页导航复用当前目录，只有前次扫描失败时重试；开机与内容变更的刷新仍保留。
打开前的 NAND 校验逐 4 KiB 块报告字节数与已校验文件数，UI 用独立读取阶段计算
速率。NAND 和当前 NOR 的摘要检查都在块间检查取消；取消不视为当前代损坏，
不会尝试回退到旧代，也不追加列表扫描。真正写入或启动前在同一锁内检查取消
并关闭取消入口，避免取消请求越过不可中断的提交边界。

保存当前开发版本也允许取消：NOR 摘要、复制、NAND 校验及提交前复核均检查取消。
接受取消后清理本次临时副本，保留已有版本；最终校验通过后，在同一锁内关闭取消
入口，再发布新副本，避免保存成功却显示已取消。

NAND 安装器在擦除前申请 4 KiB MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT 缓冲，
分配失败即终止。UI/文件资源仍可使用 PSRAM；本轮没有增加整镜像缓存或下载并行。
`io_perf` 每文件/阶段输出微秒计时、字节数、缓冲所在内存及任务结束时的内存余量。
详见文件发布联调文档中的性能验收说明。

下载快照的 `phase` 依次为 DOWNLOAD、VERIFY_FILES、CHECK_IMAGE、SAVE、COMPLETE。
传输与文件校验分别从零累计 `received`，`total` 只包含内容文件，不含预览已取得的
外层根清单。校验时按实际读入并送入 SHA 的字节更新进度，仅摘要匹配后增加
`files_verified`。UI 进入校验后隐藏传输速率；镜像检查与保存没有可准确累计的
字节百分比，显示阶段状态；提交和列表刷新完成后才显示保存完成。失败不推进阶段。

同一安装事务的应用组件完成标志还要求 `image_verified`：NOR 回读 SHA 匹配后，
执行一次完整 `esp_image_verify`。提交时使用 IDF 的
`esp_ota_set_boot_partition_skip_validate` 复用这次验证；启动分区选择本身的校验
仍由 IDF 完成。完成的组件不能重写，新事务清空标志；失败、重启、重新安装不能复用。

ESP32-S31 的 INTERNAL 堆可能包含 LP/RTC RAM。Ideas 在 Wi-Fi 初始化后创建一个
常驻工作任务，使用 INTERNAL | 8BIT | DMA 限定的 16 KiB HP SRAM 栈，优先级 3、
不绑核；所有扫描、预览、下载、安装和语言切换任务共用它。长度为 1 的队列传递
任务指针，沿用 busy/pending 准入规则，保证串行执行。完成状态发布后立即提交的
下一项可先入队，但要等上一项清理完成后才执行，避免瞬时重叠占用两个工作栈。

初始化是启动线程的串行调用；成功后重复调用不创建新任务。首次任务、互斥锁、
队列或 SRAM 栈分配失败时释放本次资源，返回 ESP_ERR_NO_MEM，允许稍后重试，
不回退到 RTC/PSRAM。只有全部资源建立后才向公共 API 发布 initialized 状态。
空闲时仍保留这 16 KiB 栈及任务/队列控制结构，这是换取稳定时延的常驻内存成本。

`io_perf` 记录任务栈地址、CPU、HP SRAM/RTC 最大空闲块及缓冲地址，便于区别
存储总线时间与慢栈造成的 CPU 开销；同一次启动中的 Ideas 任务应复用同一栈地址。
不能把 `internal_free` 或缓冲标记为 internal 当作任务栈也在 HP SRAM 的证据。

常驻栈需要和显示 DMA 一起做内存预算。Vibe 的 `vibe_lcd_budget.c` 在 LCD 总线
初始化处申请 3840 字节传输上限，并将 LCD IO 队列限制为 1；IDF 按 DMA 描述符
对齐后，临时数据约为 4 KiB，另有分配器/事务开销。它只约束板载 LCD，保留
BSP 的频率、引脚、PSRAM DMA 策略及其他 SPI 总线设置，避免语言切换或 TLS
活动时屏幕同时申请多个较大的内部 DMA 块。代价是减少显示传输的排队重叠。
