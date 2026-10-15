# app_update_check：固件版本检查

Wi-Fi 获取 IP 后异步读取独立 HTTPS 清单，发布是否有新版本及版本号的快照。
网络事件调用 `factory_update_check_request(false)`，手动检查传入 `true`；UI 使用 `factory_update_check_snapshot()`。
用户确认后调用 `factory_firmware_install_start()`，通过固定 HTTPS 目录下载完整镜像并在 PSRAM 中校验，
使用共享后端的 FIRMWARE owner 执行单组件自更新；安装状态通过 `factory_firmware_install_status()` 读取。
依赖 app_system_update 公共接口，不包含其私有头文件。

自动检查至少间隔 60 秒，手动检查绕过间隔，执行期间合并请求。快照提供检查状态、完成序号及错误；失败保留上次成功的版本信息。
默认读取 `https://mosaico-vibe-rec-ver.esp-claw.com/esp-mosaico-vibe-mode-stable.json`，验证 product 为 `esp-mosaico-vibe-mode`。
HTTP 响应必须为 200，限制 4 KiB、10 秒超时、开启证书校验且不跟随重定向。
