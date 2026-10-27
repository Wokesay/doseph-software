// SPDX-License-Identifier: AGPL-3.0-or-later
// Tests of the provenance check: its rules (tools/provenance_policy.mjs)
// and the check itself (tools/check_provenance.mjs) against a stand-in
// registry, so a version without provenance after one with provenance fails.
//   node --test tools/check_provenance.test.mjs
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { createServer } from "node:http";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { after, test } from "node:test";
import { fileURLToPath } from "node:url";
import { compareVersions, lockedPackages, previousVersion, provenanceProblems, repositoryOf, sourceOf } from "./provenance_policy.mjs";

const script = fileURLToPath(new URL("./check_provenance.mjs", import.meta.url));
const tmp = mkdtempSync(join(tmpdir(), "doseph-provenance-"));
after(() => rmSync(tmp, { recursive: true, force: true }));

// An npm attestation bundle whose SLSA provenance names `repository`.
const bundle = (statement) => ({
  attestations: [
    { predicateType: "https://github.com/npm/attestation/tree/main/specs/publish/v0.1", bundle: { dsseEnvelope: { payload: "" } } },
    {
      predicateType: "https://slsa.dev/provenance/v1",
      bundle: { dsseEnvelope: { payload: Buffer.from(JSON.stringify(statement)).toString("base64") } },
    },
  ],
});
const v1 = (repository, id) =>
  bundle({
    predicate: {
      buildDefinition: { externalParameters: { workflow: { repository } }, internalParameters: id ? { github: { repository_id: id } } : {} },
    },
  });

test("versions in SemVer order; the previous one skips pre-releases unless the locked one is one", () => {
  for (const [a, b] of [
    ["1.0.0", "1.0.1"],
    ["1.9.0", "1.10.0"],
    ["1.0.0-rc.1", "1.0.0"],
    ["1.0.0-rc.2", "1.0.0-rc.10"],
    ["1.0.0-alpha", "1.0.0-beta"],
    ["1.0.0-1", "1.0.0-alpha"],
    ["1.0.0-alpha", "1.0.0-alpha.1"],
  ]) {
    assert.ok(compareVersions(a, b) < 0, `${a} < ${b}`);
    assert.ok(compareVersions(b, a) > 0, `${b} > ${a}`);
  }
  assert.equal(compareVersions("1.0.0+build.1", "1.0.0"), 0);
  const versions = ["0.9.0", "1.0.0", "1.0.1", "1.1.0-rc.1", "1.1.0", "2.0.0", "not-a-version"];
  assert.equal(previousVersion(versions, "1.1.0"), "1.0.1");
  assert.equal(previousVersion(versions, "2.0.0"), "1.1.0");
  assert.equal(previousVersion(["1.1.0-rc.1", "1.1.0-rc.2", "1.0.0"], "1.1.0-rc.2"), "1.1.0-rc.1");
  assert.equal(previousVersion(versions, "0.9.0"), null);
  // the highest lower one that has provenance, past any without
  const has = new Set(["0.9.0", "1.0.0"]);
  assert.equal(previousVersion(versions, "2.0.0", (v) => has.has(v)), "1.0.0");
  assert.equal(previousVersion(versions, "0.9.0", (v) => has.has(v)), null);
});

test("the source repository of a provenance, in both SLSA versions, without ref or .git", () => {
  assert.equal(repositoryOf(v1("https://github.com/preactjs/preact")), "https://github.com/preactjs/preact");
  assert.equal(repositoryOf(v1("https://GitHub.com/Org/Repo.git/")), "https://github.com/org/repo");
  const v02 = (uri) => bundle({ predicate: { invocation: { configSource: { uri } } } });
  assert.equal(repositoryOf(v02("git+https://github.com/preactjs/prefresh@refs/heads/main")), "https://github.com/preactjs/prefresh");
  assert.equal(repositoryOf(v02("git+https://github.com/a/b.git@4f1c2d3e")), "https://github.com/a/b");
  assert.equal(repositoryOf(bundle({ predicate: {} })), "");
  assert.equal(repositoryOf({ attestations: [{ predicateType: "https://slsa.dev/provenance/v1", bundle: { dsseEnvelope: { payload: "not json" } } }] }), "");
  assert.equal(repositoryOf(undefined), "");
  assert.deepEqual(sourceOf(v1("https://github.com/o/r", 42)), { repository: "https://github.com/o/r", repositoryId: "42" });
  assert.deepEqual(sourceOf(v02("git+https://github.com/o/r@refs/heads/main")), { repository: "https://github.com/o/r", repositoryId: "" });
});

test("a dropped provenance or another source repository is a problem, nothing else is", () => {
  const base = { name: "x", version: "1.0.2", previous: "1.0.0" };
  const r = (repository, repositoryId = "") => ({ repository, repositoryId });
  assert.deepEqual(provenanceProblems({ ...base, lockedHas: true, locked: r("a", "1"), earlier: r("a", "1") }), []);
  assert.deepEqual(provenanceProblems({ ...base, previous: null, lockedHas: false }), [], "no lower version had any");
  assert.deepEqual(provenanceProblems({ ...base, lockedHas: true, locked: r("a"), earlier: r("a", "1") }), [], "an id only on one side");
  assert.match(provenanceProblems({ ...base, lockedHas: false })[0], /x@1\.0\.2 has no provenance, but 1\.0\.0 had/);
  assert.match(provenanceProblems({ ...base, lockedHas: true, locked: r("a"), earlier: r("b") })[0], /from a, but 1\.0\.0 from b/);
  assert.match(provenanceProblems({ ...base, lockedHas: true, locked: r(""), earlier: r("b") })[0], /names no source repository/);
  // what cannot be read fails rather than passes
  assert.match(provenanceProblems({ ...base, lockedHas: true, locked: r("a"), earlier: r("") })[0], /cannot read where 1\.0\.0 was built/);
  assert.match(provenanceProblems({ ...base, lockedHas: true, locked: r(""), earlier: r("") })[0], /cannot read where 1\.0\.0 was built/);
  // the same name, but another repository behind it: a freed name taken over
  assert.match(provenanceProblems({ ...base, lockedHas: true, locked: r("a", "2"), earlier: r("a", "1") })[0], /from a with repository id 2, but 1\.0\.0 with id 1/);
});

test("the locked packages: every installed one, also nested, not the project or a link", () => {
  const lock = {
    packages: {
      "": { name: "app" },
      "node_modules/a": { version: "1.0.0" },
      "node_modules/@s/b": { version: "2.0.0" },
      "node_modules/a/node_modules/c": { version: "3.0.0" },
      "node_modules/l": { link: true },
    },
  };
  assert.deepEqual(
    lockedPackages(lock).map(({ name, version }) => `${name}@${version}`),
    ["a@1.0.0", "@s/b@2.0.0", "c@3.0.0"],
  );
});

// A stand-in registry: abbreviated packuments and attestation bundles.
async function registry(packages) {
  const server = createServer((req, res) => {
    const url = decodeURIComponent(req.url);
    if (state.redirects[url]) {
      res.writeHead(302, { Location: state.redirects[url] }).end();
      return;
    }
    const att = /^\/-\/npm\/v1\/attestations\/(.+)$/.exec(url);
    const body = att ? state.bundles[att[1]] : state.packages[url.slice(1)];
    if (!body) {
      res.writeHead(404).end();
      return;
    }
    res.writeHead(200, { "Content-Type": "application/json" }).end(JSON.stringify(body));
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const base = `http://127.0.0.1:${server.address().port}`;
  const state = { packages: {}, bundles: {}, redirects: {} };
  for (const [name, versions] of Object.entries(packages)) {
    state.packages[name] = { name, versions: {} };
    for (const [version, repo] of Object.entries(versions)) {
      const dist = { integrity: "sha512-x" };
      if (repo !== null) {
        dist.attestations = { url: `${base}/-/npm/v1/attestations/${name}@${version}`, provenance: { predicateType: "https://slsa.dev/provenance/v1" } };
        state.bundles[`${name}@${version}`] = Array.isArray(repo) ? v1(...repo) : v1(repo);
      }
      state.packages[name].versions[version] = { name, version, dist };
    }
  }
  return { base, state, close: () => server.close() };
}

// Runs the check against `reg` with a lockfile of `locked` and `accepted` exceptions.
async function check(reg, locked, accepted) {
  const dir = mkdtempSync(join(tmp, "case-"));
  const lock = { packages: { "": { name: "web" } } };
  for (const [name, version] of Object.entries(locked)) lock.packages[`node_modules/${name}`] = { version };
  writeFileSync(join(dir, "lock.json"), JSON.stringify(lock));
  const args = [script, "--lock", join(dir, "lock.json"), "--registry", reg.base, "--exceptions", join(dir, "exceptions.json")];
  if (accepted) writeFileSync(join(dir, "exceptions.json"), JSON.stringify({ accepted }));
  const child = spawn(process.execPath, args, { stdio: ["ignore", "pipe", "pipe"] });
  let out = "";
  child.stdout.on("data", (c) => (out += c));
  child.stderr.on("data", (c) => (out += c));
  const code = await new Promise((resolve) => child.on("exit", resolve));
  return { code, out };
}

test("a version without provenance after one with provenance fails; one that never had any passes", async () => {
  const reg = await registry({
    hijacked: { "1.0.0": "https://github.com/o/hijacked", "1.0.1": null },
    twice: { "1.0.0": "https://github.com/o/twice", "1.0.1": null, "1.0.2": null },
    plain: { "1.0.0": null, "1.0.1": null },
    "@s/kept": { "1.0.0": "https://github.com/o/kept", "1.1.0-rc.1": null, "1.1.0": "https://github.com/o/kept" },
  });
  try {
    const bad = await check(reg, { hijacked: "1.0.1", plain: "1.0.1", "@s/kept": "1.1.0" });
    assert.equal(bad.code, 1, bad.out);
    assert.match(bad.out, /hijacked@1\.0\.1 has no provenance, but 1\.0\.0 had/);
    // a second release by hand does not hide behind the first
    const twice = await check(reg, { twice: "1.0.2" });
    assert.equal(twice.code, 1, twice.out);
    assert.match(twice.out, /twice@1\.0\.2 has no provenance, but 1\.0\.0 had/);
    assert.doesNotMatch(bad.out, /plain|@s\/kept/);
    const good = await check(reg, { plain: "1.0.1", "@s/kept": "1.1.0" });
    assert.equal(good.code, 0, good.out);
    assert.match(good.out, /2 npm packages, 1 with provenance/);
  } finally {
    reg.close();
  }
});

test("another repository fails unless that very case is accepted; an accepted case no package has fails", async () => {
  const reg = await registry({
    moved: { "1.0.0": "https://github.com/o/old", "1.0.1": "https://github.com/o/new" },
    taken: { "1.0.0": ["https://github.com/o/taken", 1], "1.0.1": ["https://github.com/o/taken", 2] },
  });
  const problem = "moved@1.0.1 has provenance from https://github.com/o/new, but 1.0.0 from https://github.com/o/old";
  const ok = { "moved@1.0.1": { problem, reason: "moved within the organisation, reviewed" } };
  try {
    const moved = await check(reg, { moved: "1.0.1" });
    assert.equal(moved.code, 1, moved.out);
    assert.ok(moved.out.includes(problem), moved.out);
    const accepted = await check(reg, { moved: "1.0.1" }, ok);
    assert.equal(accepted.code, 0, accepted.out);
    assert.match(accepted.out, /Accepted: moved@1\.0\.1 \(moved within the organisation, reviewed\)/);
    // accepted for another move: this one still fails
    const other = await check(reg, { moved: "1.0.1" }, { "moved@1.0.1": { problem: problem.replace("o/new", "o/other"), reason: "r" } });
    assert.equal(other.code, 1, other.out);
    assert.match(other.out, /accepts another one: moved@1\.0\.1 has provenance from https:\/\/github\.com\/o\/other/);
    const stale = await check(reg, { moved: "1.0.0" }, ok);
    assert.equal(stale.code, 1, stale.out);
    assert.match(stale.out, /moved@1\.0\.1 is accepted .* but no locked package has that problem any more/);
    const taken = await check(reg, { taken: "1.0.1" });
    assert.equal(taken.code, 1, taken.out);
    assert.match(taken.out, /with repository id 2, but 1\.0\.0 with id 1/);
  } finally {
    reg.close();
  }
});

test("no answer from the registry, a version it does not know or an attestation elsewhere fails", async () => {
  const reg = await registry({ known: { "1.0.0": "https://github.com/o/k", "1.0.1": "https://github.com/o/k" } });
  try {
    const missing = await check(reg, { unknown: "1.0.0" });
    assert.equal(missing.code, 1, missing.out);
    assert.match(missing.out, /no usable answer from the registry: .*HTTP 404/);
    const version = await check(reg, { known: "9.9.9" }, { "known@9.9.9": { problem: "p", reason: "r" } });
    assert.equal(version.code, 1, version.out);
    assert.match(version.out, /known@9\.9\.9 is not on/);
    assert.doesNotMatch(version.out, /remove it/, "a version the registry lacks is not a stale exception");
    const reasonless = await check(reg, { known: "1.0.1" }, { "known@1.0.1": { problem: "p" } });
    assert.equal(reasonless.code, 1, reasonless.out);
    assert.match(reasonless.out, /known@1\.0\.1 in .* needs the exact "problem" it accepts and a "reason"/);
    // an attestation that redirects elsewhere is not followed
    // (to the same bundle under another origin, which would pass if followed)
    reg.state.redirects["/-/npm/v1/attestations/known@1.0.1"] = `${reg.base.replace("127.0.0.1", "localhost")}/-/npm/v1/attestations/known@1.0.0`;
    const redirected = await check(reg, { known: "1.0.1" });
    assert.equal(redirected.code, 1, redirected.out);
    assert.match(redirected.out, /no usable answer from the registry: .*redirect/i);
    delete reg.state.redirects["/-/npm/v1/attestations/known@1.0.1"];
    reg.state.packages.known.versions["1.0.1"].dist.attestations.url = "https://elsewhere.example/attestation";
    const elsewhere = await check(reg, { known: "1.0.1" });
    assert.equal(elsewhere.code, 1, elsewhere.out);
    assert.match(elsewhere.out, /https:\/\/elsewhere\.example\/attestation is not on/);
  } finally {
    reg.close();
  }
  const empty = await check({ base: "http://127.0.0.1:9" }, {});
  assert.equal(empty.code, 1, empty.out);
  assert.match(empty.out, /no packages found in the lockfile/);
  // without an answer nothing says an accepted case is no longer needed
  const down = await check({ base: "http://127.0.0.1:9" }, { moved: "1.0.1" }, { "moved@1.0.1": { problem: "p", reason: "reviewed" } });
  assert.equal(down.code, 1, down.out);
  assert.match(down.out, /no usable answer from the registry/);
  assert.doesNotMatch(down.out, /remove it/);
});

test("the previous version is the release before, not a pre-release; both provenance formats compare", async () => {
  const reg = await registry({
    rc: { "1.0.0": "https://github.com/o/rc", "1.1.0-rc.1": null, "1.1.0": null },
    formats: { "1.0.0": "https://github.com/o/formats", "1.0.1": "https://github.com/o/formats" },
  });
  // 1.0.0 was published by the workflow's older SLSA format
  reg.state.bundles["formats@1.0.0"] = bundle({ predicate: { invocation: { configSource: { uri: "git+https://github.com/o/formats@refs/heads/main" } } } });
  try {
    const rc = await check(reg, { rc: "1.1.0" });
    assert.equal(rc.code, 1, rc.out);
    assert.match(rc.out, /rc@1\.1\.0 has no provenance, but 1\.0\.0 had/);
    const formats = await check(reg, { formats: "1.0.1" });
    assert.equal(formats.code, 0, formats.out);
  } finally {
    reg.close();
  }
});

test("a package the lockfile nests twice is asked about once", async () => {
  const reg = await registry({ twin: { "1.0.0": "https://github.com/o/twin", "1.0.1": "https://github.com/o/twin" } });
  try {
    const dir = mkdtempSync(join(tmp, "twin-"));
    const lock = {
      packages: { "": {}, "node_modules/twin": { version: "1.0.1" }, "node_modules/a/node_modules/twin": { version: "1.0.1" } },
    };
    writeFileSync(join(dir, "lock.json"), JSON.stringify(lock));
    const child = spawn(process.execPath, [script, "--lock", join(dir, "lock.json"), "--registry", reg.base, "--exceptions", join(dir, "none.json")], {
      stdio: ["ignore", "pipe", "pipe"],
    });
    let out = "";
    child.stdout.on("data", (c) => (out += c));
    child.stderr.on("data", (c) => (out += c));
    assert.equal(await new Promise((resolve) => child.on("exit", resolve)), 0, out);
    assert.match(out, /1 npm packages, 1 with provenance/);
  } finally {
    reg.close();
  }
});
