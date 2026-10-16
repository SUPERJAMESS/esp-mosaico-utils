import { expect, test } from "@playwright/test";

test("files distinguish missing service from missing data and react to link changes", async ({ page }) => {
  let service = false;
  let data = false;
  let requests = 0;
  const deviceId = "multichip-device";
  const device = () => ({
    device_id: deviceId, alias: "UART device", connected: true, demo: false,
    chip_target: "esp32c2", data_available: data,
    capability_names: service ? ["files"] : [],
  });
  await page.route("**/v2/devices", (route) => route.fulfill({ json: { devices: [device()], demo: false } }));
  await page.route(`**/v2/devices/${deviceId}`, (route) => route.fulfill({ json: device() }));
  await page.route(`**/v2/devices/${deviceId}/files/volumes`, (route) => {
    requests += 1;
    return route.fulfill({ json: { volumes: [] } });
  });
  await page.route("**/v2/mode", (route) => route.fulfill({ json: { mode: "develop", transitioning: false } }));
  await page.goto("/files");
  if (await page.getByLabel("开发口令").isVisible().catch(() => false)) {
    await page.getByRole("button", { name: "进入工作台" }).click();
  }
  await expect(page.getByText("设备未提供文件服务", { exact: true })).toBeVisible();
  expect(requests).toBe(0);
  service = true;
  await expect(page.getByText("数据链路未连接", { exact: true })).toBeVisible();
  expect(requests).toBe(0);
  data = true;
  await expect.poll(() => requests).toBe(1);
  await expect(page.locator(".files-page")).toBeVisible();
  data = false;
  await expect(page.getByText("数据链路未连接", { exact: true })).toBeVisible();
  expect(requests).toBe(1);
  data = true;
  await expect.poll(() => requests).toBe(2);
});
