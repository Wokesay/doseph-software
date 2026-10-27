// SPDX-License-Identifier: AGPL-3.0-or-later
// Tests for the license check of the npm packages: the SPDX expressions
// (tools/spdx_expr.mjs), the allow lists (tools/license_policy.mjs) and the
// check itself (tools/check_licenses.mjs).
//   node --test tools/check_licenses.test.mjs
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { test } from "node:test";
import { fileURLToPath } from "node:url";
import { importProblems, licenseProblems, packageOf, runtimeImports } from "./license_policy.mjs";
import { allowed } from "./spdx_expr.mjs";

const OK = new Set(["MIT", "Apache-2.0", "ISC"]);

test("single licenses", () => {
  assert.equal(allowed("MIT", OK), true);
  assert.equal(allowed("GPL-3.0-only", OK), false);
  assert.equal(allowed("", OK), false);
});

test("OR needs one allowed side, AND needs both", () => {
  assert.equal(allowed("(MIT OR GPL-3.0-only)", OK), true);
  assert.equal(allowed("MIT AND GPL-3.0-only", OK), false);
  assert.equal(allowed("MIT AND Apache-2.0", OK), true);
  assert.equal(allowed("(GPL-2.0-only OR (MIT AND Apache-2.0))", OK), true);
});

test("malformed expressions are never allowed", () => {
  assert.equal(allowed("MIT GPL-3.0-only", OK), false); // missing operator
  assert.equal(allowed("MIT and GPL-3.0-only", OK), false); // lower-case operator
  assert.equal(allowed("(MIT and GPL-2.0-only)", OK), false);
  assert.equal(allowed("(MIT OR ISC", OK), false); // missing bracket
  assert.equal(allowed("MIT OR ISC)", OK), false); // stray bracket
  assert.equal(allowed("MIT WITH Classpath-exception-2.0", OK), false); // WITH is not supported
  assert.equal(allowed("OR MIT", OK), false);
});

// One package in a lockfile; runtime packages are listed in the notices.
function problems(name, pkg) {
  const lock = { lockfileVersion: 3, packages: { "": {}, [`node_modules/${name}`]: pkg } };
  return licenseProblems(lock, `| \`${name}\` |`).problems;
}

test("MPL-2.0 only for lightningcss as a build tool", () => {
  assert.deepEqual(problems("lightningcss", { license: "MPL-2.0", dev: true }), []);
  assert.deepEqual(problems("lightningcss-linux-x64-musl", { license: "MPL-2.0", dev: true, optional: true }), []);
  // shipped, or only partly a build tool: never
  assert.equal(problems("lightningcss", { license: "MPL-2.0" }).length, 1);
  assert.equal(problems("lightningcss", { license: "MPL-2.0", devOptional: true }).length, 1);
  // any other build tool under MPL-2.0 needs a decision of its own
  assert.equal(problems("some-bundler", { license: "MPL-2.0", dev: true }).length, 1);
  assert.equal(problems("lightningcss-plugin", { license: "MPL-2.0", dev: true }).length, 1, "only its platform builds");
  assert.equal(problems("not-lightningcss", { license: "MPL-2.0", dev: true }).length, 1);
  // lightningcss gets no other license than MPL-2.0 on top
  for (const lic of ["GPL-3.0-only", "AGPL-3.0-only", "LGPL-3.0-only", "MPL-1.1", "EPL-2.0"]) {
    assert.equal(problems("lightningcss", { license: lic, dev: true }).length, 1, lic);
    assert.equal(problems("some-tool", { license: lic, dev: true }).length, 1, lic);
  }
});

test("runtime packages need a notice, every package a license", () => {
  assert.deepEqual(problems("preact", { license: "MIT" }), []);
  assert.deepEqual(licenseProblems({ lockfileVersion: 3, packages: { "": {}, "node_modules/x": { license: "MIT" } } }, "").problems, [
    "x: runtime package missing in THIRD_PARTY_NOTICES.md",
  ]);
  assert.deepEqual(problems("x", { dev: true }), ["x: no license in package-lock.json"]);
  // CC-BY-4.0 is data a build tool reads, never shipped
  assert.deepEqual(problems("caniuse-lite", { license: "CC-BY-4.0", dev: true }), []);
  assert.equal(problems("caniuse-lite", { license: "CC-BY-4.0" }).length, 1);
  assert.equal(licenseProblems({ lockfileVersion: 1, dependencies: {} }, "").problems.length, 1);
  assert.equal(licenseProblems({ lockfileVersion: 3, packages: { "": {} } }, "").problems.length, 1);
});

test("the package a bare import names", () => {
  assert.equal(packageOf("preact"), "preact");
  assert.equal(packageOf("preact/hooks"), "preact");
  assert.equal(packageOf("@preact/signals"), "@preact/signals");
  assert.equal(packageOf("@scope/pkg/deep/file.js"), "@scope/pkg");
  assert.equal(packageOf("uplot/dist/uPlot.min.css"), "uplot");
  assert.equal(packageOf("./ui"), null);
  assert.equal(packageOf("../lang/de"), null);
  assert.equal(packageOf("/abs"), null);
  assert.equal(packageOf("node:fs"), "node:fs", "a Node module is no runtime dependency of the app either");
});

test("what the code of a file loads at run time", () => {
  const source = [
    'import { h } from "a";',
    'import b from "b";',
    'import * as c from "c";',
    "import {",
    "  d1,",
    "  d2,",
    '} from "d/sub";',
    'import "e/style.css";',
    'const f = await import("f");',
    'export { g } from "g";',
    'export * from "h";',
    'import { type I1, i2 } from "i";',
    'import typeJ from "j";',
    // type-only: erased by the build, nothing loads at run time
    'import type { K } from "k";',
    'export type { L } from "l";',
    // a type alias before an import must not make that import type-only
    "export type M = { m: number }",
    'import n from "n";',
    // no imports
    'const o = Array.from("o");',
    'const p = { from: "p" };',
    'import q from "./q";',
  ].join("\n");
  assert.deepEqual(runtimeImports(source).sort(), ["./q", "a", "b", "c", "d/sub", "e/style.css", "f", "g", "h", "i", "j", "n"]);
});

test("comments and look-alikes do not hide an import", () => {
  const source = [
    // a comment inside an import must not make it type-only
    "import {",
    "  a, // was: export type A",
    "  /* import type { B } */ b,",
    '} from "a";',
    // a default import that happens to be called "type"
    'import type from "t1";',
    'import type, { c } from "t2";',
    // type-only after all
    'import type T from "t3";',
    'import type * as N from "t4";',
    // dynamic imports with options or in backticks
    'const j = await import("pkg/data.json", { with: { type: "json" } });',
    "const k = await import(`tick`);",
    // commented out: no import
    '// import gone from "gone";',
    '/* import "gone-too"; */',
    // strings keep their slashes, the import after them still counts
    'const url = "https://example.org/*not-a-comment"; import s from "s";',
    // a key that ends in "import" or "from" in a text table loads nothing
    '  "config.import": "This file cannot be loaded ({reason})",',
    'export const m = { "a.from": "y" };',
  ].join("\n");
  assert.deepEqual(runtimeImports(source).sort(), ["a", "pkg/data.json", "s", "t1", "t2", "tick"]);
});

test("a path into node_modules names its package", () => {
  assert.equal(packageOf("../node_modules/vite/dist/client.js"), "vite");
  assert.equal(packageOf("/node_modules/@scope/pkg/x.js"), "@scope/pkg");
});

test("CSS imports count too", () => {
  const pkg = { dependencies: { uplot: "1.6.32" } };
  const css = (source) => importProblems([{ name: "styles.css", source }], pkg).problems;
  assert.deepEqual(css('@import "uplot/dist/uPlot.min.css";\n@import "./local.css";\n/* @import "gone"; */'), []);
  assert.equal(css('@import "other/x.css";').length, 1);
  assert.equal(css('@import url("other/y.css");').length, 1);
});

test("every package the app imports is a runtime dependency", () => {
  const pkg = { dependencies: { preact: "11.0.0", "@preact/signals": "2.11.3" }, devDependencies: { vite: "8.3.2" } };
  const file = (name, source) => ({ name, source });
  assert.deepEqual(
    importProblems([file("a.tsx", 'import { useState } from "preact/hooks";\nimport { signal } from "@preact/signals";'), file("b.ts", 'import x from "./a";')], pkg),
    { count: 2, problems: [] },
  );
  // a runtime import of a build tool: it would be bundled but checked as a build tool
  assert.deepEqual(importProblems([file("c.ts", 'import { defineConfig } from "vite";')], pkg).problems, [
    'vite: imported by web/src/c.ts, but not under "dependencies" in web/package.json',
  ]);
  assert.equal(importProblems([file("d.ts", 'import "unknown-css/x.css";')], pkg).problems.length, 1);
  // types only: nothing is bundled
  assert.deepEqual(importProblems([file("e.ts", 'import type { UserConfig } from "vite";')], pkg).problems, []);
  // moving a runtime package to devDependencies fails the check
  const moved = { dependencies: {}, devDependencies: { preact: "11.0.0" } };
  assert.equal(importProblems([file("f.tsx", 'import { render } from "preact";')], moved).problems.length, 1);
});

test("the check runs on the real lockfile and the real sources, and says so", () => {
  const script = fileURLToPath(new URL("./check_licenses.mjs", import.meta.url));
  const out = execFileSync(process.execPath, [script], { encoding: "utf8" });
  assert.match(out, /^License check: \d+ npm packages OK\.$/m);
  assert.match(out, /^Import check: the web app imports [1-9]\d* packages, all runtime dependencies\.$/m);
});
