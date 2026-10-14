# Ideas 服务功能夹具

通过 `test_ideas_worker_host.py` 编译 `app_ideas` 的全部九个源文件，各自保持独立翻译单元。
场景驱动只调用 `factory_ideas.h` 公共接口，不持有组件私有状态。

使用真实 worker、请求状态、HTTP 处理、SHA-256 校验、字体/展示解析与渲染、目录事务和
开机身份逻辑。SDK 假实现提供可控任务调度、HTTP 响应、NVS 和 NOR；网络与存储服务在
公共接口处替换。HTTP 同时检查 HTTPS、证书包配置、禁止重定向、解析请求契约和 NAND
租约释放，并故意分段读写。每次 worker 完成检查客户端与租约均已释放。

测试动态生成两个固定 release，以及哈希正确但内容损坏的资源。链接 wrapper 把固定
`/nand` 路径映射到 pytest 临时目录，因此无需访问设备、服务端、真实 NAND 或消费 workspace。
下载和重命名使用真实文件 IO；绝不在固件内增加测试 URL 或绕过 TLS 的开关。

覆盖预览与各阶段取消、过期/重复确认、任务创建失败、固定 release、资源复用、同版本修复、
断流/损坏包回滚、HTTP 404/重定向、断电后的目录恢复、离线预览/安装、提交失败、开机身份
与下载版本分离，以及存储/网络不可用时的请求恢复。

`factory_nand_install_idea` 是此夹具的边界：假实现复制已验证文件至模拟 NOR，并调用真实
Ideas 提交前回调。此处验证安装协调与身份持久化，不替代实际更新后端、Flash 分区策略的
`test_bridge_host.py` backend 场景，也不代表云端或真机离线安装验收。
调度器在指定 IO 边界注入取消，不模拟多核抢占时序。

从仓库根目录运行，依赖 C 编译器、OpenSSL 开发头文件和 Recovery 的 cJSON 依赖：

```sh
python -m pytest -q esp-mosaico-vibe-mode/tests/test_ideas_worker_host.py
IRIS_HOST_SANITIZERS=1 python -m pytest -q esp-mosaico-vibe-mode/tests/test_ideas_worker_host.py
```

可通过 `CJSON_COMPONENT_DIR` 指定现有 cJSON 源码。ASan/UBSan 检查资源生命周期和内存边界；
每个场景使用独立进程，组件单例与设备上一样保留到进程结束。
