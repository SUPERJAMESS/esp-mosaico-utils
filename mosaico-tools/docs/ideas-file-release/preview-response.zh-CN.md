# 单次请求的完整预览响应

输入编号后，一次成功 HTTP 请求必须取得显示完整详情所需的全部文件。本文修改
HTTP 传输表示，不改变上传 ZIP、device-release/1、display/1 或 NAND 目录格式。
取消、错误响应或校验失败不得写入 NAND，也不得回退到逐文件 GET 补齐预览。

## 请求与响应

```http
GET /api/v1/ideas/042731/manifest.json?profile=esp-mosaico%2Fv2&chip_id=32&flash_size=16777216&recovery_version=0.2.0&locale=zh-CN HTTP/1.1
Accept: multipart/related
Accept-Encoding: identity
```

设备能力的含义和兼容选版规则沿用主协议；locale 必须恰好一次，为 zh-CN 或 en。
语言只决定传输哪套展示资源，不改变选版结果、根清单或 release_id。成功返回
200、Cache-Control: no-store、Vary: Accept，以及 multipart/related 响应。
没有 resolve、重定向、预览 URL 查询或其他前置请求。Accept 最多一项，未知表示
返回 406 NOT_ACCEPTABLE；不支持带权重的媒体类型列表。

```http
Content-Type: multipart/related; type="application/json"; boundary="BOUNDARY"
Content-Length: <整个响应体字节数>
ETag: "<整个响应体的64位小写SHA-256>"
```

采用 [RFC 2387](https://www.rfc-editor.org/rfc/rfc2387.html) 的受限配置：type 固定
application/json，第一部分为根清单，不使用 start 参数。boundary 为 1–70 个
ASCII 字母、数字、下划线或连字符；参数顺序不重要，可使用引号。必须恰好包含
type 和 boundary，不得重复或携带未知参数。服务端确保所选边界不会与正文冲突。

请求超时沿用清单请求的 30 秒总预算。临时网络故障最多额外重试三次；一次成功
尝试中预览的 HTTP 请求数为 1，重试不等于追加依赖文件请求。每次重试清空上次
未完成响应的清单、资源和解析状态，不能混合两次尝试的字节。

## 部分顺序与约束

部分必须按以下顺序发送，恰好 4–6 部分：

1. manifest.json：发布的原始设备根清单。
2. firmware/manifest.json：根清单引用的原始固件安装清单。
3. device/所选语言/display.json：该语言的原始展示描述。
4. 该语言 presentations[].assets 的所有文件，严格按数组顺序传输。

每部分恰好三个头，名称不区分大小写，顺序不限，不得重复或增加其他头：

```http
--BOUNDARY
Content-Type: application/json
Content-Location: manifest.json
Content-Length: <本部分正文的正整数字节数>

<原始文件字节>
```

示意中的换行在实际传输中必须是 CRLF。前三部分的 Content-Type 固定为
application/json；字体和图标部分固定为 application/octet-stream，直接传输
二进制，不使用 Base64 或 Content-Transfer-Encoding。Content-Location 是集合
内精确路径，不是下载 URL。每条头行最多 255 字节（含 CRLF）。

第一部分前是 `--BOUNDARY\r\n`。每部分头以 `\r\n\r\n` 结束；正文之后是
`\r\n--BOUNDARY\r\n`，末尾则为 `\r\n--BOUNDARY--\r\n`。不接受 preamble、
epilogue、嵌套 multipart、多余部分、未知文件、另一语言文件或固件组件二进制。
解析器依据已验证清单中的长度读取正文，并核对随后边界，不能把二进制中的
任意换行当作文件结束。

| 范围 | 字节上限 |
| --- | ---: |
| 根清单 | 65536 |
| 固件清单 | 3072 |
| 当前语言 display、字体、图标合计 | 1048576 |
| 所有部分头、边界及分隔 CRLF 合计 | 4096 |
| 整个响应体 | 1121280 |

display 的 32 KiB、图标的 64 KiB/64×64 等内容规则继续执行。完整集合仍必须
包含两种语言，且满足 32 MiB/32 文件限制；预览响应只是其中可验证的固定子集。
HTTP Content-Length 可以省略，此时同样按实际流式字节计数；若提供必须准确。
每部分 Content-Length 必填，并须与清单声明一致；首部分额外受根清单上限约束。

## 完整性、缓存及显示

HTTP ETag 覆盖整个响应体，包括各部分头和边界。它不能作为根清单摘要使用。
设备另对第一部分原始字节计算 root_sha256，用于发布固定、同 release_id 内容
冲突检查及本地目录事务。不同语言或 MIME 边界的响应可以有不同 ETag，但同一
发布的 root_sha256 必须相同。根清单不包含自己的摘要；信任来源仍为 HTTPS/TLS。

根清单中的引用校验固件清单、display 和资源的实际长度及 SHA-256；解析固件
清单获得版本、完整组件索引及大小。随后执行目标兼容性、display、字体和图标
内容校验。完整响应及其全部文件校验成功后，才发布可确认的完整预览状态。
设备流式写入各文件的 PSRAM 预览缓存，无额外整包副本，不写 NAND。

用户确认后，直接保存根清单的原始字节，复用已校验固件清单与当前语言资源，
只请求固件组件及另一语言文件。不得重新请求 latest，也不得把 multipart 包装
保存成 manifest.json。取消释放全部预览缓存；退出预览后才能切换系统语言。

诊断工具可以对相同入口使用 application/json（省略 Accept 或使用 */* 同义），
此时必须省略 locale，响应为原始根清单，ETag 覆盖根清单字节。固定发布入口
只提供该 JSON 表示。它们不是旧格式兼容，也不构成设备预览的失败回退路径。

## 验收

- 中英文正常预览各只有一次 HTTP 请求；完整显示图标、文案、版本、大小。
- 确认前没有文件 GET、NAND 写入、固件安装或启动目标修改。
- 确认下载复用缓存，发布在预览后切换也不能混版。
- 单字节网络分片、头和边界跨读取块仍可正确解析。
- 截断、缺失、重复、额外部分、错误路径/语言/顺序/长度、错误摘要、超限、重复头
  和不支持的 MIME 参数均拒绝；校验失败不能追加 GET 修补响应。
- 中途取消及重试清空部分状态，随后正常请求能成功；下载后离线双语继续可用。
