// SPDX-License-Identifier: AGPL-3.0-or-later
// Phone width (320 px, WCAG 1.4.10): no page is wider than the screen, and
// the Stop is in reach on every page.
import type { Page } from "@playwright/test";
import { expect, login, scenario, test } from "./helpers";

async function offScreen(page: Page): Promise<string[]> {
  return page.evaluate(() => {
    const vw = document.documentElement.clientWidth;
    const out: string[] = [];
    if (document.documentElement.scrollWidth > vw) out.push(`page ${document.documentElement.scrollWidth} px wide`);
    for (const el of Array.from(document.body.querySelectorAll("*"))) {
      if (el.parentElement?.closest("svg")) continue;
      const r = el.getBoundingClientRect();
      if (r.width > 0 && r.height > 0 && r.right > vw + 1 && !el.closest(".chart, .table-wrap"))
        out.push(`${el.tagName.toLowerCase()}.${el.getAttribute("class") ?? ""} "${(el.textContent ?? "").trim().slice(0, 40)}"`);
    }
    return out;
  });
}

test("every page fits a phone and has the Stop", async ({ page, request }) => {
  await scenario(request, "demo");
  await login(page);
  // A watering circuit, so that its card is laid out too
  const put = await page.evaluate(
    async () => (await fetch("/api/v1/watering/circuits", { method: "PUT", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ name: "Greenhouse by the south wall", mode: "window", rateLpm: 2 }) })).status,
  );
  expect(put).toBe(200);
  // And a light circuit
  const light = await page.evaluate(
    async () => (await fetch("/api/v1/lighting/circuits", { method: "PUT", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ name: "Grow lamps above the long bench", dimmed: true, sunriseMin: 30, sunsetMin: 30 }) })).status,
  );
  expect(light).toBe(200);
  // And a climate room with CO2
  const room = await page.evaluate(
    async () => (await fetch("/api/v1/climate/rooms", { method: "PUT", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ name: "Flowering tent at the back", co2Ppm: 1000 }) })).status,
  );
  expect(room).toBe(200);
  for (const path of ["/", "/recipe", "/tank", "/water", "/watering", "/light", "/climate", "/history", "/channels", "/setup", "/settings"]) {
    await page.goto(`/#${path}`);
    if (path === "/watering") await expect(page.getByTestId("circuit-w1")).toBeVisible();
    if (path === "/light") await expect(page.getByTestId("light-l1")).toBeVisible();
    if (path === "/climate") await expect(page.getByTestId("room-r1")).toBeVisible();
    await page.waitForTimeout(800);
    expect(await offScreen(page), path).toEqual([]);
    await expect(page.getByTestId("stop")).toBeVisible();
  }
});
