export type ExportJob = {
  job_id: string; status: string; phase?: string; error?: string;
  input_bytes: number; bytes_written: number; files: number; retain: boolean;
  created_ns: number; expires_ns?: number; download_url: string; sha256?: string;
};

export function exportActive(job: ExportJob): boolean {
  return ["queued", "running", "cancelling"].includes(job.status);
}

export function exportOptions(device: string, operation: string, since: string, until: string, retain: boolean) {
  const timestamp = (value: string) => {
    const milliseconds = Date.parse(value);
    if (!Number.isFinite(milliseconds)) throw new Error("请输入有效的导出时间");
    return milliseconds * 1e6;
  };
  const sinceNs = since ? timestamp(since) : undefined;
  const untilNs = until ? timestamp(until) : undefined;
  if (sinceNs !== undefined && untilNs !== undefined && sinceNs > untilNs) throw new Error("开始时间不能晚于结束时间");
  return { device_id: device.trim() || undefined, operation_id: operation.trim() || undefined,
    since_ns: sinceNs, until_ns: untilNs, retain };
}

export function exportStatus(job: ExportJob): string {
  const labels: Record<string, string> = { queued: "等待导出", running: "正在导出", cancelling: "正在取消",
    completed: "证据包已就绪", cancelled: "已取消", failed: "导出失败", interrupted: "导出已被网关重启中断" };
  return labels[job.status] || job.status;
}
