// SPDX-License-Identifier: AGPL-3.0-or-later
// The generator of the core's time zone table (tools/gen_tzdb.mjs).
import { test } from "node:test";
import assert from "node:assert/strict";
import { addLinks, footerRule, render } from "./gen_tzdb.mjs";

const tzif = (version, footer) => {
  const head = Buffer.alloc(44);
  head.write("TZif", 0, "latin1");
  head[4] = version;
  return Buffer.concat([head, Buffer.from(`\n${footer}\n`, "latin1")]);
};

test("the rule is read from the footer of a TZif file of version 2 or later", () => {
  assert.equal(footerRule(tzif(0x32, "CET-1CEST,M3.5.0,M10.5.0/3")), "CET-1CEST,M3.5.0,M10.5.0/3");
  assert.equal(footerRule(tzif(0x34, "<+0330>-3:30")), "<+0330>-3:30");
  assert.equal(footerRule(tzif(0, "CET-1")), null);  // version 1 has no footer
  assert.equal(footerRule(Buffer.from("not a zone file at all, but long enough to read\n")), null);
  assert.equal(footerRule(tzif(0x32, "with space")), null);
});

test("links give old names their target's rule, never overwrite a zone, and the table is sorted", () => {
  const zones = new Map([["Asia/Kolkata", "IST-5:30"], ["Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"]]);
  addLinks(zones, "# version 2026e\nL Asia/Kolkata Asia/Calcutta\nL Nowhere/Gone Old/Name\nL Asia/Kolkata Europe/Berlin\n");
  assert.equal(zones.get("Asia/Calcutta"), "IST-5:30");
  assert.equal(zones.has("Old/Name"), false);
  assert.equal(zones.get("Europe/Berlin"), "CET-1CEST,M3.5.0,M10.5.0/3");
  const out = render(zones, "2026e");
  assert.match(out, /version 2026e/);
  const names = [...out.matchAll(/\{"([^"]+)", "/g)].map((m) => m[1]);
  assert.deepEqual(names, ["Asia/Calcutta", "Asia/Kolkata", "Europe/Berlin"]);
});
