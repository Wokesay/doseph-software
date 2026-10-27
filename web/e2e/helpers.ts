// SPDX-License-Identifier: AGPL-3.0-or-later
import { test as base, expect, type APIRequestContext, type Page } from "@playwright/test";

export { expect };

// Every test also fails on an uncaught exception in the page and on a part
// the error boundary had to replace: either could otherwise pass unnoticed
// where no step looks at that part of the page. The specs read English, the
// language a new robot and browser start in.
export const test = base.extend<{ appErrorGuard: void }>({
  appErrorGuard: [
    async ({ page }, use) => {
      const errors: string[] = [];
      page.on("pageerror", (e) => errors.push(e.message));
      await use();
      expect(errors, "uncaught exceptions in the page").toEqual([]);
      // ok: German, the error boundary's text in both languages
      if (!page.isClosed()) await expect(page.getByText(/Dieser Teil lässt sich nicht anzeigen|This part can't be shown/)).toHaveCount(0);  // ok: German
    },
    { auto: true },
  ],
});

/** A fresh simulator: "demo" is set up with the password demo-password, "new" has none yet. */
export async function scenario(request: APIRequestContext, name: "new" | "demo") {
  const r = await request.post("/api/v1/sim/scenario", { data: { name } });
  expect(r.ok()).toBeTruthy();
}

export async function login(page: Page, password = "demo-password") {
  await page.goto("/");
  await page.locator("input[name=password]").fill(password);
  await page.getByRole("button", { name: "Sign in" }).click();
  await expect(page.getByTestId("cycle-line")).toBeVisible();
}

/** A simulator control, signed in as the page is. */
export async function sim(page: Page, action: string, body: Record<string, unknown> = {}) {
  const status = await page.evaluate(
    async ([a, b]) => (await fetch(`/api/v1/sim/${a}`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(b) })).status,
    [action, body] as const,
  );
  expect(status).toBe(200);
}
