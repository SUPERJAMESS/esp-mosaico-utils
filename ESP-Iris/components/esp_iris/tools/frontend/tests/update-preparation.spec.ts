import { expect, test } from "@playwright/test";

test("update preparation stays visible until measured transfer begins", async ({ page }) => {
  let preparing = true;
  const started = Date.now() * 1e6;
  await page.route("**/v2/operations", async (route) => {
    await route.fulfill({ json: { operations: [{
      operation_id: "preparation-ui-test", device_id: "demo-a1b2c3d4",
      actor_type: "local", actor_name: "test", action: "firmware.system_update",
      status: preparing ? "erasing" : "transferring", params: {}, created_ns: started,
      progress: { stage: preparing ? "erasing" : "transferring", updated_ns: started,
        progress_permille: preparing ? 430 : 510, phase_indeterminate: preparing,
        component_index: 1, component_count: 2, bytes_received: preparing ? 4096 : 1048576,
        bytes_total: 12582912 },
    }] } });
  });
  await page.goto("/");
  if (await page.getByLabel("开发口令").isVisible().catch(() => false)) {
    await page.getByRole("button", { name: "进入工作台" }).click();
  }
  await page.locator(".device-select").filter({ hasText: "Iris Alpha" }).click();
  const feedback = page.locator(".operation-progress");
  await expect(feedback.getByText("准备写入空间", { exact: true })).toBeVisible();
  await expect(feedback.getByText("组件 2/2")).toBeVisible();
  await expect(feedback.locator("progress")).not.toHaveAttribute("value");
  await expect(feedback).toContainText("完成后开始传输");
  await expect(feedback).toContainText(/已等待 [1-9]\d* 秒/);
  await page.screenshot({ path: test.info().outputPath("preparing.png") });
  preparing = false;
  await expect(feedback.getByText("传输并写入固件", { exact: true })).toBeVisible();
  await expect(feedback.locator("progress")).toHaveAttribute("value", "510");
  await expect(feedback).not.toContainText("完成后开始传输");
});
