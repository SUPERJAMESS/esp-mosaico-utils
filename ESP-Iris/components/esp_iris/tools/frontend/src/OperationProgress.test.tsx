import { describe, expect, it } from "vitest";
import { renderToStaticMarkup } from "react-dom/server";
import OperationProgress from "./OperationProgress";
import type { Operation } from "./types";

const operation: Operation = {
  operation_id: "update", device_id: "board", actor_type: "local", actor_name: "test",
  action: "firmware.system_update", status: "erasing", params: {}, created_ns: 1,
  progress: { stage: "erasing", progress_permille: 430, phase_indeterminate: true,
    component_index: 1, component_count: 2, bytes_received: 4096, bytes_total: 12582912 },
};

describe("update preparation feedback", () => {
  it("shows preparation and component identity without a made-up erase percentage", () => {
    const html = renderToStaticMarkup(<OperationProgress operation={operation} />);
    expect(html).toContain("准备写入空间");
    expect(html).toContain("组件 2/2");
    expect(html).toContain("完成后开始传输");
    expect(html).not.toContain('value="430"');
  });
  it("returns to measured progress when payload transfer starts", () => {
    const html = renderToStaticMarkup(<OperationProgress operation={{ ...operation,
      progress: { ...operation.progress!, stage: "transferring", phase_indeterminate: false,
        progress_permille: 510, bytes_received: 1048576 } }} />);
    expect(html).toContain("传输并写入固件");
    expect(html).toContain('value="510"');
    expect(html).not.toContain("完成后开始传输");
  });
});
