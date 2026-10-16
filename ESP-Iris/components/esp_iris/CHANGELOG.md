# Changelog

All notable ESP-Iris component changes are documented in this file.

## Unreleased

- Keep project Gateway history in `.gateway/`, with host-local credentials and
  explicit offline relocation of existing data. Schema 200/201 upgrades to 202;
  rollback requires complete pre-upgrade state and credentials backups.
- Bound event segments, process logs, subscriber buffers, writer batches and
  operation concurrency; report missing history and preserve operation evidence
  independently of diagnostic retention.
- Change `POST /v2/export` to an HTTP 202 background job with scoped snapshots,
  progress, cancellation, checksums and expiring downloads. HTTP clients must
  poll the job then download its ZIP; the bundled Workbench supports this flow.

- Target 0.2: standard printable console, native monitor/reset/JTAG support,
  UART and product-owned ESP-IDF REPL integration.
- Independent USB CDC and TCP data sessions with device/boot/owner binding;
  continuous media, files and firmware use data while snapshots also support control.
- Preserve pre-handshake raw logs, capture offsets and explicit hardware-reset
  evidence; retain history, rotation and Follow through reconnects.
- Break with 0.1: wire version 2, `/v2` HTTP/WebSocket API, isolated fresh host
  state and documentation-only migration; no runtime compatibility layer.

- Include the prebuilt Web Workbench, so a source checkout can serve the UI
  without Node.js. Verify in CI that rebuilding matches the committed files.

## 0.1.0 - 2026-09-10

- Publish ESP-Iris from `esp-mosaico-utils` and align the component, Python
  Gateway, and private Workbench package versions at `0.1.0`.
- Move consecutive-crash attribution into Iris: persist a versioned image
  record, distinguish immediate reset reason from a latched crash loop, clear
  after stable runtime/health, and enter factory Recovery at the configurable
  threshold without overwriting the original Core Dump.
- Allow product backends and Gateway deployments to explicitly opt into
  unsigned System Update bundles while retaining manifest/component SHA-256,
  bounded streaming, inventory validation, and product-owned Flash policy.
- Add an opt-in, recovery-only System Update service with signed manifest
  transport, bounded multi-component streaming, product-owned Flash policy,
  an independently registrable read-only Flash inventory, Gateway closed-loop
  validation, fixed-range image padding, and signed `.irisfw` bundle tooling.

- Allow TCP, application USB CDC0, and USB Serial/JTAG to wait concurrently,
  with bounded HELLO_ACK arbitration and one active transport owner.
- Add a standalone FATFS-backed file-transfer example covering streamed
  upload/download, directory operations, rename, and safe deletion.
- Add public file-service, retained-crash recovery, and runtime lifecycle
  examples; extend media formats, pairing provisioning guidance, and OTA
  policy profiles.
- Allow cross-project OTA by default and add
  `CONFIG_ESP_IRIS_OTA_REQUIRE_PROJECT_NAME_MATCH` for products that require
  the target image to retain the running firmware project name.
- Keep Linux USB recovery-first OTA attached across product-string
  re-enumeration by preferring stable physical `by-path` endpoints and treating
  the old session's `ConnectionError` as transient while recovery reconnects.

- Initial public component release.
- Add bounded USB CDC0, USB Serial/JTAG and raw TCP transports.
- Add binary logging, status, RPC/jobs, media, pairing, crash evidence and OTA.
- Include the Python Developer Gateway and React workbench sources.
