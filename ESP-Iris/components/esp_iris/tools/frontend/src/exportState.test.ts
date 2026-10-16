import { describe, expect, it } from "vitest";
import { ExportJob, exportActive, exportOptions, exportStatus } from "./exportState";

describe("evidence export", () => {
  it("keeps whole-project defaults and validates range before sending", () => {
    expect(exportOptions("", "", "", "", false)).toEqual({ retain: false });
    expect(exportOptions(" a ", " op ", "2026-10-16T10:00", "2026-10-16T11:00", true)).toMatchObject({ device_id: "a", operation_id: "op", retain: true });
    expect(() => exportOptions("", "", "invalid", "", false)).toThrow();
    expect(() => exportOptions("", "", "2026-10-16T11:00", "2026-10-16T10:00", false)).toThrow();
  });
  it("releases submission after cancellation, failure and restart", () => {
    for (const status of ["queued", "running", "cancelling"]) expect(exportActive({ status } as ExportJob)).toBe(true);
    for (const status of ["completed", "cancelled", "failed", "interrupted"]) expect(exportActive({ status } as ExportJob)).toBe(false);
    expect(exportStatus({ status: "interrupted" } as ExportJob)).toContain("重启中断");
  });
});
