import { useEffect, useState } from "react";
import { formatBytes } from "./api";
import type { Operation } from "./types";

const phases: Record<string, string> = {
  preserving_evidence: "保存设备诊断记录",
  entering_recovery: "切换到 Vibe Mode",
  waiting_recovery: "等待 Vibe Mode 连接",
  recovery_connected: "建立更新连接",
  validating_plan: "校验更新计划",
  erasing: "准备写入空间",
  transferring: "传输并写入固件",
  verifying: "校验已写入内容",
  committing: "提交更新",
  waiting_device: "等待应用启动与健康确认",
  reconnecting: "重新连接设备",
};

export default function OperationProgress({ operation }: { operation: Operation }) {
  const progress = operation.progress;
  const waiting = Boolean(progress?.phase_indeterminate);
  const [now, setNow] = useState(Date.now);
  useEffect(() => {
    if (!waiting) return;
    const timer = window.setInterval(() => setNow(Date.now()), 1000);
    return () => window.clearInterval(timer);
  }, [waiting, progress?.updated_ns]);
  if (!progress) return null;
  const label = phases[progress.stage] || "正在处理更新";
  const elapsed = Math.max(0, Math.floor((now - (progress.updated_ns || operation.started_ns || operation.created_ns) / 1e6) / 1000));
  return (
    <div className="operation-progress" role="status" aria-live="polite">
      <div className="operation-phase">
        <strong>{label}</strong>
        {progress.component_count && progress.component_index !== undefined && (
          <span>组件 {progress.component_index + 1}/{progress.component_count}</span>
        )}
        {waiting && <span>已等待 {elapsed} 秒</span>}
        {!waiting && progress.bytes_total !== undefined && (
          <span>{formatBytes(progress.bytes_received || 0)} / {formatBytes(progress.bytes_total)}</span>
        )}
      </div>
      <progress aria-label={label} max={1000} value={waiting ? undefined : progress.progress_permille} />
      {progress.stage === "erasing" && <span>设备正在准备存储空间，完成后开始传输。</span>}
    </div>
  );
}
