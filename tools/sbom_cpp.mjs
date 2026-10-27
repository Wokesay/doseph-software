// SPDX-License-Identifier: AGPL-3.0-or-later
// SBOM (CycloneDX 1.5) of the C++ libraries in the simulator, read from the
// pinned headers in cmake/deps.cmake so the two cannot drift apart. The
// release attaches it (.github/workflows/release.yml).
//   node tools/sbom_cpp.mjs <version> > sbom-simulator-cpp.cdx.json
import { readFileSync, realpathSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

// Shipped libraries with their licenses as in THIRD_PARTY_NOTICES.md; the
// test framework is not shipped. A new entry in deps.cmake must be added
// here, otherwise the SBOM fails.
const KNOWN = {
  nlohmann_json: { name: "nlohmann-json", licenses: [{ expression: "MIT AND CC0-1.0 AND Apache-2.0" }] },
  cpp_httplib: { name: "cpp-httplib", licenses: [{ license: { id: "MIT" } }] },
  doctest: null,
};

export function parseDeps(text) {
  const re = /dp_fetch_header\(\s*(\w+)\s+"([^"]+)"\s+([0-9a-f]{64})\s/g;
  const found = [...text.matchAll(re)];
  // A call written differently must not drop out of the SBOM silently.
  const calls = text.match(/^\s*dp_fetch_header\(/gm) ?? [];
  if (calls.length !== found.length) {
    throw new Error(`cmake/deps.cmake: ${calls.length} dp_fetch_header calls, ${found.length} understood`);
  }
  return found.map(([, name, url, sha256]) => {
    const m = url.match(/^https:\/\/raw\.githubusercontent\.com\/([^/]+)\/([^/]+)\/v(\d+\.\d+\.\d+)\//);
    if (!m) throw new Error(`${name}: no GitHub release URL with a version: ${url}`);
    return { name, url, sha256, owner: m[1], repo: m[2], version: m[3] };
  });
}

export function sbom(depsText, version) {
  const components = [];
  for (const d of parseDeps(depsText)) {
    if (!(d.name in KNOWN)) throw new Error(`${d.name}: unknown dependency, add it to tools/sbom_cpp.mjs`);
    const known = KNOWN[d.name];
    if (!known) continue;
    components.push({
      type: "library",
      name: known.name,
      version: d.version,
      purl: `pkg:github/${d.owner.toLowerCase()}/${d.repo.toLowerCase()}@v${d.version}`,
      hashes: [{ alg: "SHA-256", content: d.sha256 }],
      licenses: known.licenses,
      externalReferences: [{ type: "distribution", url: d.url }],
    });
  }
  return {
    bomFormat: "CycloneDX",
    specVersion: "1.5",
    version: 1,
    metadata: {
      component: { type: "application", name: "doseph-simulator", version },
    },
    components,
  };
}

// realpath: Node resolves symlinks for import.meta.url but not in argv.
if (process.argv[1] && import.meta.url === pathToFileURL(realpathSync(process.argv[1])).href) {
  const version = process.argv[2];
  if (!version) {
    console.error("usage: node tools/sbom_cpp.mjs <version>");
    process.exit(2);
  }
  const root = join(dirname(fileURLToPath(import.meta.url)), "..");
  const text = readFileSync(join(root, "cmake", "deps.cmake"), "utf8");
  console.log(JSON.stringify(sbom(text, version), null, 2));
}
