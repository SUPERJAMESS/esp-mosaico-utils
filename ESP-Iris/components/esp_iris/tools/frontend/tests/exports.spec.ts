import { expect, test } from "@playwright/test";

test("evidence export survives navigation and downloads without a browser blob", async ({ page }) => {
  await page.goto("/");
  const password = page.getByLabel("开发口令");
  if (await password.isVisible().catch(() => false)) await page.getByRole("button", { name: "进入工作台" }).click();
  await page.getByRole("button", { name: "设置", exact: true }).click();
  await page.getByLabel("设备 ID", { exact: true }).fill("demo-a1b2c3d4");
  await page.getByRole("button", { name: "导出证据包 ZIP" }).click();
  await expect(page.getByText("证据包已就绪", { exact: true }).first()).toBeVisible();
  const entry = page.locator(".export-job").first();
  await expect(entry.getByRole("link", { name: "下载证据包" })).toHaveAttribute("href", /\/v2\/exports\/.*\/download/);
  const download = page.waitForEvent("download");
  await entry.getByRole("link", { name: "下载证据包" }).click();
  expect((await download).suggestedFilename()).toMatch(/^esp-iris-.*\.zip$/);
  await page.reload();
  await page.getByRole("button", { name: "设置", exact: true }).click();
  await expect(page.getByText("证据包已就绪", { exact: true }).first()).toBeVisible();
  await page.setViewportSize({ width: 390, height: 844 });
  await expect(page.getByRole("button", { name: "导出证据包 ZIP" })).toBeVisible();
  await page.screenshot({ path: "/tmp/emvm72681-exports-mobile.png", fullPage: true });
  await page.locator(".export-job").first().getByRole("button", { name: "删除导出包" }).click();
  await expect(page.locator(".export-job")).toHaveCount(0);
});
