// SPDX-License-Identifier: AGPL-3.0-or-later
// The mixing robot end to end: the real core in the simulator, the built app.
import { expect, login, scenario, sim, test } from "./helpers";

test.beforeEach(async ({ request }) => {
  await scenario(request, "demo");
});

test("the first password needs no default and is asked for twice", async ({ page, request }) => {
  await scenario(request, "new");
  await page.goto("/");
  await expect(page.getByRole("heading", { name: "Welcome" })).toBeVisible();
  await page.locator("input[name=password]").fill("a-long-password");
  await page.locator("input[name=password2]").fill("another-password");
  await page.getByRole("button", { name: "Set password" }).click();
  await expect(page.getByText("The passwords do not match.")).toBeVisible();
  await page.locator("input[name=password2]").fill("a-long-password");
  await page.getByRole("button", { name: "Set password" }).click();
  await expect(page.getByTestId("cycle-line")).toHaveText(/No recipe in force/);
});

test("status shows the cycle, the readings and the monitoring", async ({ page }) => {
  await login(page);
  for (const id of ["ph", "ec", "temp", "flow"]) await expect(page.getByTestId(`metric-${id}`)).toBeVisible();
  await expect(page.getByTestId("recipe-name")).toHaveText("Two-part nutrient");
  await expect(page.getByTestId("watchdog")).toContainText("pH probe");
  await expect(page.getByTestId("event").first()).toBeVisible();
});

test("Stop switches everything off and holds until Resume", async ({ page }) => {
  await login(page);
  await page.getByTestId("stop").click();
  await page.getByTestId("stop-confirm").click();
  await expect(page.getByTestId("stopped-bar")).toBeVisible();
  await expect(page.getByTestId("cycle-line")).toHaveText(/Stopped/);
  // Nothing runs while stopped, not even a pump into a cup
  await page.goto("/#/pumps");
  await expect(page.getByTestId("calibrate-2")).toBeDisabled();
  await page.getByTestId("resume").click();
  await expect(page.getByTestId("stopped-bar")).toHaveCount(0);
});

test("a recipe is put in force for a number of days", async ({ page }) => {
  await login(page);
  await page.goto("/#/recipe");
  await page.locator("input[name=days]").fill("14");
  await page.getByTestId("apply-recipe").click();
  await expect(page.getByTestId("active-recipe")).toContainText("until");
  await page.getByRole("tab", { name: "Without an end" }).click();
  await page.getByTestId("apply-recipe").click();
  await expect(page.getByTestId("active-recipe")).toContainText("without an end");
});

test("a recipe made from a template takes the bottles of the same name", async ({ page }) => {
  await login(page);
  await page.goto("/#/recipe");
  await page.getByTestId("template-two_part_basic").getByRole("button").click();
  const dialog = page.getByRole("dialog");
  await expect(dialog.locator("input[name=name]")).toHaveValue("Two-part nutrient");
  await dialog.locator("input[name=name]").fill("Copy");
  await dialog.getByTestId("save-recipe").click();
  await expect(page.getByText("\"Copy\" saved")).toBeVisible();
  await expect(page.locator("[data-testid^=recipe-]").filter({ hasText: "Copy" })).toContainText("Part B 2.00, Part A 2.00, CalMag 0.50");
});

test("a pH− bottle is refused in a recipe, with the robot's reason", async ({ page }) => {
  await login(page);
  await page.goto("/#/recipe");
  await page.getByTestId("new-recipe").click();
  const dialog = page.getByRole("dialog");
  await dialog.locator("input[name=name]").fill("Without parts");
  await dialog.getByTestId("save-recipe").click();
  await expect(page.getByText("Without parts has no parts")).toBeVisible();
});

test("a new tank volume makes the next dose a test dose; the turnover is shown", async ({ page }) => {
  await login(page);
  await page.goto("/#/tank");
  await expect(page.getByTestId("turnover")).toContainText("min");
  await page.locator("input[name=volumeL]").fill("80");
  await page.getByTestId("save-volume").click();
  await expect(page.getByText("The next dose is a small test dose that checks the volume.")).toBeVisible();
  // A mixing wait outside its bounds is not sent
  await page.locator("input[name=waitEcMin]").fill("1");
  await expect(page.getByTestId("save-waits")).toBeDisabled();
});

test("a pump is calibrated: run into a cup, enter the ml", async ({ page }) => {
  await login(page);
  // Without a recipe the cycle doses nothing that a calibration would have to wait for
  await page.goto("/#/recipe");
  await page.getByRole("button", { name: "No recipe" }).click();
  await expect(page.getByText("No recipe in force any more")).toBeVisible();
  await page.goto("/#/pumps");
  await page.getByTestId("calibrate-4").click();
  await page.locator("input[name=seconds]").fill("10");
  await page.getByTestId("pump-start").click();
  await sim(page, "speed", { speed: 10 });
  await expect(page.locator("input[name=ml]")).toBeVisible({ timeout: 30_000 });
  await page.locator("input[name=ml]").fill("8");
  await page.getByTestId("pump-result").click();
  await expect(page.getByTestId("pump-done")).toContainText("Pump 4: 48.0 ml/min saved");
});

test("the pH probe is calibrated with two buffers", async ({ page }) => {
  await login(page);
  await page.goto("/#/pumps");
  await page.getByTestId("calibrate-ph").click();
  await page.getByTestId("probe-start").click();
  await sim(page, "buffer", { kind: "ph", value: 7 });
  await page.waitForTimeout(3000);
  await page.getByTestId("probe-point").click();
  await expect(page.locator("input[name=reference]")).toHaveValue("4");
  await sim(page, "buffer", { kind: "ph", value: 4 });
  await page.waitForTimeout(3000);
  await page.getByTestId("probe-point").click();
  await page.getByTestId("probe-save").click();
  await expect(page.getByText("Calibration saved")).toBeVisible();
  await sim(page, "buffer", { kind: "ph" });
});

test("German shows the robot's messages in German", async ({ page }) => {
  await login(page);
  await page.goto("/#/settings");
  await page.getByRole("tab", { name: "Deutsch" }).click();
  await page.goto("/#/");
  await expect(page.getByRole("heading", { name: "Mischzyklus" })).toBeVisible();
  await expect(page.getByTestId("watchdog")).toContainText("pH-Sonde");
  await page.goto("/#/settings");
  await page.getByRole("tab", { name: "English" }).click();
  await expect(page.getByRole("heading", { name: "Language and look" })).toBeVisible();
});

test("history draws the four curves", async ({ page }) => {
  await login(page);
  await page.goto("/#/history");
  await expect(page.locator(".chart")).toHaveCount(4);
  await page.getByRole("button", { name: "Doses" }).click();
  await expect(page.getByTestId("event").first()).toBeVisible();
});

test("history downloads as CSV in the page's format (RAT-016)", async ({ page }) => {
  await login(page);
  await page.goto("/#/history");
  const href = await page.getByTestId("history-csv").getAttribute("href");
  expect(href).toContain("lang=en");
  const res = await page.request.get(href!);
  expect(res.status()).toBe(200);
  const text = await res.text();
  expect(text.startsWith("time_utc,ph,ec,temp,flow\n")).toBe(true);
  expect(text.split("\n").length).toBeGreaterThan(10);
});
