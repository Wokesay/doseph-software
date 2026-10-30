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

test("a channel is calibrated from the intake side: back into the canister, water from the cylinder, two readings, back into the canister (SD-143)", async ({ page }) => {
  await login(page);
  // Without a recipe the cycle doses nothing that a calibration would have to wait for
  await page.goto("/#/recipe");
  await page.getByRole("button", { name: "No recipe" }).click();
  await expect(page.getByText("No recipe in force any more")).toBeVisible();
  await page.goto("/#/channels");
  await page.getByTestId("calibrate-4").click();
  await page.getByTestId("cal-start").click();
  await sim(page, "speed", { speed: 10 });
  const step = page.getByTestId("cal-step");
  const cylinder = () => page.evaluate(async () => (await (await fetch("/api/v1/sim")).json()).world.calWater.cylinderMl as number);
  await expect(step).toHaveAttribute("data-step", "to_water", { timeout: 30_000 });
  await expect(page.getByTestId("cal-message")).toContainText("measuring cylinder");
  await sim(page, "intake", { channel: 4, where: "water" });
  await page.getByTestId("cal-next").click();
  await expect(step).toHaveAttribute("data-step", "reading", { timeout: 60_000 });
  const message = page.getByTestId("cal-message");
  const enter = async (value: number) => {
    await page.locator("input[name=reading]").fill(String(value));
    await page.getByTestId("cal-reading").click();
  };
  await enter(await cylinder());
  await expect(message).toContainText("after run 1", { timeout: 60_000 });
  // A typo is refused with its cause, and may be corrected
  await enter((await cylinder()) - 60);
  await expect(page.getByTestId("cal-error")).toContainText("a typo in the reading");
  await enter(await cylinder());
  await expect(message).toContainText("after run 2", { timeout: 60_000 });
  await enter(await cylinder());
  await expect(page.getByTestId("cal-runs")).toContainText("Run 2:");
  await expect(step).toHaveAttribute("data-step", "to_canister", { timeout: 60_000 });
  await sim(page, "intake", { channel: 4, where: "canister" });
  await page.getByTestId("cal-next").click();
  await expect(step).toHaveAttribute("data-step", "done", { timeout: 60_000 });
  await expect(page.getByTestId("cal-message")).toContainText("Channel 4 calibrated:");
  await page.getByTestId("cal-close").click();
  await expect(page.getByTestId("head-4")).toContainText("ml/min");
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

test("a pump head of the same size fitted again asks whether it is new; the same head keeps its rate (SD-144)", async ({ page }) => {
  await login(page);
  await sim(page, "channelHead", { channel: 3, head: "" });
  await page.goto("/#/channels");
  await expect(page.getByTestId("head-3")).toContainText("no pump head");
  await sim(page, "channelHead", { channel: 3, head: "S" });
  await page.goto("/#/");
  await expect(page.getByTestId("latch-channel.3.head_check")).toContainText("Is it a new head?", { timeout: 15_000 });
  await page.getByTestId("same-head-channel.3.head_check").click();
  await expect(page.getByTestId("latch-channel.3.head_check")).toHaveCount(0);
  await page.goto("/#/channels");
  await expect(page.getByTestId("head-3")).toContainText("ml/min");
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
  // The proposal's growth waits for confirmation (SD-138): this one moves on by itself
  await dialog.getByTestId("phase-0").locator("label.toggle").click();
  await expect(dialog.getByTestId("phase-0").getByRole("checkbox", { name: "Moves on only once confirmed" })).not.toBeChecked();
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

test("network devices: a Shelly is added and becomes reachable; the refill valve is assigned to its output and checked; a first-generation Shelly says why it is not used (SD-132)", async ({ page }) => {
  await login(page);
  await page.goto("/#/settings");
  const card = page.getByTestId("devices");
  await card.locator("input[name=device-address]").fill("shelly-1.local");
  await card.locator("input[name=device-name]").fill("Valve");
  await page.getByTestId("add-device").click();
  await expect(card.getByTestId("device-d1")).toContainText("reachable", { timeout: 15000 });
  const role = card.getByTestId("role-refill_valve");
  await role.locator("select[name=role-device-refill_valve]").selectOption("d1");
  await page.getByTestId("save-role-refill_valve").click();
  await expect(card.getByTestId("role-state-refill_valve")).toContainText("off, ready", { timeout: 15000 });
  await expect(role).toContainText("Auto-off in the device: 28");
  await card.locator("input[name=device-address]").fill("shelly-old.local");
  await card.locator("input[name=device-name]").fill("Old");
  await page.getByTestId("add-device").click();
  await expect(card.getByTestId("device-d2")).toContainText("only Gen2 and later are used", { timeout: 15000 });
});

test("refill: the inlet's relay as the valve and its input as the float; a fill by hand opens the valve and ends when the float says full (SD-132)", async ({ page }) => {
  await login(page);
  // Without a recipe the cycle doses nothing that a fill would have to wait for
  await page.goto("/#/recipe");
  await page.getByRole("button", { name: "No recipe" }).click();
  await expect(page.getByText("No recipe in force any more")).toBeVisible();
  await page.goto("/#/settings");
  const card = page.getByTestId("devices");
  await card.locator("input[name=device-address]").fill("shelly-valve.local");
  await card.locator("input[name=device-name]").fill("Inlet");
  await page.getByTestId("add-device").click();
  await expect(card.getByTestId("device-d1")).toContainText("reachable", { timeout: 15000 });
  await card.getByTestId("role-refill_valve").locator("select[name=role-device-refill_valve]").selectOption("d1");
  await page.getByTestId("save-role-refill_valve").click();
  await card.getByTestId("role-tank_full").locator("select[name=role-device-tank_full]").selectOption("d1");
  await page.getByTestId("save-role-tank_full").click();
  const refill = page.getByTestId("refill");
  await expect(refill.getByTestId("refill-state")).toContainText("Ready", { timeout: 20000 });
  await refill.getByTestId("refill-start").click();
  await expect(refill.getByTestId("refill-state")).toContainText("Filling", { timeout: 15000 });
  await sim(page, "speed", { speed: 10 });
  await expect(refill.getByTestId("refill-state")).toContainText("the tank is full", { timeout: 30000 });
  await expect(refill.getByTestId("refill-start")).toBeDisabled();
});

test("circulation: the twin's plug as the circulation pump; set to always, it runs (SD-132)", async ({ page }) => {
  await login(page);
  await page.goto("/#/settings");
  const card = page.getByTestId("devices");
  await card.locator("input[name=device-address]").fill("shelly-plug.local");
  await card.locator("input[name=device-name]").fill("Circulation");
  await page.getByTestId("add-device").click();
  await expect(card.getByTestId("device-d1")).toContainText("reachable", { timeout: 15000 });
  await card.getByTestId("role-circulation_pump").locator("select[name=role-device-circulation_pump]").selectOption("d1");
  await page.getByTestId("save-role-circulation_pump").click();
  const circulation = page.getByTestId("circulation");
  await expect(circulation.getByRole("tab", { name: "While mixing" })).toHaveAttribute("aria-selected", "true");
  await circulation.getByRole("tab", { name: "Always" }).click();
  await expect(circulation.getByRole("tab", { name: "Always" })).toHaveAttribute("aria-selected", "true");
  await expect(card.getByTestId("role-state-circulation_pump")).toHaveText("on", { timeout: 20000 });
});

test("a lock is set to report only in the settings, with a warning that leak, Stop and the limits always hold; the watchdog notes it until it locks again (SD-133)", async ({ page }) => {
  await login(page);
  await page.goto("/#/settings");
  const card = page.getByTestId("locks");
  const row = card.getByTestId("lock-flow.dry");
  await expect(row.getByRole("tab", { name: "Lock" })).toHaveAttribute("aria-selected", "true");
  await expect(page.getByTestId("save-locks")).toBeDisabled();
  await row.getByRole("tab", { name: "Report only" }).click();
  await expect(card.getByText("Leak, high level, the cable check, Stop, no dose without flow")).toBeVisible();
  await page.getByTestId("save-locks").click();
  await expect(page.getByText("Saved")).toBeVisible();
  await page.reload();
  await expect(page.getByTestId("lock-flow.dry").getByRole("tab", { name: "Report only" })).toHaveAttribute("aria-selected", "true");
  await page.goto("/#/");
  await expect(page.getByTestId("watchdog").getByText("Set to report only: no flow after the tries, or a dry run")).toBeVisible({ timeout: 15000 });
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

test("watering: a circuit on the twin's pump is added and given its output; where the water is not fit, it waters anyway after a confirmation that names why, and a shot is ended by hand; a change keeps its pause (SD-134, RAT-086)", async ({ page }) => {
  await login(page);
  await page.goto("/#/settings");
  const card = page.getByTestId("devices");
  await card.locator("input[name=device-address]").fill("shelly-pump.local");
  await card.locator("input[name=device-name]").fill("Pumps");
  await page.getByTestId("add-device").click();
  await expect(card.getByTestId("device-d1")).toContainText("reachable", { timeout: 15000 });
  // A new tank starts the count of litres
  const call = (path: string, body: unknown = {}) =>
    page.evaluate(async ([p, b]) => (await fetch(`/api/v1/${p}`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(b) })).status, [path, body] as const);
  expect(await call("tank/new")).toBe(200);
  await page.goto("/#/watering");
  await expect(page.getByTestId("circuits")).toContainText("No watering circuit yet");
  await page.getByTestId("add-circuit").click();
  const form = page.getByTestId("circuit-form");
  await form.locator("input[name=circuit-name]").fill("Tent");
  await form.locator("input[name=circuit-rate]").fill("2");
  await page.getByTestId("save-circuit").click();
  await form.locator("select[name='role-device-water.w1']").selectOption("d1");
  await page.getByTestId("save-role-water.w1").click();
  await expect(form.getByTestId("role-state-water.w1")).not.toContainText("not assigned", { timeout: 10000 });
  await page.keyboard.press("Escape");
  const circuit = page.getByTestId("circuit-w1");
  await expect(circuit).toContainText("Pumps, output 0", { timeout: 10000 });
  await expect(page.getByTestId("circuit-schedule-w1")).toContainText("by hand only");
  // The new tank is being mixed in, and a probe is in a buffer: the water cannot be judged, and a
  // person may water anyway, told why
  expect(await call("probe", { action: "start", kind: "ph" })).toBe(200);
  await page.getByTestId("circuit-anyway-w1").click({ timeout: 20000 });
  await expect(page.getByTestId("anyway-reason")).toHaveText(/mixed and measured|being calibrated/);
  await page.getByTestId("confirm-anyway").click();
  await expect(page.getByTestId("circuit-state-w1")).toContainText("watering anyway", { timeout: 15000 });
  await page.getByTestId("circuit-stop-w1").click();
  await expect(page.getByTestId("circuit-last-w1")).toContainText("Stopped by hand", { timeout: 15000 });
  await expect(page.getByTestId("circuit-last-w1")).toContainText(/\d\.\d\u00a0L/);
  await expect(page.getByTestId("circuit-last-w1")).toContainText("watered anyway");
  expect(await call("probe", { action: "cancel" })).toBe(200);
  // A schedule, paused; a change of the circuit keeps the pause
  await page.getByTestId("circuit-edit-w1").click();
  await form.getByRole("tab", { name: "Interval" }).click();
  await page.getByTestId("save-circuit").click();
  await expect(page.getByTestId("circuit-schedule-w1")).toContainText("every 4\u00a0h");
  await expect(page.getByTestId("circuit-next-w1")).toBeVisible();
  await page.getByTestId("circuit-pause-w1").locator(".toggle").click();
  await expect(page.getByTestId("circuit-state-w1")).toContainText("paused");
  await page.getByTestId("circuit-edit-w1").click();
  await form.locator("input[name=circuit-name]").fill("Tent north");
  await page.getByTestId("save-circuit").click();
  await expect(page.getByTestId("circuit-w1")).toContainText("Tent north");
  await expect(page.getByTestId("circuit-state-w1")).toContainText("paused");
  await expect(page.getByTestId("circuit-next-w1")).toHaveCount(0);
});

test("watering: a circuit with a time window follows a plant, whose phase sets its shots; the phase's shots are set on the Plants page (SD-134)", async ({ page }) => {
  await login(page);
  const call = (method: string, path: string, body: unknown = {}) =>
    page.evaluate(
      async ([m, p, b]) => {
        const r = await fetch(`/api/v1/${p}`, { method: m, headers: { "Content-Type": "application/json" }, body: JSON.stringify(b) });
        return { status: r.status, body: await r.json().catch(() => null) };
      },
      [method, path, body] as const,
    );
  const recipe = await call("PUT", "plant-recipes", { name: "Tomatoes", phases: [{ name: "Growth", days: 21, recipe: "two_part" }] });
  expect(recipe.status).toBe(200);
  // The phase's shots, set on the Plants page
  await page.goto("/#/plants");
  await page.getByRole("button", { name: "Edit: Tomatoes" }).click();
  await page.locator("input[name=phase-shots-0]").fill("12");
  await page.locator("input[name=phase-shot-0]").fill("30");
  await page.getByTestId("save-plant-recipe").click();
  await expect(page.getByText("Saved").first()).toBeVisible();
  expect((await call("POST", "plants", { name: "Bed 1", plantRecipe: recipe.body.id })).status).toBe(200);
  await page.goto("/#/watering");
  await page.getByTestId("add-circuit").click();
  const form = page.getByTestId("circuit-form");
  await form.locator("input[name=circuit-name]").fill("Tent");
  await form.getByRole("tab", { name: "Time window" }).click();
  await form.locator("select[name=circuit-plant]").selectOption({ label: "Bed 1" });
  await page.getByTestId("save-circuit").click();
  await page.keyboard.press("Escape");
  await expect(page.getByTestId("circuit-plant-w1")).toContainText("Bed 1, phase Growth: 12 × 30 s");
  await expect(page.getByTestId("circuit-next-w1")).toContainText("30 s");
});

test("light: a dimmer on the twin is added and given its output; on all day at its brightness, off by hand until tomorrow and back to the plan; a day of 0 h switched on by hand is said (SD-135)", async ({ page }) => {
  await login(page);
  await page.goto("/#/settings");
  const card = page.getByTestId("devices");
  await card.locator("input[name=device-address]").fill("shelly-dimmer.local");
  await card.locator("input[name=device-name]").fill("Lamps");
  await page.getByTestId("add-device").click();
  await expect(card.getByTestId("device-d1")).toContainText("reachable", { timeout: 15000 });
  await page.goto("/#/light");
  await expect(page.getByTestId("lights")).toContainText("No light circuit yet");
  await page.getByTestId("add-light").click();
  const form = page.getByTestId("light-form");
  await form.locator("input[name=light-name]").fill("Tent");
  await form.getByRole("tab", { name: "Dim 0–10 V" }).click();
  // A day of 24 h: on whatever the time
  await form.locator("input[name=light-hours]").fill("24");
  await form.locator("input[name=light-pct]").fill("70");
  await expect(page.getByTestId("light-preview")).toHaveText("Light all day.");
  await page.getByTestId("save-light").click();
  await form.locator("select[name='role-device-light.l1']").selectOption("d1");
  await page.getByTestId("save-role-light.l1").click();
  await expect(form.getByTestId("role-state-light.l1")).not.toContainText("not assigned", { timeout: 10000 });
  await page.keyboard.press("Escape");
  await expect(page.getByTestId("light-state-l1")).toContainText("on, 70 %", { timeout: 30000 });
  await expect(page.getByTestId("light-output-l1")).toContainText("Lamps, output 0, reports 70 %", { timeout: 15000 });
  await expect(page.getByTestId("light-plan-l1")).toHaveText("always on (24 h) · 70 %");
  // Off by hand: a day of 24 h has no change, so until tomorrow; then back to the plan
  await page.getByTestId("light-off-l1").click();
  await expect(page.getByTestId("light-state-l1")).toContainText("off");
  await expect(page.getByTestId("light-state-l1")).toContainText("by hand until tomorrow, ");
  await expect(page.getByTestId("light-output-l1")).toHaveText("Lamps, output 0", { timeout: 15000 });
  await page.getByTestId("light-plan-again-l1").click();
  await expect(page.getByTestId("light-state-l1")).toContainText("on, 70 %", { timeout: 15000 });
  await expect(page.getByTestId("light-plan-again-l1")).toHaveCount(0);
  // A day of 0 h: off; on by hand it is on in the dark period, and says so
  await page.getByTestId("light-edit-l1").click();
  await form.locator("input[name=light-hours]").fill("0");
  await expect(page.getByTestId("light-preview")).toHaveText("Dark all day.");
  await page.getByTestId("save-light").click();
  await expect(page.getByTestId("light-state-l1")).toHaveText("off", { timeout: 15000 });
  await page.getByTestId("light-on-l1").click();
  await expect(page.getByTestId("light-state-l1")).toContainText("on, 70 %", { timeout: 15000 });
  await expect(page.getByTestId("light-l1")).toContainText("On by hand in the dark period, which can disturb flowering.");
  // Back to the plan: off
  await page.getByTestId("light-plan-again-l1").click();
  await expect(page.getByTestId("light-state-l1")).toHaveText("off", { timeout: 15000 });
  await expect(page.getByTestId("light-l1")).not.toContainText("dark period");
});

test("light: a circuit follows a plant, whose phase sets the hours of light and the brightness; the phase's light is set on the Plants page; without an output it says so and offers no switch by hand (SD-135, RAT-083)", async ({ page }) => {
  await login(page);
  const call = (method: string, path: string, body: unknown = {}) =>
    page.evaluate(
      async ([m, p, b]) => {
        const r = await fetch(`/api/v1/${p}`, { method: m, headers: { "Content-Type": "application/json" }, body: JSON.stringify(b) });
        return { status: r.status, body: await r.json().catch(() => null) };
      },
      [method, path, body] as const,
    );
  const recipe = await call("PUT", "plant-recipes", { name: "Tomatoes", phases: [{ name: "Growth", days: 21, recipe: "two_part" }] });
  expect(recipe.status).toBe(200);
  await page.goto("/#/plants");
  await page.getByRole("button", { name: "Edit: Tomatoes" }).click();
  await page.locator("input[name=phase-light-hours-0]").fill("18");
  await page.locator("input[name=phase-light-pct-0]").fill("60");
  await page.getByTestId("save-plant-recipe").click();
  await expect(page.getByText("Saved").first()).toBeVisible();
  expect((await call("POST", "plants", { name: "Bed 1", plantRecipe: recipe.body.id })).status).toBe(200);
  await page.goto("/#/light");
  await page.getByTestId("add-light").click();
  const form = page.getByTestId("light-form");
  await form.locator("input[name=light-name]").fill("Tent");
  await form.getByRole("tab", { name: "Dim 0–10 V" }).click();
  await form.locator("input[name=light-start]").fill("06:00");
  await form.locator("input[name=light-hours]").fill("12");
  await form.locator("select[name=light-plant]").selectOption({ label: "Bed 1" });
  await page.getByTestId("save-light").click();
  await page.keyboard.press("Escape");
  // No output assigned: said, and nothing to switch by hand
  await expect(page.getByTestId("light-state-l1")).toHaveText("no output");
  await expect(page.getByTestId("light-on-l1")).toHaveCount(0);
  await expect(page.getByTestId("light-off-l1")).toHaveCount(0);
  await expect(page.getByTestId("light-plant-l1")).toHaveText("Bed 1, phase Growth: 18 h, 60 %");
  await expect(page.getByTestId("light-plan-l1")).toHaveText("06:00–00:00 (18 h) · 60 %");
  await expect(page.getByTestId("light-next-l1")).toContainText(/^day (ends|begins) at /);
});

test.describe("a browser in another time zone than the device", () => {
  test.use({ timezoneId: "America/New_York" });

  test("the times of a plan show on the device's clock, as the plan counts them (SD-134, SD-135)", async ({ page }) => {
    await login(page);
    const call = (method: string, path: string, body: unknown = {}) =>
      page.evaluate(async ([m, p, b]) => (await fetch(`/api/v1/${p}`, { method: m, headers: { "Content-Type": "application/json" }, body: JSON.stringify(b) })).status, [method, path, body] as const);
    expect(await call("PUT", "watering/circuits", { name: "Tent", mode: "times", times: [{ atMin: 360, s: 30 }], longestS: 60 })).toBe(200);
    expect(await call("PUT", "lighting/circuits", { name: "Lamps", startMin: 360, hours: 12 })).toBe(200);
    await page.goto("/#/watering");
    await expect(page.getByTestId("circuit-next-w1")).toContainText("06:00");
    await page.goto("/#/light");
    await expect(page.getByTestId("light-next-l1")).toContainText(/begins at 06:00|ends at 18:00/);
  });
});

test("climate: a room is added and given its exhaust fan and sensors; too warm, the fan runs; CO2 above 2,000 ppm is said (SD-136)", async ({ page }) => {
  await login(page);
  const call = (method: string, path: string, body: unknown = {}) =>
    page.evaluate(async ([m, p, b]) => (await fetch(`/api/v1/${p}`, { method: m, headers: { "Content-Type": "application/json" }, body: JSON.stringify(b) })).status, [method, path, body] as const);
  expect(await call("POST", "devices", { kind: "shelly", address: "shelly-climate.local", name: "Room relay" })).toBe(200);
  expect(await call("POST", "devices", { kind: "esphome", address: "esp-climate.local", name: "Room sensors" })).toBe(200);
  await page.goto("/#/climate");
  await expect(page.getByTestId("rooms")).toContainText("No room yet");
  await page.getByTestId("add-room").click();
  const form = page.getByTestId("room-form");
  await form.locator("input[name=room-name]").fill("Tent");
  await form.locator("input[name=room-hours]").fill("24");  // a day all day
  await form.locator("input[name=room-co2]").fill("1000");
  await page.getByTestId("save-room").click();
  // Its exhaust fan, assigned in the dialog
  await form.locator("select[name='role-device-climate.r1.exhaust']").selectOption("d1");
  await page.getByTestId("save-role-climate.r1.exhaust").click();
  await expect(form.getByTestId("role-state-climate.r1.exhaust")).not.toContainText("not assigned", { timeout: 10000 });
  await page.keyboard.press("Escape");
  for (const [kind, entity] of [["temp", "sensor/air_temp"], ["rh", "sensor/air_rh"], ["co2", "sensor/co2"]])
    expect(await call("PUT", `roles/climate.r1.${kind}`, { source: "esphome", device: "d2", entity })).toBe(200);
  await sim(page, "climateAir", { tempC: 31, rhPct: 60, co2Ppm: 700 });
  await expect(page.getByTestId("room-devices-r1")).toContainText("Exhaust fan: on", { timeout: 30000 });
  await expect(page.getByTestId("room-temp-r1")).toContainText("target 26 °C");
  await expect(page.getByTestId("room-co2-r1")).toContainText("target 1,000 ppm");
  await expect(page.getByTestId("room-state-r1")).toContainText("Day");
  // CO2 far too high: said
  await sim(page, "climateAir", { co2Ppm: 3000 });
  await expect(page.getByTestId("room-r1")).toContainText("CO2 above 2,000 ppm", { timeout: 20000 });
  // The sensors gone: said what has no value
  expect(await call("POST", "sim/netOnline", { address: "esp-climate.local", online: false })).toBe(200);
  await expect(page.getByTestId("room-missing-r1")).toContainText("No air temperature", { timeout: 60000 });
  await expect(page.getByTestId("room-missing-r1")).toContainText("No CO2 value: the CO2 valve stays closed.");
});

test("climate: a room follows a plant, whose phase sets its targets; the phase's climate is set on the Plants page (SD-136)", async ({ page }) => {
  await login(page);
  const call = (method: string, path: string, body: unknown = {}) =>
    page.evaluate(
      async ([m, p, b]) => {
        const r = await fetch(`/api/v1/${p}`, { method: m, headers: { "Content-Type": "application/json" }, body: JSON.stringify(b) });
        return { status: r.status, body: await r.json().catch(() => null) };
      },
      [method, path, body] as const,
    );
  const recipe = await call("PUT", "plant-recipes", { name: "Tomatoes", phases: [{ name: "Growth", days: 21, recipe: "two_part" }] });
  expect(recipe.status).toBe(200);
  await page.goto("/#/plants");
  await page.getByRole("button", { name: "Edit: Tomatoes" }).click();
  await page.locator("input[name=phase-day-c-0]").fill("24");
  await page.locator("input[name=phase-vpd-0]").fill("1.2");
  await page.getByTestId("save-plant-recipe").click();
  await expect(page.getByText("Saved").first()).toBeVisible();
  expect((await call("POST", "plants", { name: "Bed 1", plantRecipe: recipe.body.id })).status).toBe(200);
  await page.goto("/#/climate");
  await page.getByTestId("add-room").click();
  const form = page.getByTestId("room-form");
  await form.locator("input[name=room-name]").fill("Tent");
  await form.locator("input[name=room-hours]").fill("24");
  await form.locator("select[name=room-plant]").selectOption({ label: "Bed 1" });
  await page.getByTestId("save-room").click();
  await page.keyboard.press("Escape");
  await expect(page.getByTestId("room-state-r1")).toContainText("Bed 1, phase Growth");
  await expect(page.getByTestId("room-temp-r1")).toContainText("target 24 °C");
  await expect(page.getByTestId("room-rh-r1")).toContainText("from VPD 1.2 kPa");
});

test("plants: a running plant's own phases change on the Plants page; the past stays and the current phase cannot go (SD-137)", async ({ page }) => {
  await login(page);
  const call = (method: string, path: string, body: unknown = {}) =>
    page.evaluate(
      async ([m, p, b]) => {
        const r = await fetch(`/api/v1/${p}`, { method: m, headers: { "Content-Type": "application/json" }, body: m === "GET" ? undefined : JSON.stringify(b) });
        return { status: r.status, body: await r.json().catch(() => null) };
      },
      [method, path, body] as const,
    );
  const recipe = await call("PUT", "plant-recipes", { name: "Tomatoes", phases: [{ name: "Growth", days: 21, recipe: "two_part" }, { name: "Fruit", days: 56, recipe: "two_part" }] });
  expect(recipe.status).toBe(200);
  const plant = await call("POST", "plants", { name: "Bed 1", plantRecipe: recipe.body.id });
  expect(plant.status).toBe(200);
  expect((await call("POST", `plants/${plant.body.id}/next`)).status).toBe(200);  // in its second phase
  await page.goto("/#/plants");
  await page.getByRole("button", { name: "Edit the phases of Bed 1" }).click();
  const form = page.getByTestId("plant-phases-form");
  await expect(form.getByTestId("phase-0")).toHaveText("Phase 1: Growth, 21 days – past");
  await expect(form.getByTestId("phase-1")).toContainText("current");
  await expect(form.getByTestId("phase-1").getByRole("button", { name: "Remove phase" })).toHaveCount(0);
  await form.locator("input[name=phase-days-1]").fill("30");
  await page.getByTestId("add-plant-phase").click();
  await form.locator("input[name=phase-name-2]").fill("Flush");
  await page.getByTestId("save-plant-phases").click();
  await expect(page.getByTestId(`plant-phase-${plant.body.id}`)).toContainText("Fruit: day 1 of 30");
  // Only the plant: its recipe stays as it was
  const after = await call("GET", "plants");
  expect(after.body.plants[0].phases.length).toBe(3);
  expect(after.body.plantRecipes[0].phases.length).toBe(2);
});

test("plants: a new plant recipe starts from a proposal and says an EC target below the floor of pH dosing; a completed plant shows its record and takes its yield (SD-138, RAT-064)", async ({ page }) => {
  await login(page);
  const call = (method: string, path: string, body: unknown = {}) =>
    page.evaluate(
      async ([m, p, b]) => {
        const r = await fetch(`/api/v1/${p}`, { method: m, headers: { "Content-Type": "application/json" }, body: m === "GET" ? undefined : JSON.stringify(b) });
        return { status: r.status, body: await r.json().catch(() => null) };
      },
      [method, path, body] as const,
    );
  await page.goto("/#/plants");
  await page.getByTestId("new-plant-recipe").click();
  await expect(page.locator("input[name=phase-name-0]")).toHaveValue("Growth");
  await expect(page.locator("input[name=phase-days-0]")).toHaveValue("21");
  await expect(page.locator("input[name=phase-light-hours-0]")).toHaveValue("18");
  await expect(page.locator("input[name=phase-name-1]")).toHaveValue("Flowering");
  await expect(page.locator("input[name=phase-days-1]")).toHaveValue("56");
  await expect(page.locator("input[name=phase-light-hours-1]")).toHaveValue("12");
  // An EC target below the floor of pH dosing is said, and goes with it
  await page.locator("input[name=phase-ec-0]").fill("0.3");
  await expect(page.getByTestId("phase-ec-floor-0")).toContainText("The EC target 0.3\u00a0mS/cm is below 0.5\u00a0mS/cm, where the device doses no pH−");
  await page.locator("input[name=phase-ec-0]").fill("1.4");
  await expect(page.getByTestId("phase-ec-floor-0")).toHaveCount(0);
  await page.locator("input[name=plant-recipe-name]").fill("Peppers");
  await page.getByTestId("save-plant-recipe").click();
  await expect(page.getByText("Saved").first()).toBeVisible();
  const saved = await call("GET", "plants");
  const pr = saved.body.plantRecipes.find((r: { name: string }) => r.name === "Peppers");
  expect(pr.phases.map((x: { name: string; days: number; confirm: boolean; lightHours: number }) => [x.name, x.days, x.confirm, x.lightHours])).toEqual([
    ["Growth", 21, true, 18],
    ["Flowering", 56, false, 12],
  ]);
  // Completed on its first day: its record, and the yield by hand
  const plant = await call("POST", "plants", { name: "Bed 1", plantRecipe: pr.id });
  expect(plant.status).toBe(200);
  expect((await call("POST", `plants/${plant.body.id}/complete`)).status).toBe(200);
  await page.reload();
  await page.locator("summary", { hasText: "Completed" }).click();
  const row = page.getByTestId(`plant-done-${plant.body.id}`);
  await expect(row).toContainText("Bed 1, completed on");
  await expect(row).toContainText("Growth 1\u00a0day");
  await expect(row).toContainText("Into the tank:");
  await row.locator(`input[name=plant-yield-${plant.body.id}]`).fill("250");
  await page.getByTestId(`plant-yield-save-${plant.body.id}`).click();
  await expect(page.getByText("Yield saved")).toBeVisible();
  const after = await call("GET", "plants");
  expect(after.body.plants.find((x: { id: string }) => x.id === plant.body.id).yieldG).toBe(250);
});

test("reset: the access is reset at the device and a new password set, the settings stay; a factory reset needs the password and starts the device as new (SD-139)", async ({ page, request }) => {
  await login(page);
  await page.goto("/#/settings");
  await expect(page.getByTestId("reset-card")).toContainText("The access is reset at the device itself, never over the network.");
  // At the device: the access only, without a password
  const r = await request.post("/api/v1/sim/atDevice", { data: { reset: "access" } });
  expect(r.ok()).toBeTruthy();
  await page.goto("/");
  await page.locator("input[name=password]").fill("another-long-password");
  await page.locator("input[name=password2]").fill("another-long-password");
  await page.getByRole("button", { name: "Set password" }).click();
  await expect(page.getByTestId("cycle-line")).toBeVisible();
  await page.goto("/#/plants");
  await expect(page.getByTestId("plants-plan")).toContainText("Two-part nutrient");  // the settings stayed
  // The factory reset: the password first, then everything goes
  page.on("dialog", (d) => d.accept());
  await page.goto("/#/settings");
  await page.locator("input[name=factory-password]").fill("wrong-password");
  await page.locator("input[name=factory-sure]").check();
  await page.getByTestId("factory-reset").click();
  await expect(page.getByText("Wrong password").first()).toBeVisible();
  await page.locator("input[name=factory-password]").fill("another-long-password");
  await page.getByTestId("factory-reset").click();
  await expect(page.getByRole("button", { name: "Set password" })).toBeVisible({ timeout: 15000 });
  const info = await request.get("/api/v1/info");
  expect((await info.json()).hasPassword).toBe(false);
});

test("a level read after the doses proposes the rate that fits, and a worn pump's drops per ml say to calibrate it (SD-145)", async ({ page, request }) => {
  test.setTimeout(180_000);
  await scenario(request, "demo");
  await login(page);
  // A level read: the start of the comparison
  await page.goto("/#/channels");
  const readLevel = async (ml: number) => {
    await page.getByRole("button", { name: "Enter level: Part A" }).click();
    await page.locator("input[name=level]").fill(String(ml));
    await page.getByRole("dialog").getByRole("button", { name: "Save" }).click();
  };
  await readLevel(1000);
  await expect(page.getByText("Level saved")).toBeVisible();
  // The first round learns each channel's drops per ml; then channel 3's tube wears
  await sim(page, "fastForward", { hours: 2 });
  await sim(page, "pumpWear", { channel: 3, factor: 0.65 });
  for (let i = 0; i < 3; i++) {
    await sim(page, "topup", { litres: 60 });
    await sim(page, "fastForward", { hours: 6 });
  }
  // 10 % more went than the doses counted
  const counted = await page.evaluate(async () => 1000 - (await (await fetch("/api/v1/state")).json()).stock.part_a.ml);
  expect(counted).toBeGreaterThan(250);
  await readLevel(Math.round(1000 - counted * 1.1));
  await expect(page.getByText("a proposal waits under Dosing channels")).toBeVisible();
  const proposal = page.getByTestId("proposal-2");
  await expect(proposal).toContainText(`the count said ${Math.round(counted)} ml`);
  await proposal.getByTestId("take-rate-2").click();
  await expect(page.getByText("Rate taken")).toBeVisible();
  await expect(page.getByTestId("proposal-2")).toHaveCount(0);
  // Channel 3 gives fewer drops per ml since its tube wore
  await expect(page.getByTestId("drift-3")).toContainText("fewer drops per ml than after its calibration");
});
