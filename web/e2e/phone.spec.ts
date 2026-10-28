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
  for (const path of ["/", "/recipe", "/tank", "/history", "/channels", "/settings"]) {
    await page.goto(`/#${path}`);
    await page.waitForTimeout(800);
    expect(await offScreen(page), path).toEqual([]);
    await expect(page.getByTestId("stop")).toBeVisible();
  }
});
