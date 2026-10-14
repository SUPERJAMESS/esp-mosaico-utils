# app_iris：Iris 产品服务适配

提供 Inventory、维护 RPC、普通 OTA 钩子、NAND 卷和远程输入注册。接受 UI 操作回调，不依赖 UI 组件。

## 依赖与边界

app_system、app_storage、app_network、app_nand_update、app_system_update、app_bridge；ESP-Iris 公共接口。

公共头文件仅在 `include/`，内部头文件通过 `PRIV_INCLUDE_DIRS` 使用。其他组件不得包含 `private/` 或持有本组件内部结构。

## 最小用法

```c
app_iris_ui_t ui = {.open_download = open_page, .touch = inject_touch, .ctx = ctx};
factory_recovery_control_register(&ui);
/* 其他 providers/卷注册后 recovery_ota_support_start()；UI 就绪后注册输入。 */
```

实际调用者必须处理返回值，完整启动顺序由 `main` 组装。

## 约束

回调表被复制，ctx 生命周期覆盖整个服务。启动 Iris 前发布文件卷；NAND 挂载失败保留 USB。平台 OTA 强符号及重启定时器必须随启动对象链接。
