// SPDX-License-Identifier: AGPL-3.0-or-later
// Every state a role of a network device can be in (SD-132) has its words in the web app, in
// German and English: the gateway's output states and the device layer's value states.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const read = (p) => readFileSync(join(root, p), "utf8");

test("every state of a device role has its words in German and English", () => {
  const states = new Set();
  for (const line of read("core/src/gateway.cpp").split("\n").filter((l) => l.includes('j["state"] =')))
    for (const m of line.matchAll(/"([a-z_]+)"/g)) if (m[1] !== "state") states.add(m[1]);
  for (const m of read("core/src/devices.cpp").matchAll(/v\.state = "([a-z_]+)"/g)) states.add(m[1]);
  assert.ok(states.size >= 10, `states found: ${[...states]}`);
  const texts = read("web/src/lang/settings.ts");
  const [de, en] = texts.split("export const en");
  for (const s of states) {
    assert.ok(de.includes(`"settings.devices.state.${s}"`), `German: ${s}`);
    assert.ok(en.includes(`"settings.devices.state.${s}"`), `English: ${s}`);
  }
});
