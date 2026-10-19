# app_update_check：在线版本检查与固件安装

Wi-Fi 获取 IP 后异步读取独立 HTTPS 清单，发布是否有新版本及可安装元数据；用户确认后下载并写入 OTA 槽。

## 依赖与边界

app_system_update、app_http_client；ESP-IDF HTTP/OTA/分区。

公共头文件仅在 `include/`，内部头文件通过 `PRIV_INCLUDE_DIRS` 使用。其他组件不得包含 `private/` 或持有本组件内部结构。

## 约束

自动检查至少间隔 60 秒，手动检查绕过间隔，执行期间合并请求。快照提供检查状态、完成序号及错误；失败保留上次成功的版本信息。
默认读取 `https://mosaico-vibe-rec-ver.esp-claw.com/esp-mosaico-vibe-mode-stable.json`，验证 product 为 `esp-mosaico-vibe-mode`。
HTTP 响应必须为 200，限制 4 KiB、10 秒超时、开启证书校验且不跟随重定向。

检查清单与下载固件在发送前调用 `factory_http_client_set_common_headers()`（见 app_http_client）。
