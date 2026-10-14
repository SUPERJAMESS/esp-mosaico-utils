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

占用者在全部路径释放；挂载失败绝不格式化。注册 Iris 文件卷属于 app_iris。本地任务 reservation 沿用既有语义，不宣称提供与远程文件服务之间新增的互斥。

## 读取缓存与 SPI

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
