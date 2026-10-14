# app_ui：显示、输入与可移植控制器

管理设备显示、GSP bundle、截图及渲染调度；ui_services 将业务快照转换为界面模型。vibe_ui 是设备和 PC 共用的不透明控制器。

## 依赖与边界

app_network、app_ideas、app_nand_update、app_system_update、app_bridge；GSP/BSP/ESP-Iris 公共 API。

公共头文件仅在 `include/`，内部头文件通过 `PRIV_INCLUDE_DIRS` 使用。其他组件不得包含 `private/` 或持有本组件内部结构。

## 最小用法

```c
factory_ui_start();
/* 非渲染任务通过 factory_ui_open_bridge(NULL) 请求打开下载页。 */
```

实际调用者必须处理返回值，完整启动顺序由 `main` 组装。

## 约束

业务初始化在 main 完成。控制器状态只在渲染任务访问；外部请求有截止时间和请求 ID。设备控制器使用 PSRAM BSS，PC 使用组件自有静态存储；返回句柄不得 free。PNG 和显示提交 linker wrapper 由本组件链接。
