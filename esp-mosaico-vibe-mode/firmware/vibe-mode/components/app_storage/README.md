# app_storage：NAND 文件系统与访问控制

独占 SPI NAND 挂载状态，提供本地工作任务的非阻塞占用和释放。

## 依赖与边界

BSP、SPI NAND、LittleFS；没有应用组件依赖。

公共头文件仅在 `include/`，内部头文件通过 `PRIV_INCLUDE_DIRS` 使用。其他组件不得包含 `private/` 或持有本组件内部结构。

## 最小用法

```c
if (app_storage_acquire() == ESP_OK) {
    /* 同步文件 IO */
    app_storage_release();
}
```

实际调用者必须处理返回值，完整启动顺序由 `main` 组装。

## 约束

占用者在全部路径释放；挂载失败绝不格式化。注册 Iris 文件卷属于 app_iris。
`app_storage_access_status()` 使新文件请求在本地任务持有 reservation 时立即返回 BUSY，
避免在全卷容量统计后才创建已被主机超时丢弃的句柄。检查不持有租约，不为已经打开的
远端流提供与后续本地任务之间的完整互斥；流生命周期仍由 Iris 管理。

挂载前通过 LittleFS 公开 raw API 只读探测已有卷的容量，探测卸载后才注册 VFS，
不同时挂载两份文件系统。Dhara 恢复坏块估计后可能改变可用逻辑页数；VFS 使用已有卷
的持久化容量，并核对其不超过当前介质容量。探测回调不能写入或擦除，不自动扩容；
损坏卷、空介质及容量不足均保留原数据并返回错误。此路径要求 LittleFS 1.23 或更新版本。

主机容量兼容回归使用真实 LittleFS；未构建固件依赖时，将 `LITTLEFS_SOURCE_DIR` 指向
LittleFS 源码目录。CI 使用固定的 v2.11.2 源码，不依赖下载后的 managed component。

## 读取缓存与 SPI

固件将 `CONFIG_LITTLEFS_LOOKAHEAD_SIZE` 设为 8192 字节，覆盖现有约 6 万个逻辑块。
默认 128 字节只能跟踪 1024 个块，保存应用时会反复遍历已使用空间。真实 LittleFS
回归对比两种位图，检查旧文件完整性并统计相同写入的读取次数；这不代表 NAND 总线吞吐量。

`CONFIG_IRIS_FACTORY_NAND_READ_CACHE_KIB` 默认 64，在 LittleFS 与 Dhara 之间缓存逻辑页。
页数据只分配在 PSRAM，标签与互斥量使用内部 RAM；缓存分配失败回退到直接读取。
0 禁用缓存，128 为配置上限。介质几何、LittleFS 磁盘格式和 NAND ECC 均保持原有语义。

缓存仅由成功的介质读取填充，不缓冲写入。所有 write、erase、ioctl 和 sync 在转发前
清空缓存，包括失败或部分完成的操作；读取错误也清空缓存。这样下载持久化后的校验
不会复用写入数据。每个块设备操作在同一互斥量内完成，覆盖本地任务和 Iris 文件服务。
释放由 LittleFS 的卸载/挂载失败路径负责，包装器同时释放底层设备。

`CONFIG_IRIS_FACTORY_NAND_POLL_SHORT` 只改变本 NAND SPI 句柄的 page-load 与 READY
短事务。数据读取继续通过中断/DMA；写入、擦除、驱动 BUSY 超时和 ECC 检查继续由原驱动
处理。使用公开 SPI API 的链接包装，避免修改 managed_components。原驱动每次同步事务
返回后才发下一条命令，因此切换 polling 时不存在未完成的队列事务；失败不自动重试。

## 计时口径

每次成功的 `app_storage_acquire/release` 输出两行 `nand_perf`。计数为该时间窗口内
整个 NAND 设备的增量，可能包含并发 Iris 文件访问，不是调用任务的独占计时。

- `logical_pages/hits/misses`：LittleFS 请求的页数及逻辑页缓存命中/未命中。
- `raw_data/raw_meta`：实际 SPI page-load 到 cache-read 的完整页/部分页读取次数。
  部分页包括 Dhara 元数据；其他操作的 OOB 标记读取也可能计入，不能当作纯文件有效字节。
- `raw_bytes`：上述数据阶段接收的字节，包含驱动的 DMA 对齐填充。
- `logical_us` 包含底层映射和读取；`raw_us` 是页加载命令开始到数据事务结束；
  `ready_us` 是页加载命令结束到 READY 状态返回。这些字段嵌套，不能相加。
- `page_us/status_us/data_us/other_us`：各类同步 SPI API 调用的累计墙钟时间，包含调度等待，
  不是逻辑分析仪测得的总线占用时间。`spi_bytes` 不含命令、地址和 dummy clocks。
- `io_errors` 为逻辑读取失败次数，`spi_errors` 为 SPI API 失败次数；设备 ECC 拒绝读取
  会继续传递到文件读取失败。日志不关闭或跳过任何完整性检查。

物理层计数直接观察 SPI 命令，避免 LTO 内联 NAND 私有函数导致漏记。
