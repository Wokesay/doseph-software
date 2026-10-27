// SPDX-License-Identifier: AGPL-3.0-or-later
// tools/signatures_report.mjs: why `npm audit signatures` failed.
//   node --test tools/signatures_report.test.mjs
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdirSync, mkdtempSync, rmSync, utimesSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { test } from "node:test";
import { describe, failedRunLog, npmCli, report } from "./signatures_report.mjs";

const invalidBabel = {
  name: "@babel/helper-validator-identifier",
  version: "7.29.7",
  location: "node_modules/@babel/helper-validator-identifier",
  resolved: "https://registry.npmjs.org/@babel/helper-validator-identifier/-/helper-validator-identifier-7.29.7.tgz",
  integrity: "sha512-qehx",
  predicateType: "https://slsa.dev/provenance/v1",
  code: "EATTESTATIONVERIFY",
  message: "artifact signature verification failed",
};

function tmp(t) {
  const dir = mkdtempSync(join(tmpdir(), "sig-"));
  t.after(() => rmSync(dir, { recursive: true, force: true }));
  return dir;
}

test("names each invalid or missing package with npm's code and message", () => {
  const lines = describe(JSON.stringify({ invalid: [invalidBabel], missing: [{ name: "left-pad", version: "1.3.0" }] }));
  assert.deepEqual(lines, [
    "invalid: @babel/helper-validator-identifier@7.29.7 (attestation https://slsa.dev/provenance/v1)",
    "  EATTESTATIONVERIFY: artifact signature verification failed",
    "  at node_modules/@babel/helper-validator-identifier",
    "  from https://registry.npmjs.org/@babel/helper-validator-identifier/-/helper-validator-identifier-7.29.7.tgz",
    "  integrity sha512-qehx",
    "missing: left-pad@1.3.0",
  ]);
});

test("a check that passes now is no clearance, and no JSON is said as such", () => {
  assert.deepEqual(describe(JSON.stringify({ invalid: [], missing: [] })), [
    "npm audit signatures passes now. If this job failed at the signature check, that did not repeat; a re-run is no clearance.",
  ]);
  assert.deepEqual(describe("npm error Failed to download"), ["npm gave no JSON:", "npm error Failed to download"]);
  assert.deepEqual(describe(""), ["npm gave no JSON."]);
});

test("npm that cannot check at all is not taken for a check that passed", () => {
  // What npm answers when it cannot reach Sigstore's TUF repository
  assert.deepEqual(describe(JSON.stringify({ error: { summary: "Failed to download", detail: "" } })), [
    "npm could not check the signatures: Failed to download",
  ]);
  assert.deepEqual(describe(JSON.stringify({ error: { code: "ENOTFOUND", summary: "getaddrinfo", detail: "tuf-repo-cdn.sigstore.dev" } })), [
    "npm could not check the signatures: ENOTFOUND: getaddrinfo",
    "  tuf-repo-cdn.sigstore.dev",
  ]);
  for (const other of ["null", "{}", JSON.stringify({ invalid: "x" })]) assert.deepEqual(describe(other), [`npm gave no report: ${other}`], other);
});

test("text from npm is one line with its command markers broken up; odd and many entries stay readable", () => {
  // The fence against workflow commands is ::stop-commands:: around the whole
  // output (the last test); this is the layer under it.
  const sneaky = { ...invalidBabel, message: "x\n::add-mask::invalid\r\n##[warning]re-run, a flake\u202e" };
  const lines = describe(JSON.stringify({ invalid: [sneaky, null, "text"], missing: [] }));
  for (const l of lines) assert.doesNotMatch(l, /::|##\[|[\u0000-\u001f\u202e]/, l);
  assert.ok(lines.includes("  EATTESTATIONVERIFY: x : :add-mask: :invalid ## [warning]re-run, a flake"), lines.join("\n"));
  assert.ok(lines.includes("invalid: null") && lines.includes('invalid: "text"'));
  assert.deepEqual(describe(JSON.stringify({ error: { summary: "a\n::error::b", detail: "c\n::stop-commands::d" } })), [
    "npm could not check the signatures: a : :error: :b",
    "  c : :stop-commands: :d",
  ]);
  const many = describe(JSON.stringify({ invalid: Array.from({ length: 250 }, (_, i) => ({ name: `p${i}`, version: "1.0.0", message: "m".repeat(9000) })), missing: [] }));
  assert.equal(many.at(-1), "… and 50 more");
  assert.ok(many.every((l) => l.length <= 4002));
});

test("npm's log of the check shows the failed run, or that the check passed and the job failed elsewhere", (t) => {
  const logs = tmp(t);
  const at = (file, text, seconds) => {
    writeFileSync(join(logs, file), text);
    utimesSync(join(logs, file), seconds, seconds);
  };
  at("1-debug-0.log", "0 verbose title npm ci\n1 verbose exit 0\n", 100);
  at("2-debug-0.log", "0 verbose title npm audit signatures\n1 http fetch GET 200 https://registry.npmjs.org/-/npm/v1/keys\n2 verbose stack Error: artifact signature verification failed\n3 verbose exit 1\n", 200);
  at("3-debug-0.log", "0 verbose title npm run build\n1 verbose exit 0\n", 300);
  assert.deepEqual(failedRunLog(logs, 1_000_000), [
    "End of npm's log of the check (2-debug-0.log, exit 1):",
    "  0 verbose title npm audit signatures",
    "  1 http fetch GET 200 https://registry.npmjs.org/-/npm/v1/keys",
    "  2 verbose stack Error: artifact signature verification failed",
    "  3 verbose exit 1",
  ]);
  // A log written after the report started is the report's own
  at("4-debug-0.log", "0 verbose title npm audit signatures\n1 verbose exit 0\n", 2000);
  assert.equal(failedRunLog(logs, 1_000_000)[0], "End of npm's log of the check (2-debug-0.log, exit 1):");
  assert.deepEqual(failedRunLog(logs, 3_000_000), ["The signature check passed in this job (npm log 4-debug-0.log); the job failed elsewhere."]);
  assert.match(failedRunLog(join(logs, "none"), 1)[0], /^No npm logs in /);
  const empty = tmp(t);
  assert.deepEqual(failedRunLog(empty, 1), ["No npm log of a signature check found; the job may have failed before it, or elsewhere."]);
  // Logs older than this job's npm ci come from a cache: not this job's check
  assert.deepEqual(failedRunLog(logs, 1_000_000, 250_000), ["No npm log of a signature check found; the job may have failed before it, or elsewhere."]);
  writeFileSync(join(logs, "5-debug-0.log"), "unreadable");
  utimesSync(join(logs, "5-debug-0.log"), 500, 500);
  rmSync(join(logs, "5-debug-0.log"));
  mkdirSync(join(logs, "6-debug-0.log"));
  utimesSync(join(logs, "6-debug-0.log"), 600, 600);
  assert.equal(failedRunLog(logs, 1_000_000)[0], "End of npm's log of the check (2-debug-0.log, exit 1):");
});

test("the report asks npm in the web folder and says when npm cannot be started", (t) => {
  const web = tmp(t);
  mkdirSync(join(web, "node_modules"));
  const logs = tmp(t);
  const calls = [];
  const npm = (args, cwd) => {
    calls.push([args.join(" "), cwd]);
    if (args[0] === "-v") return { out: "10.9.9\n", err: "", status: 0 };
    return { out: JSON.stringify({ invalid: [invalidBabel], missing: [] }), err: "npm warn x\n", status: 1 };
  };
  const lines = [];
  report(web, { npm, logsDir: logs, now: 1, print: (l) => lines.push(l) });
  assert.deepEqual(calls, [
    ["-v", web],
    ["audit signatures --json", web],
  ]);
  // npm's log of the failed run comes first, before anything can take long
  assert.equal(lines[0], "No npm log of a signature check found; the job may have failed before it, or elsewhere.");
  assert.equal(lines[1], `Node ${process.version}, npm 10.9.9`);
  assert.ok(lines.includes("Asked again now:"));
  assert.ok(lines.includes("  invalid: @babel/helper-validator-identifier@7.29.7 (attestation https://slsa.dev/provenance/v1)"));
  assert.ok(lines.includes("  npm warn x"));
  const gone = [];
  report(web, { npm: () => ({ out: "", err: "", status: null, error: Object.assign(new Error("spawn npm ENOENT"), { code: "ENOENT" }) }), logsDir: logs, print: (l) => gone.push(l) });
  assert.ok(gone.includes("  npm could not be started or did not finish: ENOENT"), gone.join("\n"));
});

test("the npm next to the running Node is found without PATH, also on Windows", () => {
  assert.equal(npmCli("/opt/node/bin/node", "linux"), join("/opt/node/bin", "..", "lib", "node_modules", "npm", "bin", "npm-cli.js"));
  assert.equal(npmCli(join("C:", "node", "node.exe"), "win32"), join("C:", "node", "node_modules", "npm", "bin", "npm-cli.js"));
});

test("run as a step, it fences its output against workflow commands and never fails the job", (t) => {
  const empty = tmp(t);
  const r = spawnSync(process.execPath, [join(import.meta.dirname, "signatures_report.mjs"), empty], { encoding: "utf8" });
  assert.equal(r.status, 0);
  const out = r.stdout.trim().split("\n");
  const token = out[0].match(/^::stop-commands::([0-9a-f-]{36})$/)?.[1];
  assert.ok(token, out[0]);
  assert.equal(out.at(-1), `::${token}::`);
  assert.match(r.stdout, /nothing to report/);
});
