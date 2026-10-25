// SPDX-License-Identifier: AGPL-3.0-or-later
import { defineConfig } from "@playwright/test";

// End-to-end against the simulator: the real core, the real API, the built web app.
// First: cmake --build ../build && npm run build
const port = Number(process.env.DP_E2E_PORT ?? 8099);

export default defineConfig({
  testDir: "./e2e",
  timeout: 120_000,
  expect: { timeout: 15_000 },
  workers: 1,
  fullyParallel: false,
  reporter: process.env.CI ? [["list"], ["html", { open: "never", outputFolder: "../build/e2e-report" }]] : "list",
  outputDir: "../build/e2e-results",
  use: {
    baseURL: `http://127.0.0.1:${port}`,
    locale: "de-DE",
    timezoneId: "Europe/Berlin",
    screenshot: "only-on-failure",
    trace: "retain-on-failure",
    // A Chromium of its own where Playwright's download is not available
    launchOptions: process.env.DP_CHROMIUM ? { executablePath: process.env.DP_CHROMIUM } : {},
  },
  webServer: {
    command: `../build/doseph_sim_server --port ${port} --scenario demo --web dist --password demo-password --prefill 2 --allow-reset`,
    url: `http://127.0.0.1:${port}/api/v1/info`,
    reuseExistingServer: !process.env.CI,
    timeout: 60_000,
  },
  projects: [
    { name: "desktop", testIgnore: /phone\.spec/, use: { viewport: { width: 1360, height: 900 } } },
    // 320 px is the width WCAG 1.4.10 (reflow) asks for
    { name: "phone", testMatch: /phone\.spec/, use: { viewport: { width: 320, height: 640 }, isMobile: true, hasTouch: true, deviceScaleFactor: 3 } },
  ],
});
