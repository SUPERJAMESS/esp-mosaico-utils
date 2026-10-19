# app_network：Wi-Fi 与发现

管理扫描、连接、凭据持久化及 mDNS；用快照发布状态。

## 依赖与边界

app_system、app_update_check；ESP-IDF Wi-Fi/netif/event、NVS、mDNS、ESP-Iris 公共身份接口。

公共头文件仅在 `include/`，内部头文件通过 `PRIV_INCLUDE_DIRS` 使用。其他组件不得包含 `private/` 或持有本组件内部结构。

## 最小用法

```c
factory_network_start();
factory_network_request_scan();
```

实际调用者必须处理返回值，完整启动顺序由 `main` 组装。

## 约束

事件回调使用私有上下文；快照锁跨失败重试保留；密码只在连接成功后持久化。

获取 IP 后异步触发 Vibe Mode 固件版本检查；HTTP 请求不在网络事件回调中执行。
业务 HTTP 公共请求头由 [app_http_client](../app_http_client/README.md) 提供。
