# app_ideas：Ideas 库与展示资源

协调解析、预览确认、下载、目录事务、离线安装及开机身份。cloud/preview/library/worker 分别处理传输、缓存、目录与串行任务。

## 依赖与边界

app_network、app_storage、app_nand_update、app_system；HTTP/NVS/加密。字体和展示解析不依赖 RTOS/GSP。

公共头文件仅在 `include/`，内部头文件通过 `PRIV_INCLUDE_DIRS` 使用。其他组件不得包含 `private/` 或持有本组件内部结构。

## 最小用法

```c
factory_ideas_init();
factory_ideas_request(IDEAS_RESOLVE, "042731");
/* 读取 snapshot，用户确认后 factory_ideas_confirm(preview_id, true)。 */
```

实际调用者必须处理返回值，完整启动顺序由 `main` 组装。

## 约束

预览 ID 和 release 固定；取消后的迟到响应不能下载。等待确认不持有 NAND；副本由调用者 free。内部 package、pending、job 不跨组件暴露。
