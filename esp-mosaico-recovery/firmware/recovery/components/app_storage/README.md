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
