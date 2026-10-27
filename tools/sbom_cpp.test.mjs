// SPDX-License-Identifier: AGPL-3.0-or-later
// Tests for the SBOM of the simulator's C++ dependencies (tools/sbom_cpp.mjs).
//   node --test tools/sbom_cpp.test.mjs
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { test } from "node:test";
import { fileURLToPath } from "node:url";
import { parseDeps, sbom } from "./sbom_cpp.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const deps = readFileSync(join(here, "..", "cmake", "deps.cmake"), "utf8");
const JSON_SHA = "9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6";

test("every pinned header of cmake/deps.cmake is found with version and hash", () => {
  const found = parseDeps(deps);
  assert.deepEqual(
    found.map((d) => d.name),
    ["nlohmann_json", "cpp_httplib", "doctest"],
  );
  for (const d of found) {
    assert.match(d.version, /^\d+\.\d+\.\d+$/, d.name);
    assert.match(d.sha256, /^[0-9a-f]{64}$/, d.name);
  }
  assert.equal(found[0].sha256, JSON_SHA);
});

test("a call that cannot be read stops the SBOM instead of dropping out", () => {
  const upper = deps.replace(JSON_SHA, JSON_SHA.toUpperCase());
  assert.throws(() => parseDeps(upper), /3 dp_fetch_header calls, 2 understood/);
  const noVersion = deps.replace("nlohmann/json/v3.11.3/", "nlohmann/json/develop/");
  assert.throws(() => parseDeps(noVersion), /nlohmann_json: no GitHub release URL/);
});

test("the SBOM lists the shipped libraries, not the test framework", () => {
  const bom = sbom(deps, "1.2.3-proto.1");
  assert.equal(bom.bomFormat, "CycloneDX");
  assert.equal(bom.metadata.component.name, "doseph-simulator");
  assert.equal(bom.metadata.component.version, "1.2.3-proto.1");
  const byName = Object.fromEntries(bom.components.map((c) => [c.name, c]));
  assert.deepEqual(Object.keys(byName).sort(), ["cpp-httplib", "nlohmann-json"]);
  const json = byName["nlohmann-json"];
  assert.equal(json.version, "3.11.3");
  assert.equal(json.purl, "pkg:github/nlohmann/json@v3.11.3");
  assert.equal(json.hashes[0].alg, "SHA-256");
  assert.equal(json.hashes[0].content, JSON_SHA);
  // As in THIRD_PARTY_NOTICES.md: MIT with parts under CC0 and Apache-2.0
  assert.deepEqual(json.licenses, [{ expression: "MIT AND CC0-1.0 AND Apache-2.0" }]);
  const httplib = byName["cpp-httplib"];
  assert.deepEqual(httplib.licenses, [{ license: { id: "MIT" } }]);
  assert.equal(httplib.purl, `pkg:github/yhirose/cpp-httplib@v${httplib.version}`);
});

test("an unknown dependency stops the build instead of an incomplete SBOM", () => {
  const extra = `${deps}\ndp_fetch_header(newlib\n  "https://raw.githubusercontent.com/x/newlib/v1.0.0/newlib.h"\n  ${"a".repeat(64)}\n  "newlib.h")\n`;
  assert.throws(() => sbom(extra, "1.0.0"), /newlib/);
});

test("run as a step, it writes the SBOM for the version it is given, and fails without one", () => {
  const script = join(here, "sbom_cpp.mjs");
  const r = spawnSync(process.execPath, [script, "0.1.0-proto.5"], { encoding: "utf8" });
  assert.equal(r.status, 0, r.stderr);
  const bom = JSON.parse(r.stdout);
  assert.equal(bom.metadata.component.version, "0.1.0-proto.5");
  assert.equal(bom.components.length, 2);
  const none = spawnSync(process.execPath, [script], { encoding: "utf8" });
  assert.equal(none.status, 2);
  assert.equal(none.stdout, "");
});
