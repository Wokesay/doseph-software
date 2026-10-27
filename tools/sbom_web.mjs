// SPDX-License-Identifier: AGPL-3.0-or-later
// The web SBOM: exactly the runtime packages of web/package-lock.json, the
// packages whose code the app may hold. A runtime package is every entry
// not marked `dev`, as for the license check (tools/license_policy.mjs):
// devOptional ones too, which `npm ci --omit=dev` installs.
// `npm sbom --omit dev` is not that: it drops every package a build tool
// depends on, peers included, and so leaves out preact. This takes npm's
// SBOM of the whole tree and keeps, unchanged, the components whose
// `cdx:npm:package:path` is a runtime entry, and the dependency edges
// between them. It fails rather than write a wrong SBOM: a runtime package
// npm did not list (an optional one not installed here is left out, it
// cannot ship), one at another version than the lockfile's, one npm marks
// as development, a component without a path, one listed twice, two edge
// lists for one reference.
// The release builds it from the lockfile, the same one `npm ci` installed
// for the simulator's packages in the same run:
//   (cd web && npm sbom --sbom-format cyclonedx --package-lock-only) \
//     | node tools/sbom_web.mjs web/package-lock.json > sbom-web.cdx.json
import { readFileSync, realpathSync } from "node:fs";
import { fileURLToPath } from "node:url";

const PATH_PROP = "cdx:npm:package:path";
const DEVELOPMENT_PROP = "cdx:npm:package:development";

// Lockfile path → name@version of every runtime package
export function runtimePackages(lock) {
  if (!(lock?.lockfileVersion >= 2) || !lock.packages) throw new Error("web/package-lock.json must be lockfile version 2 or later");
  const out = new Map();
  for (const [path, entry] of Object.entries(lock.packages)) {
    // A link has no flags of its own; its target's entry has them
    if (!path || entry.dev || entry.link) continue;
    const name = entry.name ?? path.replace(/^.*node_modules\//, "");
    out.set(path, `${name}@${entry.version}`);
  }
  return out;
}

export function webSbom(full, lock) {
  const runtime = runtimePackages(lock);
  const components = [];
  const seenPaths = new Set();
  const refs = new Set();
  for (const c of full.components ?? []) {
    const path = c.properties?.find((p) => p.name === PATH_PROP)?.value;
    if (path === undefined) throw new Error(`${c["bom-ref"]} has no ${PATH_PROP}: npm's SBOM changed, check tools/sbom_web.mjs`);
    if (!runtime.has(path)) continue;
    if (c["bom-ref"] !== runtime.get(path)) throw new Error(`npm lists ${c["bom-ref"]} at ${path}, web/package-lock.json has ${runtime.get(path)}: the installed packages are not the lockfile's, run npm ci`);
    if (c.properties.some((p) => p.name === DEVELOPMENT_PROP)) throw new Error(`npm marks ${c["bom-ref"]} (${path}) as development, the lockfile does not: run npm install and check web/package-lock.json`);
    if (refs.has(c["bom-ref"])) throw new Error(`npm lists ${c["bom-ref"]} twice among the runtime packages: its dependency edges would be ambiguous`);
    refs.add(c["bom-ref"]);
    seenPaths.add(path);
    components.push(c);
  }
  const missing = [...runtime].filter(([path]) => !seenPaths.has(path) && !lock.packages[path].optional && !lock.packages[path].devOptional);
  if (missing.length) throw new Error(missing.map(([path, id]) => `${id} (${path}) is a runtime package of web/package-lock.json but not in npm's SBOM`).join("\n"));
  refs.add(full.metadata?.component?.["bom-ref"]);
  const dependencies = (full.dependencies ?? [])
    .filter((d) => refs.has(d.ref))
    .map((d) => ({ ...d, dependsOn: (d.dependsOn ?? []).filter((r) => refs.has(r)) }));
  const listed = new Set();
  for (const d of dependencies) {
    if (listed.has(d.ref)) throw new Error(`two dependency entries for ${d.ref}: a build tool's copy has the same name and version, its edges would mix in`);
    listed.add(d.ref);
  }
  // The file is npm's SBOM, filtered here: say so
  const metadata = Array.isArray(full.metadata?.tools)
    ? { ...full.metadata, tools: [...full.metadata.tools, { vendor: "doseph-software", name: "tools/sbom_web.mjs" }] }
    : full.metadata;
  return { ...full, metadata, components, dependencies };
}

// No try: an error here must fail the step, not leave an empty SBOM
function isMain() {
  return Boolean(process.argv[1]) && realpathSync(process.argv[1]) === realpathSync(fileURLToPath(import.meta.url));
}

if (isMain()) {
  const lockPath = process.argv[2];
  if (!lockPath) {
    console.error("usage: npm sbom --sbom-format cyclonedx --package-lock-only | node tools/sbom_web.mjs <package-lock.json>");
    process.exit(2);
  }
  const input = readFileSync(0, "utf8");
  if (!input.trim()) {
    console.error("no SBOM from npm on stdin");
    process.exit(1);
  }
  const sbom = webSbom(JSON.parse(input), JSON.parse(readFileSync(lockPath, "utf8")));
  process.stdout.write(`${JSON.stringify(sbom, null, 2)}\n`);
}
