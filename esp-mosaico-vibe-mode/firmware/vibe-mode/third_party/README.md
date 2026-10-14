# 第三方依赖与适配

依赖统一在 `../main/idf_component.yml` 声明，由 ESP-IDF Component Manager 管理；
`dependencies.lock` 是现有构建工作区的解析产物。ESP-Iris 使用同一 utils revision
的组件，BSP 使用 manifest 中的 Git revision 或消费 workspace 显式提供的版本。
不复制 SDK/第三方组件到 `components/app_*`，不修改 `managed_components/` 源码。

| 依赖 | 当前约束/来源 | 本地适配 |
| --- | --- | --- |
| ESP-IDF | >= 6.2；S31 验证 checkout 由固件验证记录锁定 | 更新后端 SDK 私有 Flash API；bootloader 保持既有实现 |
| ESP-Iris | 同仓库 `ESP-Iris/components/esp_iris` | app_iris 平台钩子和产品服务 |
| ESP-Mosaico BSP | Git `05e067de0613a0aa3fe2af42e75616804decbf4b` | app_system 静音、app_storage NAND、app_ui 显示 |
| ESP-GSP / GSPC | 1.4.0 / 0.5.0 | app_ui bundle 压缩、PNG inflate wrapper |
| esp_display_present | 1.1.1 | app_ui 提交 wrapper，用于按需截图 |
| mDNS / cJSON / LittleFS | 1.12.0 / ^1.7.19 / ^1.22.0 | 使用公共 API |

显式固定 presenter 1.1.1，防止组件图重算时将已验证的传递依赖升级为 1.1.2。
当前 PNG 修正位于 `app_ui/src/vibe_png_inflate.c`，没有修改 GSP/ROM 的源码。
GSP 预编译包通过 `INTERFACE_SOURCES` 导出的公共触摸适配器由 `app_ui`
编译一次，并用 linker `-u` 保证提取；不向入口传播 C 源码，也不修改托管源码。

如需第三方源码修正，将版本、来源、校验值、修改原因和统一 diff 一起提交到
本目录的 `patches/<原组件名>/`。先从锁定来源复制到独立构建目录，校验补丁基线，
应用补丁后通过 Component Manager 的 `override_path` 使用该副本；补丁不匹配时停止构建。
禁止在 managed_components 中手工改代码，禁止修改包缓存或静默忽略补丁失败。
没有源码补丁时不引入补丁执行框架。
