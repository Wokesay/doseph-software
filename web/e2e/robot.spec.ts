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
  await expect(page.getByTestId("cycle-line")).toHaveText(/Confirm the placement first/);
});

test("status shows the cycle, the readings and the monitoring", async ({ page }) => {
  await login(page);
  for (const id of ["ph", "ec", "temp", "flow"]) await expect(page.getByTestId(`metric-${id}`)).toBeVisible();
  await expect(page.getByTestId("recipe-name")).toHaveText("Two-part nutrient");
  await expect(page.getByTestId("watchdog")).toContainText("pH probe");
  await expect(page.getByTestId("event").first()).toBeVisible();
});

test("status says why the robot doses or not (RAT-037)", async ({ page }) => {
  await login(page);
  await page.getByTestId("checks").locator("summary").click();
  await expect(page.getByTestId("checks-all")).toContainText("Recipe in force: Two-part nutrient");
  await expect(page.getByTestId("checks-ec")).toContainText("The EC reading");
  await expect(page.getByTestId("checks-ph")).toContainText("pH− ready");
});

test("maintenance holds the robot until it is ended (RAT-042)", async ({ page }) => {
  await login(page);
  await page.goto("/#/tank");
  await page.getByTestId("maintenance").getByRole("button", { name: "30\u00a0min" }).click();
  await expect(page.getByTestId("maintenance-until")).toBeVisible();
  await page.goto("/#/");
  await expect(page.getByTestId("cycle-line")).toContainText("Maintenance");
  await page.goto("/#/tank");
  await page.getByRole("button", { name: "End maintenance" }).click();
  await expect(page.getByTestId("maintenance-until")).toHaveCount(0);
});

test("Stop switches everything off and holds until Resume", async ({ page }) => {
  await login(page);
  await page.getByTestId("stop").click();
  await page.getByTestId("stop-confirm").click();
  await expect(page.getByTestId("stopped-bar")).toBeVisible();
  await expect(page.getByTestId("cycle-line")).toHaveText(/Stopped/);
  // Nothing runs while stopped, not even a channel into a cup
  await page.goto("/#/channels");
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

test("a recipe made from a template takes the canisters of the same name", async ({ page }) => {
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

test("a pH− canister is refused in a recipe, with the robot's reason", async ({ page }) => {
  await login(page);
  await page.goto("/#/recipe");
  await page.getByTestId("new-recipe").click();
  const dialog = page.getByRole("dialog");
  await dialog.locator("input[name=name]").fill("Without parts");
  await dialog.getByTestId("save-recipe").click();
  await expect(page.getByText("Without parts has no parts")).toBeVisible();
});

test("a new tank volume makes the next dose a test dose", async ({ page }) => {
  await login(page);
  await page.goto("/#/tank");
  await page.locator("input[name=volumeL]").fill("80");
  await page.getByTestId("save-volume").click();
  await expect(page.getByText("The next dose is a small test dose that checks the volume.")).toBeVisible();
  // A mixing wait outside its bounds is not sent
  await page.locator("input[name=mixMinMin]").fill("700");
  await expect(page.getByTestId("save-waits")).toBeDisabled();
});

test("a channel is calibrated: run into a cup, enter the ml", async ({ page }) => {
  await login(page);
  // Without a recipe the cycle doses nothing that a calibration would have to wait for
  await page.goto("/#/recipe");
  await page.getByRole("button", { name: "No recipe" }).click();
  await expect(page.getByText("No recipe in force any more")).toBeVisible();
  await page.goto("/#/channels");
  await page.getByTestId("calibrate-4").click();
  await page.locator("input[name=seconds]").fill("10");
  await page.getByTestId("job-start").click();
  await sim(page, "speed", { speed: 10 });
  await expect(page.locator("input[name=ml]")).toBeVisible({ timeout: 30_000 });
  await page.locator("input[name=ml]").fill("8");
  await page.getByTestId("job-result").click();
  await expect(page.getByTestId("job-done")).toContainText("Channel 4: 48.0 ml/min saved");
});

test("another pump head voids a channel's calibration; without a head the channel runs nothing (SD-108)", async ({ page }) => {
  await login(page);
  await page.goto("/#/channels");
  await expect(page.getByTestId("head-2")).toContainText("pump head S");
  await expect(page.getByTestId("drops-2")).toContainText("pulls back after each dose");
  await sim(page, "channelHead", { channel: 2, head: "M" });
  await expect(page.getByTestId("head-2")).toContainText("pump head M");
  await expect(page.getByTestId("head-2")).toContainText("not calibrated");
  await expect(page.getByTestId("calibrate-2")).toBeEnabled();
  await sim(page, "channelHead", { channel: 2, head: "" });
  await expect(page.getByTestId("head-2")).toContainText("no pump head");
  await expect(page.getByTestId("calibrate-2")).toBeDisabled();
});

test("a canister that siphons locks its channel until it is released (SD-111)", async ({ page }) => {
  await login(page);
  await sim(page, "channelFault", { channel: 4, siphon: true });
  await page.goto("/#/channels");
  await expect(page.getByTestId("locked-4")).toBeVisible({ timeout: 30_000 });
  await sim(page, "channelFault", { channel: 4, siphon: false });
  await page.goto("/#/");
  const lock = page.getByTestId("latch-channel.4.siphon");
  await expect(lock).toContainText("Channel 4 locked: it dripped without its pump");
  await lock.getByRole("button", { name: "Release" }).click();
  await expect(lock).toHaveCount(0);
});

test("a line is filled into the tank until it drips (SD-111)", async ({ page }) => {
  await login(page);
  // Without a recipe the cycle doses nothing that the fill would have to wait for
  await page.goto("/#/recipe");
  await page.getByRole("button", { name: "No recipe" }).click();
  await expect(page.getByText("No recipe in force any more")).toBeVisible();
  await sim(page, "lineEmpty", { channel: 4 });
  await page.goto("/#/channels");
  await page.getByTestId("prime-4").click();
  await expect(page.getByText("The outlet stays on the device")).toBeVisible();
  await expect(page.locator("input[name=seconds]")).toHaveCount(0);
  await page.getByTestId("job-start").click();
  await sim(page, "speed", { speed: 10 });
  await expect(page.getByTestId("job-done")).toContainText("Channel 4: the line is full", { timeout: 30_000 });
  await expect(page.getByTestId("drops-4")).toContainText("Drop sensor: last drop");
});

test("the water path shows how flow is proven, the chamber and the hardware release (SD-109, SD-110)", async ({ page }) => {
  await login(page);
  await page.goto("/#/water");
  // The example Home profile: flow by the outlet's level sensor, return by gravity
  await expect(page.getByTestId("flow-method")).toContainText("The outlet's level sensor reports water");
  await expect(page.getByTestId("turnover")).toContainText("min");
  await expect(page.getByTestId("return-by")).toContainText("by itself");
  await expect(page.getByTestId("chamber")).toContainText("pH");
  await expect(page.getByTestId("release")).toContainText("given");
  await sim(page, "stopButton", { pressed: true });
  await expect(page.getByTestId("release")).toContainText("withdrawn: stop button");
  await sim(page, "stopButton", { pressed: false });
  await expect(page.getByTestId("release")).toContainText("given");
});

test("the pH probe is calibrated with two buffers", async ({ page }) => {
  await login(page);
  await page.goto("/#/water");
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

test("a problem is reported with a diagnostics package that holds no secret", async ({ page }) => {
  await login(page);
  await page.goto("/#/settings");
  const card = page.getByTestId("report");
  await card.locator("textarea[name=what]").fill("The cycle stayed at the flow check");
  await card.getByRole("button", { name: "Create diagnostics package" }).click();
  await expect(page.getByTestId("report-id")).toContainText(/Package DP-[0-9a-f]{8}: \d+ events/);
  await card.getByRole("button", { name: "Show contents" }).click();
  await expect(page.getByTestId("report-contents")).toContainText('"reportId"');
  await expect(page.getByTestId("report-contents")).not.toContainText('"salt"');
  const [download] = await Promise.all([page.waitForEvent("download"), page.getByTestId("report-download").click()]);
  expect(download.suggestedFilename()).toMatch(/^doseph-diagnostics-DP-[0-9a-f]{8}\.json$/);
  const href = new URL((await page.getByTestId("report-issue").getAttribute("href"))!);
  expect(href.pathname).toBe("/Wokesay/doseph-software/issues/new");
  expect(href.searchParams.get("template")).toBe("bug.yml");
  expect(href.searchParams.get("report")).toMatch(/^DP-[0-9a-f]{8}$/);
  expect(href.searchParams.get("what")).toBe("The cycle stayed at the flow check");
});

test("settings are backed up and restored; a broken backup is refused with every reason", async ({ page }) => {
  await login(page);
  await page.goto("/#/settings");
  const card = page.getByTestId("backup");
  const [download] = await Promise.all([page.waitForEvent("download"), page.getByTestId("backup-download").click()]);
  expect(download.suggestedFilename()).toMatch(/^doseph-backup-\d{4}-\d{2}-\d{2}\.json$/);
  const backup = JSON.parse(await (await page.request.get("/api/v1/config/export")).text());
  const choose = card.locator("input[name=restore]");

  // A file that breaks two rules: nothing changes, and both reasons are shown
  const broken = { ...backup, system: { ...backup.system, name: "" }, probes: { ...backup.probes, ec: { factor: 5 } } };
  await choose.setInputFiles({ name: "broken.json", mimeType: "application/json", buffer: Buffer.from(JSON.stringify(broken)) });
  await card.getByRole("button", { name: "Restore" }).click();
  await expect(page.getByTestId("backup-errors").locator("li")).toHaveCount(2);
  await expect(page.getByTestId("backup-errors")).toContainText("EC calibration in the backup");

  // The good one comes back under another name
  const renamed = { ...backup, system: { ...backup.system, name: "From the backup" } };
  await choose.setInputFiles({ name: "good.json", mimeType: "application/json", buffer: Buffer.from(JSON.stringify(renamed)) });
  await expect(card).toContainText('Restore "good.json"?');
  await card.getByRole("button", { name: "Restore" }).click();
  await expect(page.getByTestId("backup-errors")).toHaveCount(0);
  await expect(page.locator("input[name=name]")).toHaveValue("From the backup");
});

test("a canister's level is counted, entered by hand and set full when refilled (SD-119)", async ({ page }) => {
  await login(page);
  await page.goto("/#/channels");
  const level = page.getByTestId("level-part_b");
  await expect(level).toContainText(/of 1,?000 ml/);
  await page.getByRole("button", { name: "Enter level: Part B" }).click();
  await page.locator("input[name=level]").fill("100");
  await page.getByRole("dialog").getByRole("button", { name: "Save" }).click();
  await expect(level).toContainText(/^100 of 1,?000 ml/);
  await expect(level).toContainText("nearly empty");
  await page.getByTestId("refilled-part_b").click();
  await expect(level).toContainText(/1,?000 of 1,?000 ml/);
  await expect(level).not.toContainText("nearly empty");
});

test("the simulator panel switches the water path's faults: a leak turns the hardware release off (SD-110)", async ({ page }) => {
  await login(page);
  await page.getByTestId("sim-open").click();
  const release = page.getByTestId("sim-release");
  await expect(release).toHaveText("Hardware release on");
  await expect(page.getByTestId("sim-water-path")).toContainText("Measuring chamber");
  await page.getByTestId("sim-leak").check();
  await expect(release).toHaveText("Hardware release off: leak");
  await page.getByTestId("sim-leak").uncheck();
  await expect(release).toHaveText("Hardware release on");
});

test("a leak locks every pump and is a critical alarm; acknowledging pauses its reminders; the lock is released only once the leak is fixed (SD-110, SD-123)", async ({ page }) => {
  await login(page);
  // The leak through the simulator's API: the panel would cover the status page
  await sim(page, "leak", { on: true });
  const lock = page.getByTestId("latch-hw.leak");
  await expect(lock).toContainText("Leak: every pump stays off");
  const alarm = page.getByTestId("alarm-latch.hw.leak");
  await expect(alarm).toContainText("critical");
  await page.getByTestId("ack-latch.hw.leak").click();
  await expect(alarm).toContainText("reminders paused until");
  await expect(page.getByTestId("ack-latch.hw.leak")).toHaveCount(0);
  await lock.getByRole("button", { name: "Release" }).click();
  await expect(page.getByText("Not released: the device still reports it")).toBeVisible();
  await sim(page, "leak", { on: false });
  await lock.getByRole("button", { name: "Release" }).click();
  await expect(lock).toHaveCount(0);
  await expect(alarm).toHaveCount(0);
});

test("quiet hours are set in the settings; what the robot notified is listed there (SD-123)", async ({ page }) => {
  await login(page);
  await page.goto("/#/settings");
  await page.locator("input[name=quietFrom]").fill("21:30");
  await page.locator("input[name=quietTo]").fill("06:45");
  await expect(page.getByText("in the morning report at 06:45")).toBeVisible();
  await page.getByTestId("save-quiet").click();
  await expect(page.getByText("Saved")).toBeVisible();
  await page.reload();
  await expect(page.locator("input[name=quietTo]")).toHaveValue("06:45");
  await page.locator("input[name=quietFrom]").fill("22:00");
  await page.locator("input[name=quietTo]").fill("07:00");
  await page.getByTestId("save-quiet").click();
});

test("push channels: a token is never shown again; a test message to a server that does not answer says why (SD-124)", async ({ page }) => {
  await login(page);
  await page.goto("/#/settings");
  const ntfy = page.getByTestId("push-ntfy");
  await ntfy.locator("input[name=ntfy-server]").fill("http://127.0.0.1:9");
  await ntfy.locator("input[name=ntfy-topic]").fill("doseph-e2e");
  await ntfy.locator("input[name=ntfy-token]").fill("tk_e2e_secret");
  await ntfy.locator("label.toggle").click();
  await page.getByTestId("save-push").click();
  await expect(page.getByText("Saved")).toBeVisible();
  await page.reload();
  await expect(page.locator("input[name=ntfy-topic]")).toHaveValue("doseph-e2e");
  await expect(page.locator("input[name=ntfy-token]")).toHaveValue("");
  await expect(page.getByTestId("push-ntfy")).toContainText("Set. Leave empty to keep it.");
  await page.getByTestId("test-push").click();
  await expect(page.getByTestId("push-status-ntfy")).toContainText("not delivered", { timeout: 30_000 });
  await page.getByTestId("push-ntfy").locator("label.toggle").click();
  await page.getByTestId("save-push").click();
  await expect(page.getByTestId("test-push")).toBeDisabled();
});

test("maintenance dates: the next date with its items, cleaning marked done, a calibration item links to its page, intervals in bounds (SD-125)", async ({ page }) => {
  await login(page);
  await page.goto("/#/tank");
  const card = page.getByTestId("service");
  await expect(card).toContainText("Next date:");
  await expect(card).toContainText("Clean the measuring chamber, the feed pump and its inlet screen");
  await page.getByTestId("service-done-chamber.clean").click();
  await expect(page.getByText("Noted")).toBeVisible();
  await expect(page.getByTestId("service-probe.ph.cal").getByRole("link", { name: "Calibrate" })).toHaveAttribute("href", "#/water");
  await card.getByText("Adjust intervals").click();
  await page.locator("input[name='service-chamber.clean']").fill("3");
  await page.getByTestId("save-service").click();
  await expect(page.getByText("A maintenance interval is 7 to 730")).toBeVisible();
});

test("plants: a plant recipe with phases, a plant started from it sets the water, the next phase and the calendar, and once completed the robot holds (SD-128)", async ({ page }) => {
  await login(page);
  await page.goto("/#/plants");
  await expect(page.getByTestId("plants-plan")).toContainText("Two-part nutrient");  // chosen by hand
  await page.getByTestId("new-plant-recipe").click();
  const dialog = page.getByRole("dialog");
  await dialog.locator("input[name=plant-recipe-name]").fill("Lettuce");
  await dialog.locator("input[name=phase-name-0]").fill("Young");
  await dialog.locator("input[name=phase-days-0]").fill("10");
  await dialog.locator("input[name=phase-ec-0]").fill("1.2");
  await dialog.getByTestId("add-phase").click();
  await dialog.locator("input[name=phase-name-1]").fill("Grown");
  await dialog.locator("input[name=phase-days-1]").fill("20");
  await dialog.getByTestId("save-plant-recipe").click();
  await expect(page.getByText("Young → Grown")).toBeVisible();
  await page.getByTestId("open-start-plant").click();
  await page.getByRole("dialog").locator("input[name=plant-name]").fill("Bed A");
  await page.getByRole("dialog").getByTestId("start-plant").click();
  await expect(page.getByTestId("plants-plan")).toContainText("Bed A sets the water: phase Young");
  await expect(page.getByText("Young: day 1 of 10")).toBeVisible();
  await expect(page.getByTestId("calendar").getByTestId("cal-phase")).toContainText("Bed A: Grown begins");
  // The status page names the plant and its phase, with the phase's EC target
  await page.goto("/#/");
  await expect(page.getByTestId("plan-plant")).toHaveText("Bed A, Young");
  await expect(page.getByText("1.20 ± 0.10 mS/cm")).toBeVisible();
  // The recipe page leads to the plants
  await page.goto("/#/recipe");
  await expect(page.getByTestId("active-recipe")).toContainText("Bed A sets the water");
  // The next phase by hand, then completed: no water plan is left
  await page.goto("/#/plants");
  await page.locator("[data-testid^=plant-next-]").click();
  await expect(page.getByText("Grown: day 1 of 20")).toBeVisible();
  page.once("dialog", (d) => d.accept());
  await page.locator("[data-testid^=plant-complete-]").click();
  await expect(page.getByTestId("plants-plan")).toContainText("No water plan");
  await expect(page.getByTestId("plants-completed")).toContainText("Bed A");
});

test("the cultivation system is chosen on the tank page; a new phase is offered its pH range (SD-128)", async ({ page }) => {
  await login(page);
  await page.goto("/#/tank");
  await page.locator("select[name=system]").selectOption("soil");
  await page.getByTestId("save-system").click();
  await expect(page.getByText("Saved")).toBeVisible();
  await page.goto("/#/plants");
  await page.getByTestId("new-plant-recipe").click();
  await expect(page.getByRole("dialog")).toContainText("a new phase gets the one for Soil: 6.2–6.8");
});

test("an access token is made in the settings, shown once, works within its role and is revoked (SD-129)", async ({ page, request }) => {
  await login(page);
  await page.goto("/#/settings");
  await expect(page.getByTestId("device-id")).toHaveText(/^[0-9a-f]{16}$/);
  await page.locator("input[name=tokenName]").fill("Account e2e");
  await page.getByRole("tab", { name: "Account" }).click();
  await expect(page.getByText("Never limits, releases, calibration")).toBeVisible();
  await page.getByTestId("create-token").click();
  const token = (await page.getByTestId("new-token").textContent()) ?? "";
  expect(token).toMatch(/^dpt_[0-9a-f]{64}$/);
  const row = page.getByTestId("tokens").getByText("Account e2e");
  await expect(row).toBeVisible();

  const bearer = { Authorization: `Bearer ${token}` };
  expect((await request.get("/api/v1/state", { headers: bearer })).status()).toBe(200);
  expect((await request.put("/api/v1/control", { headers: bearer, data: { ecTolerance: 0.5 } })).status()).toBe(403);
  expect((await request.post("/api/v1/sim/speed", { headers: bearer, data: { speed: 1 } })).status()).toBe(403);
  expect((await request.post("/api/v1/stop", { headers: bearer, data: {} })).status()).toBe(200);

  // Shown once: after a reload only the name and role are left
  await page.reload();
  await expect(page.getByTestId("new-token")).toHaveCount(0);
  await expect(page.getByTestId("tokens")).toContainText("Account e2e");
  await expect(page.getByTestId("tokens")).toContainText("last used");
  page.once("dialog", (d) => d.accept());
  await page.getByTestId("tokens").getByRole("button", { name: "Revoke" }).click();
  await expect(page.getByText("No access token yet.")).toBeVisible();
  expect((await request.get("/api/v1/state", { headers: bearer })).status()).toBe(401);
});

test("commissioning: a new robot shows the next step everywhere; placement is confirmed, push skipped, and the state stays after a reload (SD-129)", async ({ page, request }) => {
  await scenario(request, "new");
  await page.goto("/");
  await page.locator("input[name=password]").fill("a-long-password");
  await page.locator("input[name=password2]").fill("a-long-password");
  await page.getByRole("button", { name: "Set password" }).click();
  await expect(page.getByTestId("setup-banner")).toHaveText(/next is "Placement"/);
  await page.getByTestId("setup-banner").getByRole("link", { name: "Continue" }).click();
  await expect(page.getByTestId("setup-banner")).toHaveCount(0);
  await expect(page.getByTestId("setup-password")).toHaveAttribute("data-state", "done");
  await expect(page.getByTestId("setup-network")).toHaveAttribute("data-state", "later");
  await expect(page.getByTestId("setup-placement")).toContainText("Lay the feed pump in the tank");
  await page.getByTestId("confirm-placement").click();
  await expect(page.getByTestId("setup-placement")).toHaveAttribute("data-state", "done");
  await page.getByTestId("skip-push").click();
  await expect(page.getByTestId("setup-push")).toHaveAttribute("data-state", "skipped");
  // The robot's own steps have no button to tick them off
  await expect(page.getByTestId("setup-probes").getByRole("button")).toHaveCount(0);
  await expect(page.getByTestId("setup-probes")).toContainText("The robot ticks it off itself");
  await page.reload();
  await expect(page.getByTestId("setup-placement")).toHaveAttribute("data-state", "done");
  await expect(page.getByTestId("setup-progress")).toHaveText("3 of 9 steps done");
  await page.goto("/#/tank");
  await expect(page.getByTestId("setup-banner")).toBeVisible();
});

test("commissioning: the demo robot is set up, so no step is asked for (SD-129)", async ({ page }) => {
  await login(page);
  await expect(page.getByTestId("setup-banner")).toHaveCount(0);
  await page.goto("/#/setup");
  await expect(page.getByText("Commissioning is complete.")).toBeVisible();
});

test("MQTT is off until switched on; without TLS it only sends values; the password is never shown again (PD-461, SD-130)", async ({ page }) => {
  await login(page);
  await page.goto("/#/settings");
  const card = page.getByTestId("mqtt");
  await expect(card.getByLabel("Take commands")).toBeDisabled();
  await card.locator('label.toggle[title="On"]').click();
  await card.locator("input[name=mqtt-host]").fill("broker.lan");
  await card.locator("input[name=mqtt-user]").fill("doseph");
  await card.locator("input[name=mqtt-password]").fill("mqtt-secret");
  await expect(card.getByText("Without TLS anyone on your network can read the values")).toBeVisible();
  await page.getByTestId("save-mqtt").click();
  await expect(page.getByText("Saved")).toBeVisible();
  await card.locator('label.toggle[title="TLS (encrypted)"]').click();
  await expect(card.locator("input[name=mqtt-port]")).toHaveValue("8883");
  await expect(card.getByLabel("Take commands")).toBeEnabled();
  await card.locator('label.toggle[title="Take commands"]').click();
  await page.getByTestId("save-mqtt").click();
  await page.reload();
  await expect(page.locator("input[name=mqtt-host]")).toHaveValue("broker.lan");
  await expect(page.locator("input[name=mqtt-password]")).toHaveValue("");
  await expect(page.getByTestId("mqtt")).toContainText("Set. Leave empty to keep it.");
  await expect(page.getByTestId("mqtt").getByLabel("Take commands")).toBeChecked();
  await expect(page.getByTestId("mqtt-status")).toContainText("Topics under doseph/");
});

test("a lock is set to report only in the settings, with a warning that leak, Stop and the limits always hold; the overview names it until it locks again (SD-133)", async ({ page }) => {
  await login(page);
  await page.goto("/#/settings");
  const card = page.getByTestId("locks");
  const row = card.getByTestId("lock-flow.dry");
  await expect(row.getByRole("tab", { name: "Lock" })).toHaveAttribute("aria-selected", "true");
  await expect(page.getByTestId("save-locks")).toBeDisabled();
  await row.getByRole("tab", { name: "Report only" }).click();
  await expect(card.getByText("Leak, high level, Stop, no dose without flow")).toBeVisible();
  await page.getByTestId("save-locks").click();
  await expect(page.getByText("Saved")).toBeVisible();
  await page.reload();
  await expect(page.getByTestId("lock-flow.dry").getByRole("tab", { name: "Report only" })).toHaveAttribute("aria-selected", "true");
  await page.goto("/#/");
  await expect(page.getByText("Set to report only: no flow after the tries, or a dry run").first()).toBeVisible({ timeout: 15000 });
  await page.goto("/#/settings");
  await page.getByTestId("lock-flow.dry").getByRole("tab", { name: "Lock" }).click();
  await page.getByTestId("save-locks").click();
  await expect(page.getByText("Saved")).toBeVisible();
});

test("coupling: this device shows a code for the leader; a pairing that cannot reach the other device, or reaches this one, says why (SD-131)", async ({ page }) => {
  await login(page);
  await page.goto("/#/settings");
  const card = page.getByTestId("coupling");
  await card.getByTestId("show-code").click();
  const code = (await card.getByTestId("pairing-code").textContent()) ?? "";
  expect(code).toMatch(/^[2-9A-HJ-NP-Z]{4}-[2-9A-HJ-NP-Z]{4}$/);
  // Nothing answers there
  await card.locator("input[name=peer-address]").fill("127.0.0.1:9");
  await card.locator("input[name=peer-code]").fill("ABCD-EFGH");
  await card.getByTestId("pair").click();
  await expect(card.getByTestId("pairing-state")).toHaveText("The other device can't be reached", { timeout: 15_000 });
  // This device itself, with its own code: refused
  await card.locator("input[name=peer-address]").fill(new URL(page.url()).host);
  await card.locator("input[name=peer-code]").fill(code.toLowerCase());
  await card.getByTestId("pair").click();
  await expect(card.getByTestId("pairing-state")).toHaveText("A device can't be coupled with itself", { timeout: 15_000 });
  await expect(card.getByTestId("coupled")).toHaveCount(0);
});
