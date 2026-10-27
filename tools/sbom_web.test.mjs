// SPDX-License-Identifier: AGPL-3.0-or-later
// tools/sbom_web.mjs: the web SBOM lists exactly the runtime packages of
// web/package-lock.json. `npm sbom --omit dev` would leave out preact, a
// runtime package that a build tool has as a peer.
//   node --test tools/sbom_web.test.mjs
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { test } from "node:test";
import { runtimePackages, webSbom } from "./sbom_web.mjs";

const root = join(import.meta.dirname, "..");
const lock = {
  lockfileVersion: 3,
  packages: {
    "": { name: "doseph-web", version: "0.1.0" },
    "node_modules/preact": { version: "11.0.0" },
    "node_modules/@preact/signals": { version: "2.11.3" },
    "node_modules/vite": { version: "8.3.2", dev: true },
    "node_modules/x": { version: "1.0.0", dev: true },
    // a dev dependency that a runtime package has as an optional peer:
    // installed by `npm ci --omit=dev`, so runtime
    "node_modules/y": { version: "2.0.0", devOptional: true },
    // runtime binaries for another platform, not installed here
    "node_modules/z-win32": { version: "1.0.0", optional: true },
    "node_modules/w-win32": { version: "1.0.0", devOptional: true },
    "node_modules/@preact/signals/node_modules/preact": { version: "10.0.0" },
    "node_modules/local": { resolved: "../local", link: true },
  },
};
// As npm writes them: scope optional for optional and devOptional ones, the
// development property only for dev ones
const component = (name, version, path, { dev = false, optional = false } = {}) => ({
  "bom-ref": `${name}@${version}`,
  type: "library",
  name,
  version,
  scope: dev || optional ? "optional" : "required",
  properties: [{ name: "cdx:npm:package:path", value: path }, ...(dev ? [{ name: "cdx:npm:package:development", value: "true" }] : [])],
});
const full = {
  bomFormat: "CycloneDX",
  metadata: { component: { "bom-ref": "doseph-web@0.1.0", name: "web" } },
  components: [
    component("preact", "11.0.0", "node_modules/preact"),
    component("@preact/signals", "2.11.3", "node_modules/@preact/signals"),
    component("vite", "8.3.2", "node_modules/vite", { dev: true, optional: true }),
    component("x", "1.0.0", "node_modules/x", { dev: true, optional: true }),
    component("y", "2.0.0", "node_modules/y", { optional: true }),
    component("preact", "10.0.0", "node_modules/@preact/signals/node_modules/preact"),
  ],
  dependencies: [
    { ref: "doseph-web@0.1.0", dependsOn: ["@preact/signals@2.11.3", "preact@11.0.0", "vite@8.3.2"] },
    { ref: "@preact/signals@2.11.3", dependsOn: ["preact@10.0.0"] },
    { ref: "preact@11.0.0", dependsOn: ["y@2.0.0"] },
    { ref: "vite@8.3.2", dependsOn: ["x@1.0.0", "preact@11.0.0"] },
  ],
};
const kept = (sbom) => sbom.components.map((c) => c.properties.find((p) => p.name === "cdx:npm:package:path").value);
// npm's SBOM of the real lockfile, as the release takes it
const npmSbom = () => {
  const r = spawnSync("npm", ["sbom", "--sbom-format", "cyclonedx", "--package-lock-only", "--offline"], {
    cwd: join(root, "web"),
    encoding: "utf8",
    maxBuffer: 64 * 1024 * 1024,
  });
  assert.equal(r.status, 0, r.stderr);
  return r.stdout;
};

test("the runtime packages of the lockfile: everything not dev, devOptional included, nested ones by their own path, no links", () => {
  assert.deepEqual([...runtimePackages(lock).keys()], [
    "node_modules/preact",
    "node_modules/@preact/signals",
    "node_modules/y",
    "node_modules/z-win32",
    "node_modules/w-win32",
    "node_modules/@preact/signals/node_modules/preact",
  ]);
  assert.throws(() => runtimePackages({ lockfileVersion: 1, dependencies: {} }), /lockfile version 2 or later/);
});

test("the SBOM keeps the runtime packages as npm wrote them, preact included, and drops the build tools and their edges", () => {
  const sbom = webSbom(full, lock);
  assert.deepEqual(kept(sbom), ["node_modules/preact", "node_modules/@preact/signals", "node_modules/y", "node_modules/@preact/signals/node_modules/preact"]);
  for (const c of sbom.components) assert.deepEqual(c, full.components.find((f) => f["bom-ref"] === c["bom-ref"] && f.properties[0].value === c.properties[0].value));
  assert.deepEqual(sbom.dependencies, [
    { ref: "doseph-web@0.1.0", dependsOn: ["@preact/signals@2.11.3", "preact@11.0.0"] },
    { ref: "@preact/signals@2.11.3", dependsOn: ["preact@10.0.0"] },
    { ref: "preact@11.0.0", dependsOn: ["y@2.0.0"] },
  ]);
  assert.deepEqual(sbom.metadata, full.metadata);
  assert.equal(full.components.length, 6, "the input is not changed");
});

test("it fails when npm and the lockfile disagree, rather than write a wrong SBOM", () => {
  const without = (ref) => ({ ...full, components: full.components.filter((c) => c["bom-ref"] !== ref) });
  assert.throws(() => webSbom(without("preact@11.0.0"), lock), /preact@11\.0\.0 \(node_modules\/preact\) is a runtime package of web\/package-lock\.json but not in npm's SBOM/);
  const devMarked = { ...full, components: full.components.map((c) => (c["bom-ref"] === "preact@11.0.0" ? component("preact", "11.0.0", "node_modules/preact", { dev: true }) : c)) };
  assert.throws(() => webSbom(devMarked, lock), /npm marks preact@11\.0\.0 \(node_modules\/preact\) as development, the lockfile does not/);
  const pathless = { ...full, components: [...full.components, { "bom-ref": "q@1.0.0", name: "q", version: "1.0.0" }] };
  assert.throws(() => webSbom(pathless, lock), /q@1\.0\.0 has no cdx:npm:package:path/);
  const nested = "node_modules/@preact/signals/node_modules/preact";
  const twice = { ...full, components: full.components.map((c) => (c.properties[0].value === nested ? component("preact", "11.0.0", nested) : c)) };
  assert.throws(() => webSbom(twice, { ...lock, packages: { ...lock.packages, [nested]: { version: "11.0.0" } } }), /preact@11\.0\.0 twice/);
  // A tree that is not the lockfile's
  const other = { ...full, components: full.components.map((c) => (c["bom-ref"] === "preact@11.0.0" ? component("preact", "11.0.1", "node_modules/preact") : c)) };
  assert.throws(() => webSbom(other, lock), /npm lists preact@11\.0\.1 at node_modules\/preact, web\/package-lock\.json has preact@11\.0\.0/);
  // A build tool's copy with the same name and version leaves a second
  // edge list for the same reference
  const shared = { ...full, dependencies: [...full.dependencies, { ref: "preact@11.0.0", dependsOn: ["x@1.0.0"] }] };
  assert.throws(() => webSbom(shared, lock), /two dependency entries for preact@11\.0\.0/);
});

test("the SBOM names the filter among its tools", () => {
  const sbom = webSbom({ ...full, metadata: { ...full.metadata, tools: [{ vendor: "npm", name: "cli", version: "10.9.9" }] } }, lock);
  assert.deepEqual(sbom.metadata.tools, [
    { vendor: "npm", name: "cli", version: "10.9.9" },
    { vendor: "doseph-software", name: "tools/sbom_web.mjs" },
  ]);
});

test("on the real lockfile: npm's SBOM filtered is exactly the runtime packages, preact among them", () => {
  const realLock = JSON.parse(readFileSync(join(root, "web", "package-lock.json"), "utf8"));
  const sbom = webSbom(JSON.parse(npmSbom()), realLock);
  assert.deepEqual(kept(sbom).sort(), [...runtimePackages(realLock).keys()].filter((p) => !realLock.packages[p].optional && !realLock.packages[p].devOptional).sort());
  assert.ok(kept(sbom).includes("node_modules/preact"));
  // the build tools stay out
  assert.ok(!kept(sbom).includes("node_modules/vite"));
});

test("run as a step, it reads npm's SBOM from stdin and writes the web SBOM", () => {
  const script = join(root, "tools", "sbom_web.mjs");
  const lockPath = join(root, "web", "package-lock.json");
  const input = npmSbom();
  const r = spawnSync(process.execPath, [script, lockPath], { input, encoding: "utf8", maxBuffer: 64 * 1024 * 1024 });
  assert.equal(r.status, 0, r.stderr);
  assert.ok(JSON.parse(r.stdout).components.some((c) => c.name === "preact"));
  const bad = spawnSync(process.execPath, [script, lockPath], { input: "", encoding: "utf8" });
  assert.notEqual(bad.status, 0, "no input is no SBOM");
  assert.equal(bad.stdout, "");
  // An error inside the filter fails the step and writes nothing
  const all = JSON.parse(input);
  const withoutPreact = { ...all, components: all.components.filter((c) => !c.properties.some((p) => p.value === "node_modules/preact")) };
  const missing = spawnSync(process.execPath, [script, lockPath], { input: JSON.stringify(withoutPreact), encoding: "utf8" });
  assert.notEqual(missing.status, 0);
  assert.equal(missing.stdout, "");
  assert.match(missing.stderr, /preact@[\d.]+ \(node_modules\/preact\) is a runtime package/);
});

test("the release builds the web SBOM with it, from the lockfile, not with --omit", () => {
  const release = readFileSync(join(root, ".github", "workflows", "release.yml"), "utf8");
  assert.match(
    release,
    /\n {10}\(cd web && npm sbom --sbom-format cyclonedx --package-lock-only\) \| node tools\/sbom_web\.mjs web\/package-lock\.json > out\/dist\/sbom-web\.cdx\.json\n/,
  );
  assert.doesNotMatch(release, /--omit\b/);
  assert.equal(release.match(/> out\/dist\/sbom-web\.cdx\.json/g)?.length, 1, "one line writes the web SBOM");
  assert.match(release, /^ {6}- tools\/sbom_web\.mjs$/m, "a change to it runs the release dry run");
});
