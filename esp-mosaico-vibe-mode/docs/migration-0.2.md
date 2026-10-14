# 0.2 启动契约与迁移

评审基础包现为 0.2.0，验证范围见[验收记录](validation-0.2.md)。0.2 工具
拒绝 0.1 Gateway、设备协议及基础包，不提供自动转换或跨代升级。

## 启动契约

- 产品契约：`esp-mosaico/v2`；布局：`mosaico-retained-test-2m-v2`；Recovery ABI：2。
- 保留固件：`vibe_mode`，`app/test`，`0x20000`，大小 `0x1c0000`。
- 正常应用：`main_app`，`app/ota_0`，从 `0x210000` 开始；大小由应用资源布局决定。
- `sysmeta` 的 NVS 容器格式不变；操作结果记录仍为 64 字节，版本为 2，
  不解释旧记录，不清空凭据或身份。
- 软件维护请求保存在 `sysmeta` 的新命名空间 `mosaico_boot_v2`，键 `intent`
  为 u32，`0x02000001` 表示进入 Vibe Mode，0 表示已消费。

请求进入维护模式时先验证保留镜像，再提交 NVS 请求，然后安排重启。
bootloader 只读取意图；GPIO7、有效软件请求、基础安装标记和无应用启动目标
选择 test 分区，镜像验证由 IDF loader 执行。读取意图出错时优先尝试维护固件。
Vibe Mode 在启动外设和更新服务前将意图提交为 0。消费前掉电会重试维护启动；
消费后按正常选择或无有效应用回退规则启动。GPIO61 仍由 ROM 处理。

基础包的 otadata 为 8192 字节，除偏移 32 处的 16 字节
`MOSAICO-BOOT-02!` 外均为 `0xff`。该标记确保 `recover` 在仍有有效应用时也先
进入 Vibe Mode。Vibe Mode 提交 NVS 消费状态后擦除标记所在的首个 otadata
扇区，保留第二扇区。普通应用的原生 flash 使用 IDF 标准空白 otadata，
不带此标记；不能将基础安装 otadata 混入普通应用烧录清单。

返回正常应用先通过 IDF 验证并选择 OTA 分区，再清除软件维护意图。调用者负责
串行化启动转换并在成功后重启。单个应用槽与单份维护镜像不提供 A/B 无损回滚。

## 制品与主机

基础包 manifest 使用 schema 3，声明 `vibe_mode/test` 和 `main_app/ota_0`。
System Update 使用 `esp-iris-system-update/0.2`，拒绝旧 `/v1`、`/v2` schema。
Bridge 保留服务的 `/api/v1` URL，但必须使用 `control_protocol=2` 独立授权，
不再回退到 progress 授权；新的产品 profile 为 `iris-s31-test-layout-v2`，
factory manifest 的 `protocol_version` 为 2。URL 版本不代表 Iris 设备协议版本。
Bridge 待启动记录使用 `iris_bridge_v2` 命名空间和版本 2，不读取旧记录。
配套远程服务必须支持这些契约；本仓库改动不代表云端已经发布。

bootloader 保持原 24 KiB 窗口，默认输出 Error 级日志。Vibe Mode 保留断言检查，
省略重复断言字符串以满足固定镜像容量；通过 ELF 与 Core Dump 定位故障。
构建仍检查 bootloader 和整个 Vibe Mode 镜像的真实大小，不扩大分区规避检查。

## 文档迁移流程

1. 用旧环境导出必要数据、日志和 Core Dump，记录硬件 MAC；释放旧 Gateway。
2. 准备匹配的新 bootloader、分区表、带启动标记的 otadata 和 Vibe Mode 镜像。
   逐项核对哈希和擦写范围；保留 `sysmeta`、NVS、Core Dump 及未声明写入的资源。
3. 通过 ROM 与 0.2 `mosaico.py recover` 安装基础包；不依赖跨代 Iris 通信，不全片擦除。
4. 核验同一 Device ID、新 Boot ID、Vibe Mode 版本、ABI、布局和维护能力。
5. 重新生成或迁移正常应用到公共集成入口，完整重建，通过默认产品流程安装。
6. 分别验证正常应用 → Vibe Mode → 正常应用、GPIO7、崩溃恢复、原生
   `idf.py flash monitor` 和可用的 JTAG 接口。

第三方 ESP-IDF 示例可以使用自己的分区表和 stock bootloader 原生烧录，不要求
接入 Iris。其清单可能覆盖产品维护区和数据；回到 Mosaico 时重新 recover，
不能承诺恢复被第三方覆盖的数据。旧包和旧工程仅保留为离线资料。
