import { deviceStateLabel } from "./deviceState";
import {
  PointerEvent as ReactPointerEvent,
  useEffect,
  useRef,
  useState,
} from "react";
import { api, formatBootId, formatBytes, formatTime } from "./api";
import LogsPanel from "./LogsPanel";
import ConsoleInput from "./ConsoleInput";
import AudioControls from "./AudioControls";
import OperationProgress from "./OperationProgress";
import { firmwareModeLabel } from "./Shell";
import type { Device, DeviceStatus, GatewayEvent, Operation } from "./types";

type Props = {
  device?: Device;
  status: DeviceStatus | null;
  mode: "develop" | "observe";
  operations: Operation[];
  events: GatewayEvent[];
  refresh: () => Promise<void>;
  systemUpdateTrustConfigured: boolean;
  onOpenRecords: () => void;
};

type Dialog =
  | "rpc"
  | "raw"
  | "restart"
  | "factory"
  | "ota"
  | "system_update"
  | "job"
  | null;
type RpcMethod = {
  name: string;
  service_id: number;
  method_id: number;
  timeout_ms: number;
};
type OtaValidationMode = "elf_sha256" | "version";

export default function Workspace({
  device,
  status,
  mode,
  operations,
  events,
  refresh,
  systemUpdateTrustConfigured,
  onOpenRecords,
}: Props) {
  const [dialog, setDialog] = useState<Dialog>(null);
  const [busy, setBusy] = useState(false);
  const [notice, setNotice] = useState("");
  const [rpcParams, setRpcParams] = useState("{}");
  const [rpcMethods, setRpcMethods] = useState<RpcMethod[]>([]);
  const [rpcMethod, setRpcMethod] = useState("system.info");
  const [rawIds, setRawIds] = useState({
    service: "1",
    method: "1",
    payload: "",
  });
  const [jobId, setJobId] = useState("1");
  const [otaFiles, setOtaFiles] = useState<{
    bin: File | null;
    elf: File | null;
    map: File | null;
  }>({ bin: null, elf: null, map: null });
  const [otaExecutionMode, setOtaExecutionMode] = useState<
    "recovery" | "application"
  >("recovery");
  const [otaValidationMode, setOtaValidationMode] =
    useState<OtaValidationMode>("elf_sha256");
  const [systemUpdateBundle, setSystemUpdateBundle] = useState<File | null>(
    null,
  );
  const logsDisclosure = useRef<HTMLDetailsElement>(null);
  const advancedActions = useRef<HTMLDetailsElement>(null);

  function openAdvancedDialog(next: Exclude<Dialog, null>) {
    advancedActions.current?.removeAttribute("open");
    setDialog(next);
  }
  const deviceOperations = operations.filter(
    (item) => item.device_id === device?.device_id,
  );
  const disabled = mode === "observe" || !device?.connected;
  const systemUpdateReady = Boolean(
    device?.capability_names?.includes("system_inventory"),
  );

  useEffect(() => {
    api<{ methods?: RpcMethod[] }>("/v2/rpc-catalog")
      .then((value) => {
        const methods = value.methods || [];
        setRpcMethods(methods);
        if (methods.length)
          setRpcMethod((current) =>
            methods.some((method) => method.name === current)
              ? current
              : methods[0].name,
          );
      })
      .catch(() => setRpcMethods([]));
  }, []);

  async function call(path: string, init: RequestInit, success: string) {
    if (!device) return;
    setBusy(true);
    setNotice("");
    try {
      await api(path, init);
      setNotice(success);
      setDialog(null);
      await refresh();
    } catch (reason) {
      setNotice(reason instanceof Error ? reason.message : String(reason));
    } finally {
      setBusy(false);
    }
  }

  async function submitOta(deviceId: string) {
    if (!otaFiles.bin || !otaFiles.elf || !otaFiles.map) return;
    setBusy(true);
    setNotice("");
    try {
      const form = new FormData();
      form.append("bin", otaFiles.bin);
      form.append("elf", otaFiles.elf);
      form.append("map", otaFiles.map);
      const archived = await api<{ artifact: { artifact_id: string } }>(
        "/v2/firmware-artifacts",
        { method: "POST", body: form },
      );
      const accepted = await api<{ operation: { operation_id: string } }>(
        `/v2/devices/${deviceId}/ota`,
        {
          method: "POST",
          body: JSON.stringify({
            artifact_id: archived.artifact.artifact_id,
            execution_mode: otaExecutionMode,
            validation_mode: otaValidationMode,
          }),
          headers: { "Content-Type": "application/json" },
        },
      );
      setNotice(
        `OTA 已受理 · ${accepted.operation.operation_id}，请在时间线查看进度`,
      );
      setDialog(null);
      await refresh();
    } catch (reason) {
      setNotice(reason instanceof Error ? reason.message : String(reason));
    } finally {
      setBusy(false);
    }
  }

  async function submitSystemUpdate(deviceId: string) {
    if (!systemUpdateBundle) return;
    setBusy(true);
    setNotice("");
    try {
      const accepted = await api<{ operation: { operation_id: string } }>(
        `/v2/devices/${deviceId}/system-update`,
        {
          method: "POST",
          body: systemUpdateBundle,
          headers: {
            "Content-Type": "application/vnd.esp-iris.system-update+zip",
          },
        },
      );
      setNotice(
        `系统更新已受理 · ${accepted.operation.operation_id}，请在时间线查看闭环校验`,
      );
      setDialog(null);
      await refresh();
    } catch (reason) {
      setNotice(reason instanceof Error ? reason.message : String(reason));
    } finally {
      setBusy(false);
    }
  }

  function submitDialog() {
    if (!device) return;
    const id = encodeURIComponent(device.device_id);
    if (dialog === "rpc") {
      let params: unknown;
      try {
        params = JSON.parse(rpcParams);
      } catch {
        setNotice("RPC 参数必须是有效 JSON");
        return;
      }
      call(
        `/v2/devices/${id}/rpc/${encodeURIComponent(rpcMethod)}`,
        {
          method: "POST",
          body: JSON.stringify({ params }),
          headers: { "Content-Type": "application/json" },
        },
        "RPC 已完成",
      );
    } else if (dialog === "raw") {
      call(
        `/v2/devices/${id}/rpc/raw`,
        {
          method: "POST",
          body: JSON.stringify({
            service_id: Number(rawIds.service),
            method_id: Number(rawIds.method),
            payload_text: rawIds.payload,
          }),
          headers: { "Content-Type": "application/json" },
        },
        "原始 RPC 已完成",
      );
    } else if (dialog === "restart") {
      call(
        `/v2/devices/${id}/restart`,
        {
          method: "POST",
          body: JSON.stringify({ delay_ms: 250 }),
          headers: { "Content-Type": "application/json" },
        },
        "重启请求已发送",
      );
    } else if (dialog === "factory") {
      call(
        `/v2/devices/${id}/factory-recovery`,
        { method: "POST" },
        "设备已进入 factory recovery",
      );
    } else if (dialog === "job") {
      call(
        `/v2/devices/${id}/jobs/${Number(jobId)}`,
        { method: "DELETE" },
        "Job 取消请求已发送",
      );
    } else if (dialog === "ota") {
      void submitOta(id);
    } else if (dialog === "system_update") {
      void submitSystemUpdate(id);
    }
  }

  if (!device)
    return (
      <div className="workspace-empty">
        <strong>等待设备</strong>
        <span>连接 ESP-Iris Normal 或启动 --demo</span>
      </div>
    );

  const activeOperation = deviceOperations.find(
    (operation) =>
      ![
        "succeeded",
        "failed",
        "cancelled",
        "interrupted",
        "outcome_unknown",
      ].includes(operation.status),
  );

  return (
    <div className="workspace-page">
      <section className="workspace-main">
        <div className="device-heading">
          <div>
            <h1>
              {device.alias ||
                device.suggested_alias ||
                device.device_id.slice(-12)}
            </h1>
            <span className="mono-id">{device.device_id}</span>
            {device.hardware_mac && (
              <span className="mono-id">MAC {device.hardware_mac}</span>
            )}
          </div>
          <div className="heading-status">
            <span
              className={`connection-label ${device.connected ? "connected" : ""}`}
            >
              <i
                className={`status-dot ${device.state}`}
              />
              {deviceStateLabel(device.state)}
            </span>
            <span>
              {device.project_name || "项目未知"} {device.app_version || ""}
            </span>
            <span>
              {device.transport_name || device.endpoint || "传输未知"}
            </span>
            <span>{firmwareModeLabel(device.firmware_mode)}</span>
            <span>控制：{device.connected && (device.control_link || device.demo) ? "已连接" : "未连接"}</span>
            <span>数据：{device.connected && (device.data_available || device.demo) ? "已连接" : "未连接"}</span>
          </div>
        </div>
        {mode === "observe" && (
          <div className="observe-banner">
            <strong>观察模式</strong>
            <span>
              所有业务设备请求已停止；以下显示缓存状态和设备主动上报。
            </span>
          </div>
        )}
        {(device.cached || status?.stale) && (
          <div className="stale-flag">缓存状态 · 数据可能已过期</div>
        )}
        <div className="metric-strip">
          <Metric
            label="运行时间"
            value={
              status?.uptime_us
                ? `${(status.uptime_us / 1e6).toFixed(0)} s`
                : "—"
            }
          />
          <Metric
            label="可用内部堆"
            value={formatBytes(status?.free_internal)}
            detail={`最低 ${formatBytes(status?.min_free_internal)}`}
          />
          <Metric
            label="日志丢失"
            value={formatBytes(status?.log_dropped_bytes)}
            tone={status?.log_dropped_bytes ? "warn" : "good"}
          />
          <Metric
            label="时钟误差"
            value={
              status?.clock_uncertainty_us != null
                ? `±${status.clock_uncertainty_us.toFixed(0)} µs`
                : "—"
            }
          />
          <details className="device-details">
            <summary>设备信息</summary>
            <dl>
              <dt>生命周期</dt>
              <dd>{formatLifecycle(status?.lifecycle_state)}</dd>
              <dt>堆占用</dt>
              <dd>
                {formatBytes(status?.heap_used)} /{" "}
                {formatBytes(status?.heap_total)}
              </dd>
              <dt>Boot ID</dt>
              <dd>{formatBootId(device)}</dd>
              <dt>设备能力</dt>
              <dd>{device.capability_names?.join(" · ") || "—"}</dd>
            </dl>
          </details>
        </div>
        <div className="action-bar">
          <button
            className="primary-button"
            disabled={disabled || busy}
            onClick={() =>
              call(
                `/v2/devices/${encodeURIComponent(device.device_id)}/rpc/system.info`,
                {
                  method: "POST",
                  body: JSON.stringify({ params: {} }),
                  headers: { "Content-Type": "application/json" },
                },
                "系统信息已读取",
              )
            }
          >
            读取系统信息
          </button>
          <button disabled={disabled} onClick={() => setDialog("rpc")}>
            调用 RPC…
          </button>
          <button disabled={disabled || !(device.console_available ?? (device.control_link || device.demo))} onClick={() => {
            if (logsDisclosure.current) logsDisclosure.current.open = true;
            document.getElementById(`console-input-${device.device_id}`)?.focus();
          }}>
            输入命令
          </button>
          <span className="queue-info">
            队列 {status?.queue?.queued.length ?? 0} · 运行{" "}
            {status?.queue?.running.length ?? 0}
          </span>
          <details ref={advancedActions} className="advanced-actions">
            <summary>更多操作</summary>
            <div>
              <button
                disabled={disabled}
                onClick={() => openAdvancedDialog("raw")}
              >
                原始 RPC
              </button>
              <button
                disabled={disabled}
                onClick={() => openAdvancedDialog("job")}
              >
                取消 Job
              </button>
              <button
                disabled={disabled || (!device.data_available && !device.demo)}
                title={!device.data_available && !device.demo ? "固件传输需要数据链路" : undefined}
                onClick={() => openAdvancedDialog("ota")}
              >
                OTA 更新
              </button>
              <button
                className="danger-outline"
                disabled={disabled || !systemUpdateReady || (!device.data_available && !device.demo)}
                title={
                  systemUpdateReady
                    ? systemUpdateTrustConfigured
                      ? "上传签名 .irisfw 系统更新包"
                      : "上传无签名 .irisfw 系统更新包（无发布者认证）"
                    : "设备未提供只读 System Inventory"
                }
                onClick={() => openAdvancedDialog("system_update")}
              >
                系统更新包
              </button>
              <button
                className="danger-outline"
                disabled={disabled}
                onClick={() => openAdvancedDialog("factory")}
              >
                Factory Recovery
              </button>
              <button
                className="danger-outline"
                disabled={disabled}
                onClick={() => openAdvancedDialog("restart")}
              >
                重启设备
              </button>
            </div>
          </details>
        </div>
        {notice && <div className="inline-notice">{notice}</div>}
        {activeOperation && (
          <div className="active-operation">
            <span>当前操作</span>
            <strong>{actionLabel(activeOperation.action)}</strong>
            <em className={`op-status ${activeOperation.status}`}>
              {activeOperation.status}
            </em>
            <OperationProgress operation={activeOperation} />
          </div>
        )}
        <DeviceScreen device={device} mode={mode} events={events} />
        <RecentOperations
          operations={deviceOperations}
          onOpenRecords={onOpenRecords}
        />
        <details className="logs-disclosure" ref={logsDisclosure} open>
          <summary>
            设备日志{" "}
            <span>
              {
                events.filter(
                  (item) =>
                    item.category === "log" &&
                    item.device_id === device.device_id,
                ).length
              }{" "}
              条
            </span>
          </summary>
          <LogsPanel events={events} deviceId={device.device_id} compact />
          <ConsoleInput deviceId={device.device_id} disabledReason={mode === "observe" ? "观察模式不能发送命令"
            : !device.connected ? "设备未连接，不能发送命令"
            : !(device.console_available ?? (device.control_link || device.demo)) ? "控制链路未连接，不能发送文本命令" : ""} />
        </details>
      </section>
      {dialog && (
        <ActionDialog
          dialog={dialog}
          busy={busy}
          onClose={() => setDialog(null)}
          onSubmit={submitDialog}
          rpcParams={rpcParams}
          setRpcParams={setRpcParams}
          rpcMethods={rpcMethods}
          rpcMethod={rpcMethod}
          setRpcMethod={setRpcMethod}
          rawIds={rawIds}
          setRawIds={setRawIds}
          jobId={jobId}
          setJobId={setJobId}
          otaFiles={otaFiles}
          setOtaFiles={setOtaFiles}
          otaExecutionMode={otaExecutionMode}
          setOtaExecutionMode={setOtaExecutionMode}
          otaValidationMode={otaValidationMode}
          setOtaValidationMode={setOtaValidationMode}
          systemUpdateBundle={systemUpdateBundle}
          setSystemUpdateBundle={setSystemUpdateBundle}
        />
      )}
    </div>
  );
}

function Metric({
  label,
  value,
  detail,
  tone,
}: {
  label: string;
  value: string;
  detail?: string;
  tone?: string;
}) {
  return (
    <div className="metric-cell">
      <span>{label}</span>
      <strong className={tone}>{value}</strong>
      {detail && <small>{detail}</small>}
    </div>
  );
}

function RecentOperations({
  operations,
  onOpenRecords,
}: {
  operations: Operation[];
  onOpenRecords: () => void;
}) {
  const recent = operations.slice(0, 3);
  return (
    <section className="recent-operations">
      <header>
        <h2>最近操作</h2>
        <button onClick={onOpenRecords}>查看全部</button>
      </header>
      {recent.length ? (
        <div className="recent-list">
          {recent.map((operation) => (
            <button key={operation.operation_id} onClick={onOpenRecords}>
              <strong>{actionLabel(operation.action)}</strong>
              <span>{operation.actor_name}</span>
              <em className={`op-status ${operation.status}`}>
                {operation.status}
              </em>
              <time>{formatTime(operation.created_ns)}</time>
            </button>
          ))}
        </div>
      ) : (
        <p className="compact-empty">尚无设备操作</p>
      )}
    </section>
  );
}

function formatLifecycle(value?: string) {
  const labels: Record<string, string> = {
    "0": "已停止",
    "1": "启动中",
    "2": "运行中",
    "3": "停止中",
    "4": "失败",
    stopped: "已停止",
    starting: "启动中",
    running: "运行中",
    stopping: "停止中",
    failed: "失败",
    recovery: "恢复模式",
  };
  return value == null ? "—" : labels[String(value)] || `未知（${value}）`;
}

function DeviceScreen({
  device,
  mode,
  events,
}: {
  device: Device;
  mode: "develop" | "observe";
  events: GatewayEvent[];
}) {
  const disabled = mode === "observe" || !device.connected;
  const alive = useRef(true);
  const ownsMirror = useRef(false);
  const [image, setImage] = useState<string>("");
  useEffect(() => {
    alive.current = true;
    return () => {
      alive.current = false;
      if (ownsMirror.current) void api(`/v2/devices/${encodeURIComponent(device.device_id)}/mirror/stop`, {
        method: "POST", body: JSON.stringify({ channel: "screen" }),
      }).catch(() => undefined);
    };
  }, [device.device_id]);
  useEffect(() => () => { if (image.startsWith("blob:")) URL.revokeObjectURL(image); }, [image]);
  const [mirroring, setMirroring] = useState(false);
  const [streamKind, setStreamKind] = useState<"none" | "raw" | "encoded">(
    "none",
  );
  const [frameDescription, setFrameDescription] =
    useState<MediaDescription | null>(null);
  const [screenBusy, setScreenBusy] = useState(false);
  const [screenNotice, setScreenNotice] = useState("");
  const [inputEnabled, setInputEnabled] = useState(false);
  const screenCanvas = useRef<HTMLCanvasElement | null>(null);
  const screenImage = useRef<HTMLImageElement | null>(null);
  const gesture = useRef<{
    begin: { x: number; y: number };
    moves: { x: number; y: number }[];
  } | null>(null);
  const lastAgentInput = [...events]
    .reverse()
    .find(
      (item) =>
        item.device_id === device.device_id && item.operation?.actor_type === "agent" &&
        item.operation.action === "input.gesture",
    );

  useEffect(() => {
    if (!mirroring) return;
    let currentUrl = "";
    const protocol = location.protocol === "https:" ? "wss:" : "ws:";
    const socket = new WebSocket(
      `${protocol}//${location.host}/v2/devices/${encodeURIComponent(device.device_id)}/streams/screen`,
    );
    socket.binaryType = "arraybuffer";
    socket.onmessage = (message) => {
      const bytes = new Uint8Array(message.data as ArrayBuffer);
      if (bytes.length < 4) return;
      const metadataLength = new DataView(bytes.buffer).getUint32(0, true);
      if (metadataLength > bytes.length - 4) return;
      const metadata = JSON.parse(
        new TextDecoder().decode(bytes.slice(4, 4 + metadataLength)),
      ) as { description?: MediaDescription };
      const data = bytes.slice(4 + metadataLength);
      const description = metadata.description;
      if (description?.format === 1 || description?.format === 2) {
        const canvas = screenCanvas.current;
        if (!canvas) return;
        drawRawTile(canvas, frameDescription, description, data);
        setStreamKind("raw");
      } else {
        const contentType =
          description?.format === 3 ? "image/jpeg" : "image/png";
        const nextUrl = URL.createObjectURL(
          new Blob([data], { type: contentType }),
        );
        setImage(nextUrl);
        setStreamKind("encoded");
        if (currentUrl) URL.revokeObjectURL(currentUrl);
        currentUrl = nextUrl;
      }
    };
    return () => {
      socket.close();
      if (currentUrl) URL.revokeObjectURL(currentUrl);
    };
  }, [mirroring, device.device_id, frameDescription]);

  async function toggleMirror() {
    const next = !mirroring;
    setScreenBusy(true);
    setScreenNotice(next ? "正在启动镜像…" : "正在停止镜像…");
    try {
      const result = await api<{ mirror?: { description?: MediaDescription } }>(
        `/v2/devices/${encodeURIComponent(device.device_id)}/mirror/${next ? "start" : "stop"}`,
        {
          method: "POST",
          body: JSON.stringify({ channel: "screen", fps: 5, description: {} }),
          headers: { "Content-Type": "application/json" },
        },
      );
      ownsMirror.current = next;
      if (!alive.current) {
        if (next) await api(`/v2/devices/${encodeURIComponent(device.device_id)}/mirror/stop`, {
          method: "POST", body: JSON.stringify({ channel: "screen" }),
        });
        return;
      }
      setFrameDescription(next ? (result.mirror?.description ?? null) : null);
      setStreamKind("none");
      setMirroring(next);
      setScreenNotice("");
    } catch (reason) {
      setScreenNotice(
        reason instanceof Error ? reason.message : String(reason),
      );
    } finally {
      setScreenBusy(false);
    }
  }

  async function captureScreenshot() {
    setScreenBusy(true);
    setScreenNotice("正在读取并转换截图，请稍候…");
    try {
      const response = await fetch(
        `/v2/devices/${encodeURIComponent(device.device_id)}/screenshot?save=true`,
        {
          method: "POST",
          credentials: "same-origin",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({}),
        },
      );
      if (!response.ok) throw new Error("截图失败");
      const metadata = JSON.parse(
        response.headers.get("X-ESP-Iris-Media") || "{}",
      ) as MediaDescription;
      const reusedMirror = Boolean(metadata.mirror_reused);
      const blob = await response.blob();
      if (!alive.current) return;
      const nextUrl = URL.createObjectURL(blob);
      setImage((previous) => {
        if (previous.startsWith("blob:")) URL.revokeObjectURL(previous);
        return nextUrl;
      });
      setStreamKind("encoded");
      if (reusedMirror && !mirroring) {
        setFrameDescription({
          ...metadata,
          format: metadata.source_format ?? metadata.format,
          stride: metadata.source_stride ?? metadata.stride,
        });
        setStreamKind("none");
        setMirroring(true);
      }
      setScreenNotice(
        mirroring || reusedMirror ? "PNG 截图已完成，实时镜像保持运行" : "",
      );
    } catch (reason) {
      setScreenNotice(
        reason instanceof Error ? reason.message : String(reason),
      );
    } finally {
      setScreenBusy(false);
    }
  }

  function point(event: ReactPointerEvent<HTMLDivElement>, clamp = false) {
    const rect = event.currentTarget.getBoundingClientRect();
    const mediaWidth =
      streamKind === "raw"
        ? (frameDescription?.width ?? screenCanvas.current?.width ?? 0)
        : (screenImage.current?.naturalWidth ?? 0);
    const mediaHeight =
      streamKind === "raw"
        ? (frameDescription?.height ?? screenCanvas.current?.height ?? 0)
        : (screenImage.current?.naturalHeight ?? 0);
    return mapPointerToContainedMedia(
      event.clientX,
      event.clientY,
      {
        left: rect.left,
        top: rect.top,
        width: rect.width,
        height: rect.height,
      },
      { width: mediaWidth, height: mediaHeight },
      clamp,
    );
  }

  async function pointerUp(event: ReactPointerEvent<HTMLDivElement>) {
    if (disabled || !gesture.current) return;
    const end = point(event, true);
    const current = gesture.current;
    gesture.current = null;
    if (!end) return;
    const value = {
      type: "pointer",
      begin: current.begin,
      moves: current.moves,
      end,
    };
    await api(`/v2/devices/${encodeURIComponent(device.device_id)}/input`, {
      method: "POST",
      body: JSON.stringify(value),
      headers: { "Content-Type": "application/json" },
    }).catch((error) => setScreenNotice(String(error)));
  }

  return (
    <section className="screen-pane">
      <div className="panel-title">
        <span>设备画面</span>
        <small>
          {screenBusy ? "处理中…" : mirroring ? "镜像中" : "未启动"}
        </small>
        <div>
          <button
            disabled={disabled || screenBusy}
            onClick={captureScreenshot}
          >
            截图
          </button>
          <button
            disabled={disabled || screenBusy || (!device.data_available && !device.demo)}
            title={!device.data_available && !device.demo ? "连续画面需要数据链路；仍可按需截图" : undefined}
            onClick={toggleMirror}
          >
            {mirroring ? "停止镜像" : "启动镜像"}
          </button>
          <button
            disabled={disabled}
            className={inputEnabled ? "active-control" : ""}
            onClick={() => setInputEnabled((value) => !value)}
          >
            交互输入
          </button>
        </div>
      </div>
      {!device.data_available && !device.demo && <div className="inline-notice">仅控制链路：支持按需截图和输入；连续画面、音频及文件传输需要数据链路。</div>}
      {screenNotice && <div className="inline-notice">{screenNotice}</div>}
      <div
        className={`screen-surface ${inputEnabled ? "input-enabled" : ""}`}
        onPointerDown={(event) => {
          if (inputEnabled && !disabled) {
            const begin = point(event);
            if (begin) {
              event.currentTarget.setPointerCapture(event.pointerId);
              gesture.current = { begin, moves: [] };
            }
          }
        }}
        onPointerMove={(event) => {
          if (gesture.current) {
            const move = point(event, true);
            if (move) gesture.current.moves.push(move);
          }
        }}
        onPointerUp={pointerUp}
        onPointerCancel={() => {
          gesture.current = null;
        }}
      >
        <canvas
          ref={screenCanvas}
          className={streamKind === "raw" ? "" : "hidden"}
          aria-label="设备实时画面"
        />
        {streamKind !== "raw" && image ? (
          <img ref={screenImage} src={image} alt="设备实时画面" />
        ) : (
          streamKind !== "raw" && (
            <div className="screen-placeholder">
              <strong>屏幕未启动</strong>
              <small>点击“启动镜像”查看设备画面</small>
            </div>
          )
        )}
        {lastAgentInput && (
          <div className="agent-pointer">
            <i />
            Agent 输入
          </div>
        )}
        {inputEnabled && (
          <div className="input-overlay-label">INPUT CAPTURE</div>
        )}
      </div>
      {device.capability_names?.includes("audio") && (device.data_available || device.demo) && (
        <AudioControls deviceId={device.device_id} mode={disabled ? "observe" : "develop"} />
      )}
    </section>
  );
}

type MediaDescription = {
  x?: number;
  y?: number;
  width?: number;
  height?: number;
  stride?: number;
  format?: number;
  mirror_reused?: number;
  source_format?: number;
  source_stride?: number;
};

type PointerBounds = {
  left: number;
  top: number;
  width: number;
  height: number;
};
type MediaSize = { width: number; height: number };

export function mapPointerToContainedMedia(
  clientX: number,
  clientY: number,
  bounds: PointerBounds,
  media: MediaSize,
  clamp = false,
) {
  if (
    bounds.width <= 0 ||
    bounds.height <= 0 ||
    media.width <= 0 ||
    media.height <= 0
  )
    return null;
  const scale = Math.min(
    1,
    bounds.width / media.width,
    bounds.height / media.height,
  );
  const width = media.width * scale;
  const height = media.height * scale;
  const left = bounds.left + (bounds.width - width) / 2;
  const top = bounds.top + (bounds.height - height) / 2;
  const normalizedX = (clientX - left) / width;
  const normalizedY = (clientY - top) / height;
  if (
    !clamp &&
    (normalizedX < 0 || normalizedX > 1 || normalizedY < 0 || normalizedY > 1)
  )
    return null;
  const boundedX = Math.max(0, Math.min(1, normalizedX));
  const boundedY = Math.max(0, Math.min(1, normalizedY));
  return { x: Math.round(boundedX * 10000), y: Math.round(boundedY * 10000) };
}

function drawRawTile(
  canvas: HTMLCanvasElement,
  frame: MediaDescription | null,
  tile: MediaDescription,
  data: Uint8Array,
) {
  const format = tile.format;
  const bytesPerPixel = format === 1 ? 2 : format === 2 ? 3 : 0;
  const width = tile.width ?? 0;
  const height = tile.height ?? 0;
  const stride = tile.stride ?? width * bytesPerPixel;
  const canvasWidth =
    frame?.width ?? Math.max(width + (tile.x ?? 0), canvas.width);
  const canvasHeight =
    frame?.height ?? Math.max(height + (tile.y ?? 0), canvas.height);
  if (
    !bytesPerPixel ||
    !width ||
    !height ||
    stride < width * bytesPerPixel ||
    data.length !== stride * height
  )
    return;
  if (canvas.width !== canvasWidth || canvas.height !== canvasHeight) {
    canvas.width = canvasWidth;
    canvas.height = canvasHeight;
  }
  const context = canvas.getContext("2d");
  if (!context) return;
  const pixels = context.createImageData(width, height);
  let output = 0;
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const source = y * stride + x * bytesPerPixel;
      if (format === 1) {
        const value = data[source] | (data[source + 1] << 8);
        const red = (value >> 11) & 0x1f;
        const green = (value >> 5) & 0x3f;
        const blue = value & 0x1f;
        pixels.data[output++] = (red << 3) | (red >> 2);
        pixels.data[output++] = (green << 2) | (green >> 4);
        pixels.data[output++] = (blue << 3) | (blue >> 2);
      } else {
        pixels.data[output++] = data[source];
        pixels.data[output++] = data[source + 1];
        pixels.data[output++] = data[source + 2];
      }
      pixels.data[output++] = 255;
    }
  }
  context.putImageData(pixels, tile.x ?? 0, tile.y ?? 0);
}

type ActionProps = {
  dialog: Exclude<Dialog, null>;
  busy: boolean;
  onClose: () => void;
  onSubmit: () => void;
  rpcParams: string;
  setRpcParams: (value: string) => void;
  rpcMethods: RpcMethod[];
  rpcMethod: string;
  setRpcMethod: (value: string) => void;
  rawIds: { service: string; method: string; payload: string };
  setRawIds: (value: {
    service: string;
    method: string;
    payload: string;
  }) => void;
  jobId: string;
  setJobId: (value: string) => void;
  otaFiles: { bin: File | null; elf: File | null; map: File | null };
  setOtaFiles: (value: {
    bin: File | null;
    elf: File | null;
    map: File | null;
  }) => void;
  otaExecutionMode: "recovery" | "application";
  setOtaExecutionMode: (value: "recovery" | "application") => void;
  otaValidationMode: OtaValidationMode;
  setOtaValidationMode: (value: OtaValidationMode) => void;
  systemUpdateBundle: File | null;
  setSystemUpdateBundle: (value: File | null) => void;
};

function ActionDialog(props: ActionProps) {
  const dangerous = [
    "raw",
    "restart",
    "factory",
    "ota",
    "system_update",
    "job",
  ].includes(props.dialog);
  const titles = {
    rpc: "调用 RPC",
    raw: "发送原始 RPC",
    restart: "确认重启设备",
    factory: "进入 Factory Recovery",
    ota: "执行 OTA 更新",
    system_update: "执行系统更新",
    job: "取消设备 Job",
  };
  const otaReady = Boolean(
    props.otaFiles.bin && props.otaFiles.elf && props.otaFiles.map,
  );
  return (
    <div
      className="dialog-backdrop"
      role="presentation"
      onMouseDown={props.onClose}
    >
      <section
        className="action-dialog"
        role="dialog"
        aria-modal="true"
        onMouseDown={(event) => event.stopPropagation()}
      >
        <div className="dialog-heading">
          <h2>{titles[props.dialog]}</h2>
          <button aria-label="关闭" onClick={props.onClose}>
            ×
          </button>
        </div>
        {props.dialog === "rpc" && (
          <>
            <label>
              方法
              <select
                value={props.rpcMethod}
                onChange={(event) => props.setRpcMethod(event.target.value)}
              >
                {props.rpcMethods.map((method) => (
                  <option key={method.name} value={method.name}>
                    {method.name} · {method.timeout_ms} ms
                  </option>
                ))}
              </select>
            </label>
            <label>
              JSON 参数
              <textarea
                value={props.rpcParams}
                onChange={(event) => props.setRpcParams(event.target.value)}
              />
            </label>
          </>
        )}
        {props.dialog === "raw" && (
          <>
            <div className="field-pair">
              <label>
                Service ID
                <input
                  value={props.rawIds.service}
                  onChange={(event) =>
                    props.setRawIds({
                      ...props.rawIds,
                      service: event.target.value,
                    })
                  }
                />
              </label>
              <label>
                Method ID
                <input
                  value={props.rawIds.method}
                  onChange={(event) =>
                    props.setRawIds({
                      ...props.rawIds,
                      method: event.target.value,
                    })
                  }
                />
              </label>
            </div>
            <label>
              原始载荷
              <textarea
                value={props.rawIds.payload}
                onChange={(event) =>
                  props.setRawIds({
                    ...props.rawIds,
                    payload: event.target.value,
                  })
                }
              />
            </label>
          </>
        )}
        {props.dialog === "job" && (
          <label>
            Job ID
            <input
              type="number"
              value={props.jobId}
              onChange={(event) => props.setJobId(event.target.value)}
            />
          </label>
        )}
        {props.dialog === "ota" && (
          <>
            <label>
              执行位置
              <select
                value={props.otaExecutionMode}
                onChange={(event) =>
                  props.setOtaExecutionMode(
                    event.target.value as "recovery" | "application",
                  )
                }
              >
                <option value="recovery">Factory Recovery（默认）</option>
                <option value="application">当前应用</option>
              </select>
            </label>
            <label>
              完成后校验
              <select
                value={props.otaValidationMode}
                onChange={(event) =>
                  props.setOtaValidationMode(
                    event.target.value as OtaValidationMode,
                  )
                }
              >
                <option value="elf_sha256">ELF SHA-256（默认）</option>
                <option value="version">项目版本号（兼容）</option>
              </select>
            </label>
            {(["bin", "elf", "map"] as const).map((kind) => (
              <label className="file-field" key={kind}>
                {kind.toUpperCase()} 文件
                <input
                  type="file"
                  accept={`.${kind},application/octet-stream`}
                  onChange={(event) =>
                    props.setOtaFiles({
                      ...props.otaFiles,
                      [kind]: event.target.files?.[0] || null,
                    })
                  }
                />
                <span>
                  {props.otaFiles[kind]?.name || `选择 .${kind} 文件`}
                </span>
              </label>
            ))}
          </>
        )}
        {props.dialog === "system_update" && (
          <>
            <p className="dialog-copy">
              Gateway 会按当前信任策略校验并保存原始包，再保全 Core Dump、进入
              Recovery、提交更新，最后校验新固件与实际 Flash 清单。
            </p>
            <label className="file-field">
              IRISFW 文件
              <input
                aria-label="选择系统更新包"
                type="file"
                accept=".irisfw,application/vnd.esp-iris.system-update+zip,application/zip"
                onChange={(event) =>
                  props.setSystemUpdateBundle(event.target.files?.[0] || null)
                }
              />
              <span>
                {props.systemUpdateBundle?.name || "选择 .irisfw 文件"}
              </span>
            </label>
          </>
        )}
        {props.dialog === "restart" && (
          <p className="dialog-copy">
            设备会在 250 ms 后重启；网关会继续等待设备重新连接。
          </p>
        )}
        {props.dialog === "factory" && (
          <p className="dialog-copy">
            设备将进入 Factory Recovery
            并重启。只有明确支持此能力的固件才能执行。
          </p>
        )}
        {dangerous && (
          <div className="confirm-warning">
            <span>!</span>
            <p>
              <strong>需要开发者确认</strong>
              <small>此操作会改变当前设备状态，请确认设备和操作无误。</small>
            </p>
          </div>
        )}
        <footer>
          <button onClick={props.onClose}>取消</button>
          <button
            className={dangerous ? "danger-button" : "primary-button"}
            disabled={
              props.busy ||
              (props.dialog === "ota" && !otaReady) ||
              (props.dialog === "system_update" && !props.systemUpdateBundle) ||
              (props.dialog === "rpc" && !props.rpcMethod)
            }
            onClick={props.onSubmit}
          >
            {props.busy ? "执行中…" : "确认执行"}
          </button>
        </footer>
      </section>
    </div>
  );
}

export function actionLabel(action: string) {
  const labels: Record<string, string> = {
    "rpc.raw": "原始 RPC",
    "console.execute": "发送 Console 命令",
    "device.restart": "设备重启",
    "recovery.enter_factory": "Factory Recovery",
    "firmware.ota": "OTA 更新",
    "firmware.system_update": "系统更新",
    "job.cancel": "取消 Job",
    "job.query": "查询 Job",
    "media.screenshot": "设备截图",
    "media.mirror_start": "启动镜像",
    "media.mirror_stop": "停止镜像",
    "input.gesture": "交互输入",
    "audio.upload": "音频上传",
  };
  return labels[action] || action;
}
