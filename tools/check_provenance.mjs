// SPDX-License-Identifier: AGPL-3.0-or-later
// Provenance check of the web app's npm packages (CI, before the packages
// are installed). For every package in web/package-lock.json, every
// platform build included, it asks the registry for the highest lower
// version that was published with provenance, and fails when the locked
// version has none, or has it from another source repository
// (tools/provenance_policy.mjs). An accepted case goes into
// tools/provenance-exceptions.json with the exact problem it accepts and
// why; it then applies only to that problem, and an entry that no locked
// package has any more fails too. No answer from the registry fails.
//   node tools/check_provenance.mjs [--lock <file>] [--exceptions <file>] [--registry <url>]
import { existsSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { lockedPackages, previousVersion, provenanceProblems, sourceOf } from "./provenance_policy.mjs";

const root = fileURLToPath(new URL("..", import.meta.url));
const arg = (name) => {
  const i = process.argv.indexOf(name);
  return i > 0 ? process.argv[i + 1] : undefined;
};
const lockFile = arg("--lock") ?? join(root, "web", "package-lock.json");
const exceptionsFile = arg("--exceptions") ?? join(root, "tools", "provenance-exceptions.json");
const registry = (arg("--registry") ?? "https://registry.npmjs.org").replace(/\/+$/, "");

const lock = JSON.parse(readFileSync(lockFile, "utf8"));
const accepted = existsSync(exceptionsFile) ? (JSON.parse(readFileSync(exceptionsFile, "utf8")).accepted ?? {}) : {};

class NoAnswer extends Error {}
async function getJson(url, accept) {
  // only from the registry this check asks: an attestation URL elsewhere is refused
  if (!url.startsWith(`${registry}/`)) throw new Error(`${url} is not on ${registry}`);
  for (let attempt = 1; ; attempt++) {
    try {
      const res = await fetch(url, { headers: accept ? { Accept: accept } : {}, redirect: "error", signal: AbortSignal.timeout(60000) });
      if (!res.ok) throw new Error(`${url}: HTTP ${res.status}`);
      return await res.json();
    } catch (e) {
      if (attempt >= 2) throw new NoAnswer(e.cause?.message ? `${e.message}: ${e.cause.message}` : e.message);
    }
  }
}
const packument = (name) => getJson(`${registry}/${name.replace("/", "%2f")}`, "application/vnd.npm.install-v1+json");

// a few requests at a time
async function pool(items, size, fn) {
  const out = new Array(items.length);
  let next = 0;
  await Promise.all(
    Array.from({ length: Math.min(size, items.length) }, async () => {
      while (next < items.length) {
        const i = next++;
        out[i] = await fn(items[i]);
      }
    }),
  );
  return out;
}

// each name@version once, wherever the lockfile nests it
const packages = [...new Map(lockedPackages(lock).map((p) => [`${p.name}@${p.version}`, p])).values()];
const problems = [];
const used = new Set();
const unknown = new Set(); // name@version the registry does not have
for (const [key, entry] of Object.entries(accepted))
  if (typeof entry?.problem !== "string" || typeof entry?.reason !== "string" || !entry.reason.trim())
    problems.push(`${key} in ${exceptionsFile} needs the exact "problem" it accepts and a "reason"`);
let withProvenance = 0;
let complete = true;
try {
  const names = [...new Set(packages.map((p) => p.name))];
  const docs = new Map((await pool(names, 8, packument)).map((doc, i) => [names[i], doc]));
  await pool(packages, 8, async ({ name, version }) => {
    const versions = docs.get(name)?.versions ?? {};
    if (!versions[version]) {
      unknown.add(`${name}@${version}`);
      problems.push(`${name}@${version} is not on ${registry}`);
      return;
    }
    const attestations = (v) => versions[v]?.dist?.attestations;
    const lockedAtt = attestations(version);
    if (lockedAtt) withProvenance++;
    const previous = previousVersion(Object.keys(versions), version, (v) => !!attestations(v));
    let locked = {};
    let earlier = {};
    if (lockedAtt && previous) [locked, earlier] = (await Promise.all([getJson(lockedAtt.url), getJson(attestations(previous).url)])).map(sourceOf);
    for (const p of provenanceProblems({ name, version, previous, lockedHas: !!lockedAtt, locked, earlier })) {
      const key = `${name}@${version}`;
      if (accepted[key]?.problem === p) used.add(key);
      else problems.push(accepted[key] ? `${p} (${exceptionsFile} accepts another one: ${accepted[key].problem})` : p);
    }
  });
} catch (e) {
  complete = false;
  problems.push(e instanceof NoAnswer ? `no usable answer from the registry: ${e.message}` : `check failed: ${e.message}`);
}
// only a complete check can tell that an accepted case is gone
if (complete)
  for (const key of Object.keys(accepted)) if (!used.has(key) && !unknown.has(key)) problems.push(`${key} is accepted in ${exceptionsFile}, but no locked package has that problem any more: remove it`);
if (packages.length === 0) problems.push("no packages found in the lockfile: nothing was checked");

if (problems.length) {
  console.error("Provenance check failed:\n- " + problems.join("\n- "));
  console.error("A case accepted after review goes into tools/provenance-exceptions.json with its exact problem and the reason.");
  process.exit(1);
}
for (const key of used) console.log(`Accepted: ${key} (${accepted[key].reason})`);
console.log(`Provenance check: ${packages.length} npm packages, ${withProvenance} with provenance, none lost it or changed its source.`);
