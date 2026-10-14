# app_system_update：系统更新策略与事务

管理唯一系统写入器、清单及分区授权、镜像验证、Flash 提交和 Iris 后端。内部按 transaction、manifest、policy、flash、iris_adapter 分工。

## 依赖与边界

app_system；ESP-IDF Flash/OTA、ESP-Iris、cJSON、加密 API。

公共头文件仅在 `include/`，内部头文件通过 `PRIV_INCLUDE_DIRS` 使用。其他组件不得包含 `private/` 或持有本组件内部结构。

## 最小用法

```c
factory_system_update_register();
/* 来源先 reserve，再 prepare/begin/write/end/commit；失败 abort。 */
```

实际调用者必须处理返回值，完整启动顺序由 `main` 组装。

## 约束

本组件不能反向注册 NAND/Bridge。app_update 是 ESP-IDF 自带组件，故应用组件命名为 app_system_update。锁留在内部 RAM，计划数组保持 PSRAM BSS；分区表最后提交，Ideas 延迟重启契约保持不变。
