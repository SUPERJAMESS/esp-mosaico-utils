# app_nand_update：离线系统更新来源

扫描 NAND 更新目录，校验和流式读取本地组件，适配普通 NAND 更新与 Ideas 离线安装。

## 依赖与边界

app_storage、app_system_update、app_system；ESP-Iris 公共类型和启动 ABI。

公共头文件仅在 `include/`，内部头文件通过 `PRIV_INCLUDE_DIRS` 使用。其他组件不得包含 `private/` 或持有本组件内部结构。

## 最小用法

```c
factory_nand_update_init();
factory_nand_update_request_scan();
```

实际调用者必须处理返回值，完整启动顺序由 `main` 组装。

## 约束

普通异步任务自行占用 NAND；Ideas 安装入口要求调用者持有占用且已验证全部文件。RPC 与卷注册不在本组件中。
