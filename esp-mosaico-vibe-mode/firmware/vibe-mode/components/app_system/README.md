# app_system：启动安全与保留元数据

提供功放静音、启动诊断、sysmeta 初始化和最后更新结果。业务命名空间仍由所属服务管理。

## 依赖与边界

ESP-IDF NVS/OTA/GPIO、BSP、产品 ABI；没有应用组件依赖。

公共头文件仅在 `include/`，内部头文件通过 `PRIV_INCLUDE_DIRS` 使用。其他组件不得包含 `private/` 或持有本组件内部结构。

## 最小用法

```c
app_system_mute_audio();
factory_system_metadata_init();
```

实际调用者必须处理返回值，完整启动顺序由 `main` 组装。

## 约束

先静音再启动服务；sysmeta 初始化失败不得通过擦除恢复。
