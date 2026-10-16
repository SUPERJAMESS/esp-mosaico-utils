import { FormEvent, useEffect, useState } from "react";
import { api, formatBytes, formatDateTime } from "./api";
import { ExportJob, exportActive, exportOptions, exportStatus } from "./exportState";

export default function ExportPanel() {
  const [jobs, setJobs] = useState<ExportJob[]>([]);
  const [device, setDevice] = useState("");
  const [operation, setOperation] = useState("");
  const [since, setSince] = useState("");
  const [until, setUntil] = useState("");
  const [retain, setRetain] = useState(false);
  const [submitting, setSubmitting] = useState(false);
  const [error, setError] = useState("");
  async function refresh() {
    const result = await api<{ exports: ExportJob[] }>("/v2/exports");
    setJobs(result.exports);
  }
  useEffect(() => {
    let disposed = false;
    let timer: ReturnType<typeof setTimeout>;
    async function poll() {
      try {
        const result = await api<{ exports: ExportJob[] }>("/v2/exports");
        if (!disposed) setJobs(result.exports);
      } catch (reason) {
        if (!disposed) setError(String(reason));
      } finally {
        if (!disposed) timer = setTimeout(() => void poll(), 1000);
      }
    }
    void poll();
    return () => { disposed = true; clearTimeout(timer); };
  }, []);
  async function submit(event: FormEvent) {
    event.preventDefault();
    setSubmitting(true);
    setError("");
    try {
      const job = await api<ExportJob>("/v2/export", { method: "POST", body: JSON.stringify(exportOptions(device, operation, since, until, retain)) });
      setJobs((previous) => [job, ...previous.filter((item) => item.job_id !== job.job_id)]);
    } catch (reason) { setError(String(reason)); }
    finally { setSubmitting(false); }
  }
  async function action(job: ExportJob, cancel: boolean) {
    try {
      await api(`/v2/exports/${job.job_id}${cancel ? "/cancel" : ""}`, { method: cancel ? "POST" : "DELETE" });
      await refresh();
    } catch (reason) { setError(String(reason)); }
  }
  return <section className="settings-section settings-wide export-panel">
    <div className="panel-title"><span>证据导出</span></div>
    <p className="section-copy">留空导出当前项目的全部留存证据。可按设备、操作和时间缩小范围；操作导出包含该设备在操作期间的日志。</p>
    <form className="export-form" onSubmit={submit}>
      <label>设备 ID<input value={device} onChange={(event) => setDevice(event.target.value)} /></label>
      <label>操作 ID<input value={operation} onChange={(event) => setOperation(event.target.value)} /></label>
      <label>开始时间<input type="datetime-local" value={since} onChange={(event) => setSince(event.target.value)} /></label>
      <label>结束时间<input type="datetime-local" value={until} onChange={(event) => setUntil(event.target.value)} /></label>
      <label className="export-retain"><input type="checkbox" checked={retain} onChange={(event) => setRetain(event.target.checked)} />保留归档，直到手动删除</label>
      <button className="primary-button" disabled={submitting || jobs.some(exportActive)}>导出证据包 ZIP</button>
    </form>
    {error && <p role="alert" className="inline-notice">{error}</p>}
    <div className="export-jobs">{jobs.map((job) => <article className="export-job" key={job.job_id}>
      <div><strong role="status">{exportStatus(job)}</strong><small>{formatDateTime(job.created_ns)} · {job.files} 个文件 · 已处理 {formatBytes(job.input_bytes)}</small>
        {job.error && <p role="alert">{job.error}</p>}
        {job.status === "completed" && <small>{formatBytes(job.bytes_written)} · {job.retain ? "保留归档" : `到期清理：${formatDateTime(job.expires_ns)}`}</small>}
        {job.sha256 && <code title="SHA-256">{job.sha256}</code>}
      </div>
      <div className="settings-actions">
        {job.status === "completed" && <a href={job.download_url} download>下载证据包</a>}
        {exportActive(job) ? <button disabled={job.status === "cancelling"} onClick={() => void action(job, true)}>取消导出</button>
          : <button onClick={() => void action(job, false)}>删除导出包</button>}
      </div>
    </article>)}</div>
  </section>;
}
