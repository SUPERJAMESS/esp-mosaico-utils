# 2026-10-18 振动退出清理验证

Vibe Mode 在启用触感前注册 `esp_restart()` 清理回调。回调与振动 worker、
定时器共用互斥锁，停止定时器、关闭电机，并拒绝退出期间的后续脉冲。
覆盖打开应用、OTA 和显式软件重启；注册失败时不启用触感。

- 外设启动与退出回归：8 项通过，启用 ASan/UBSan。覆盖振动中退出、脉冲间暂停时
  退出、已派发回调、后续输入、重复清理和初始化失败。
- Ideas、Bridge、设置和组件边界回归：74 项通过。
- ESP32-S31 构建通过，0 warnings；ESP-IDF revision 为
  `7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe`，保留环境原有 camera 修改。
- BIN：1,683,232 bytes，固定槽剩余 151,776 bytes。
- BIN SHA-256：`eda3ab50eaee1ebbd91f8391108bdd3ef03356a05886a0ddead4790efd0fa609`。
- ELF SHA-256：`a20e6d054a18194cc21480878c8f2a1fb231da23b5be89eb1820dc287bb012c9`。

设备 `4553502d49524953010030eda0f40c10` 通过消费工作区 `mosaico.py` 更新。
写入前归档原有 11,200-byte Core Dump，仅更新 recovery 组件。
操作 `32e3235c-3440-4b0a-af11-427e509a1876` 返回 `succeeded`、`healthy: true`
和匹配的目标 ELF。Boot ID 从 `11514190037770494340` 变为 `6280854816293456942`；
安装后读取确认 HEALTHY、崩溃计数 0、无待处理崩溃恢复。
bootloader、完整分区表摘要和 layout 5 保持不变，未替换 reviewed prebuilt。

随后请求重启到现有应用，工具未能验证重连。用户说明当前应用无法返回，并要求停止
测试、提交改动；已停止后续设备操作。此轮确认了修复固件安装和运行健康，
不将应用往返或电机实际停止的体感结果记为通过。
临时诊断补丁已还原，本任务持有的设备连接已释放。

构建日志位于 utils 的
`.codex-runs/haptic-shutdown/build/20261018-202823-build-674754/raw.log`。
消费工作区的 `.codex-runs/haptic-shutdown-device/` 留存 `crash-before.json`、
`inventory-before.json`、`update.json`、`inventory-after.json` 和
`restart-to-app-error.json`；主机测试日志位于 utils 的 `.codex-runs/haptic-shutdown/`。
