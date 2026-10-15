# Vibe Mode 在线固件发布与更新

设备联网后读取稳定版清单，发现新版本时显示版本号，以及“开始更新”和“跳过”按钮。
只有用户点击“开始更新”才下载、校验并写入固件，成功后自动重启到新 Vibe Mode。
确认页和整个更新过程都显示“请勿断电或关机”。更新期间不能返回其他页面或自动休眠。

## 地址与文件命名

固定目录：`https://dl.espressif.com/mosaico/`。

| 文件 | 命名规则 | 示例 |
| --- | --- | --- |
| 稳定版清单 | `esp-mosaico-vibe-mode-stable.json` | 地址始终不变 |
| Vibe Mode 应用镜像 | `esp-mosaico-vibe-mode-<MAJOR.MINOR.PATCH>.bin` | `esp-mosaico-vibe-mode-0.2.1.bin` |

正式版本使用三段数字，通常递增 PATCH。**同一个版本的 bin 发布后不再覆盖**；修复问题必须提升版本。
保留旧 bin，保证已读取旧清单的设备仍能完成下载。清单可以指向新的版本。
当前仅支持同一主版本内在线安装；跨主版本升级需要通过 USB/ROM 迁移。
不要将普通应用、bootloader、分区表、合并烧录镜像或 `.irisfw` 文件重命名为这个 bin。
设备需要的是 `project_name=vibe-mode` 的独立应用镜像，大小不能超过 1.75 MiB。

## 清单格式

```json
{
  "schema_version": 1,
  "product": "esp-mosaico-vibe-mode",
  "latest_version": "0.2.1",
  "title": "Vibe Mode 0.2.1",
  "summary": "本次版本的更新说明",
  "firmware": {
    "file": "esp-mosaico-vibe-mode-0.2.1.bin",
    "size": 1234567,
    "sha256": "替换为工具生成的64位小写十六进制SHA-256"
  }
}
```

上面大小和哈希仅展示字段含义，不能直接上传。使用下述工具自动生成真实值。
`latest_version` 必须与 bin 内部版本完全一致。`file` 是同目录文件名，不能包含 URL、路径、查询参数或重定向。
`size` 和 `sha256` 都针对实际上传的原始 bin；设备会自行补齐槽位，不需要手动填充 bin。
旧版只有版本信息的清单仍可用于检查，但没有完整 `firmware` 信息时不能开始安装。

## 构建与生成发布文件

以下命令以 `esp-mosaico-utils/esp-mosaico-vibe-mode/firmware/vibe-mode` 为工作目录。

1. 在已验证的 ESP-IDF 6.2 或更新环境中构建 ESP32-S31 工程。将 `CONFIG_APP_PROJECT_VER` 设置为本次发布版本，例如 `0.2.1`。已有构建目录以其中的实际 sdkconfig 为准，不能只修改 defaults 后假定版本已更新。
2. 构建并生成发布目录：

   ```sh
   idf.py -B build build
   python tools/prepare_online_release.py \
     --image build/vibe-mode.bin \
     --output-dir release/0.2.1
   ```

3. 工具从镜像读取版本，检查芯片、项目名称、镜像段、校验和、内嵌 SHA-256 和槽位大小，生成：

   ```text
   release/0.2.1/esp-mosaico-vibe-mode-0.2.1.bin
   release/0.2.1/esp-mosaico-vibe-mode-stable.json
   ```

   如果输出目录已有同名但内容不同的 bin，工具会拒绝覆盖。可以编辑清单中的 `title`、`summary`；不要手工改动版本、大小或哈希。
4. 在测试设备上验证新固件正常启动、Wi-Fi、维护接口和健康状态，再发布到稳定地址。

完整工程必须编译成功并通过实机验收；组件编译通过不能作为可发布固件。

## 上传顺序与服务器要求

1. **先上传 bin** 到 `https://dl.espressif.com/mosaico/esp-mosaico-vibe-mode-0.2.1.bin`。
2. 从公网地址重新下载该 bin，核对文件长度和 SHA-256 与生成清单一致。
3. **最后上传清单**，替换 `https://dl.espressif.com/mosaico/esp-mosaico-vibe-mode-stable.json`，并刷新该地址的 CDN 缓存。
4. 清单建议使用 `Cache-Control: no-cache`；版本化 bin 可以长期缓存。两个地址均应直接返回 HTTP 200，使用有效 HTTPS 证书，不经过登录页、重定向或内容转换。

不要先更新清单再上传 bin，否则设备可能发现一个尚无法下载的版本。设备限制清单为 4 KiB，单次清单请求超时为 10 秒。
固件下载每次读取超时 15 秒，总下载窗口约 180 秒；超时或断网不会自动重放擦写操作。

## 设备端安装过程

1. Wi-Fi 获取 IP 后自动检查；重连检查至少间隔 60 秒。
2. 检查到更高版本，在空闲首页提示。配网、其他下载及安装期间延后。同一版本每次启动只自动提示一次。
3. 用户点击“跳过”返回首页；点击“开始更新”后锁定固件写入事务，并阻止其他更新同时写 Flash。
4. 从固定目录下载清单指定 bin，完整保存在 PSRAM。校验实际长度、SHA-256、项目和版本，调用共享后端进行 ESP32-S31 镜像和布局检查。
5. 将镜像按 `0xff` 补齐至固定 Vibe Mode 槽位。当前分区表只用于一致性校验，不改 bootloader、分区表或普通应用。
6. 所有预检通过后才原地擦写 Vibe Mode，执行读回哈希及镜像验证，设置启动目标并自动重启。
7. 重启后确认设备进入 Vibe Mode，版本正确且报告健康；仅收到下载成功或提交成功不能代替启动验收。

本实现使用独立的 `FIRMWARE` writer owner，该来源只能安装单个 `recovery` 组件，不允许通过清单写入普通应用、数据、bootloader 或分区表。
下载缓冲和后端暂存缓冲在校验阶段同时存在，需要两块各 1.75 MiB 的连续 PSRAM，另加网络和 UI 内存。

## 失败与恢复

- 下载、HTTP、文件长度、哈希或镜像检查失败：停止安装并显示失败结果，尚未擦写 Vibe Mode。检查网络和发布文件后，可重新进入 Vibe Mode检查更新。
- 固件写入失败：保持供电，根据日志判断是否需要 USB/ROM 恢复，不要把“返回”当作固件已恢复。
- 擦写阶段掉电：由于 Vibe Mode 仍是单副本，可能无法启动，需进入 ROM Download Mode，按工作区约定使用 `python mosaico.py recover --source current` 或已评审固件包恢复。
- 设备旧版没有此在线更新功能：先通过已有 USB/ROM 更新路径安装支持该功能的版本。

当前后端不支持 Flash 加密构建，也没有发布密钥签名校验；HTTPS 与 SHA-256 验证传输及文件一致性。
界面会阻止软件返回和休眠，但无法阻止拔电或硬件强制关机，因此必须持续展示供电警告。
