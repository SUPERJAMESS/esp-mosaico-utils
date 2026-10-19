# app_http_client：公共 HTTP 请求头

为 Vibe Mode 的业务 HTTPS 请求统一设置可选设备身份元数据。请求的
URL、认证头、API 参数和请求体仍由调用方负责；本组件只管理产品级公共请求头。

## 依赖与边界

ESP-Iris 公共身份接口与 ESP-IDF HTTP client。

公共头文件仅在 `include/`，其他组件不得包含本组件内部实现。

## 最小用法

```c
if (factory_http_client_set_common_headers(client) != ESP_OK) {
    /* 处理请求头设置失败 */
}
```

调用方必须在 `esp_http_client_open()` 或 `esp_http_client_perform()` 前调用，
并继续管理 HTTP client 的生命周期。身份暂不可用时会移除旧值并允许请求继续。
