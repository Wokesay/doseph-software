// SPDX-License-Identifier: AGPL-3.0-or-later
// The rationale register (docs/RATIONALE.md) and the invariants
// (docs/INVARIANTS.md) say what the code does: every rule in force names
// where it is implemented and the tests that cover it, those files and
// tests exist, and every rule is placed in a module of the invariants.
import { test } from "node:test";
import assert from "node:assert/strict";
import { existsSync, readFileSync, readdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const read = (p) => readFileSync(join(root, p), "utf8");

// RAT-xxx → its entry; dropped rules carry "**Dropped:**"
function register() {
  const rules = new Map();
  for (const part of read("docs/RATIONALE.md").split(/\n(?=### RAT-)/).slice(1)) {
    const id = part.match(/^### (RAT-\d{3})/)[1];
    const field = (name) => part.match(new RegExp(`^- \\*\\*${name}:\\*\\* (.*)$`, "m"))?.[1];
    rules.set(id, { dropped: part.includes("- **Dropped:**"), rule: field("Rule"), impl: field("Implemented in"), tests: field("Tests") });
  }
  return rules;
}

// The names of all tests: doctest cases, Playwright tests and the tools' node tests
function testNames() {
  const names = new Set();
  const add = (dir, re) => {
    for (const f of readdirSync(join(root, dir)))
      for (const m of read(join(dir, f)).matchAll(re)) names.add(JSON.parse(`"${m[1]}"`));
  };
  add("tests/core", /TEST_CASE\("((?:[^"\\]|\\.)*)"\)/g);
  add("web/e2e", /\btest\("((?:[^"\\]|\\.)*)",/g);
  for (const f of readdirSync(join(root, "tools")).filter((f) => f.endsWith(".test.mjs")))
    for (const m of read(join("tools", f)).matchAll(/\btest\("((?:[^"\\]|\\.)*)",/g)) names.add(JSON.parse(`"${m[1]}"`));
  return names;
}

test("every rule in force says what it is, where it is implemented and which tests cover it", () => {
  const rules = register();
  assert.ok(rules.size > 80, "rules found");
  for (const [id, r] of rules) {
    if (r.dropped) continue;
    assert.ok(r.rule, `${id}: Rule`);
    assert.ok(r.impl, `${id}: Implemented in`);
    assert.ok(r.tests, `${id}: Tests`);
  }
});

test("the files and tests the register names exist", () => {
  const names = testNames();
  for (const [id, r] of register()) {
    if (r.dropped) continue;
    // Paths only; a symbol in backticks (`MixCycle::checks`) says where in the file
    for (const [, file] of r.impl.matchAll(/`([^`]+\/[^`]+)`/g)) assert.ok(existsSync(join(root, file)), `${id}: ${file}`);
    if (r.tests === "—") continue;  // no test of its own: marked ◐ in the invariants
    // Each item: `file` "test name", items separated by "; "
    for (const item of r.tests.split(/; (?=`)/)) {
      const m = item.match(/^`([^`]+)`(?: "(.*)")?/);
      assert.ok(m, `${id}: "${item}" is no test`);
      assert.ok(existsSync(join(root, m[1])), `${id}: ${m[1]}`);
      if (m[2] !== undefined) assert.ok(names.has(m[2]), `${id}: no test "${m[2]}"`);
    }
  }
});

test("a test that names a rule names one in force", () => {
  const rules = register();
  for (const name of testNames())
    for (const [id] of name.matchAll(/RAT-\d{3}/g)) {
      assert.ok(rules.has(id), `"${name}": ${id} is not in the register`);
      assert.ok(!rules.get(id).dropped, `"${name}": ${id} is dropped`);
    }
});

test("every rule in force is placed in a module of the invariants once, with a status that fits", () => {
  const rules = register();
  const placed = new Map();  // RAT-xxx → status
  for (const [, id, status] of read("docs/INVARIANTS.md").matchAll(/^\|[^|]+\| (RAT-\d{3}) \| (✓|◐) \|/gm)) {
    assert.ok(!placed.has(id), `${id} is placed twice`);
    placed.set(id, status);
  }
  for (const [id, r] of rules) {
    if (r.dropped) {
      assert.ok(!placed.has(id), `${id} is dropped but in INVARIANTS.md`);
      continue;
    }
    assert.ok(placed.has(id), `${id} is not in INVARIANTS.md`);
    assert.equal(placed.get(id), r.tests === "—" ? "◐" : "✓", `${id}: status`);
  }
});
