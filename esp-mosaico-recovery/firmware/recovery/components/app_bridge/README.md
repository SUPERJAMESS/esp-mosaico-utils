# app_bridge：HTTPS 维护 Bridge

管理远程会话、HTTP、授权执行、取消与结果上报，以及启动恢复。它是产品应用组件，原 iris_bridge C 接口和协议标识保留。

## 依赖与边界

app_network、app_system_update、app_system；ESP-Iris、HTTP/TLS、NVS、加密及启动 ABI。

公共头文件仅在 `include/`，内部头文件通过 `PRIV_INCLUDE_DIRS` 使用。其他组件不得包含 `private/` 或持有本组件内部结构。

## 最小用法

```c
iris_bridge_resume_boot();
/* 按产品配置显式调用 factory_bridge_open() 或 iris_bridge_start(&config)。 */
```

实际调用者必须处理返回值，完整启动顺序由 `main` 组装。

## 约束

常规 Ideas 页面不自动启用历史 Bridge 流程。worker/observer/reporter 通过同一私有上下文组合，交接 HTTP 句柄前须等待上一所有者退出；已授权关键提交不能被页面退出中断。
