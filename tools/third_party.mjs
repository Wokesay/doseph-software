// SPDX-License-Identifier: AGPL-3.0-or-later
// The license notices of the third-party libraries shipped in the
// simulator's download (MIT and ISC require them in copies too).
//   node tools/third_party.mjs <target.txt>
// Web libraries from web/node_modules (after `npm ci`), C++ libraries from
// the headers cmake/deps.cmake fetched (version and copyright are read
// there, so they cannot drift apart).
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = fileURLToPath(new URL("..", import.meta.url));
const out = resolve(process.argv[2] ?? "THIRD_PARTY_LICENSES.txt");
const mods = join(root, "web", "node_modules");
const fail = (m) => {
  console.error(m);
  process.exit(1);
};

// The web app's runtime dependencies and theirs (no development tools)
const seen = new Set();
const walk = (name) => {
  if (seen.has(name)) return;
  const dir = join(mods, name);
  if (!existsSync(join(dir, "package.json"))) fail(`${name} is missing – run npm ci in web/ first`);
  seen.add(name);
  const pkg = JSON.parse(readFileSync(join(dir, "package.json"), "utf8"));
  for (const d of Object.keys(pkg.dependencies ?? {})) walk(d);
};
const web = JSON.parse(readFileSync(join(root, "web", "package.json"), "utf8"));
for (const d of Object.keys(web.dependencies ?? {})) walk(d);

const licenseText = (dir) => {
  for (const f of ["LICENSE", "LICENSE.md", "LICENSE.txt", "license", "LICENCE"]) {
    if (existsSync(join(dir, f))) return readFileSync(join(dir, f), "utf8").trim();
  }
  return fail(`No license file in ${dir}`);
};

const parts = [];
for (const name of [...seen].sort()) {
  const dir = join(mods, name);
  const pkg = JSON.parse(readFileSync(join(dir, "package.json"), "utf8"));
  parts.push(`${name} ${pkg.version} (${pkg.license})\n\n${licenseText(dir)}`);
}

// The MIT wording from preact's license (the same for every MIT package)
const preact = licenseText(join(mods, "preact"));
const at = preact.indexOf("Permission is hereby granted");
if (at < 0) fail("MIT wording not found in preact's license");
const mit = preact.slice(at);

const inc = ["build-pkg", "build"].map((b) => join(root, b, "_deps", "include")).find((d) => existsSync(join(d, "httplib.h")));
if (!inc) fail("C++ headers not found (build-pkg/_deps/include) – run cmake first");
const json = readFileSync(join(inc, "nlohmann", "json.hpp"), "utf8");
const jsonVersion = json.match(/version (\d+\.\d+\.\d+)/)?.[1] ?? fail("version of nlohmann/json not found");
const jsonCopy = [...new Set([...json.matchAll(/SPDX-FileCopyrightText: (.+)/g)].map((m) => `Copyright (c) ${m[1].trim()}`))];
parts.push(
  `nlohmann/json ${jsonVersion} (MIT)\n\n${jsonCopy.join("\n")}\n\n${mit}\n\n` +
    "Contains Hedley by Evan Nemerson (CC0-1.0) and parts of Google Abseil (Apache-2.0, text below).",
);
const httplib = readFileSync(join(inc, "httplib.h"), "utf8");
const httpVersion = httplib.match(/CPPHTTPLIB_VERSION "([^"]+)"/)?.[1] ?? fail("version of cpp-httplib not found");
const httpCopy = httplib.match(/Copyright \(c\) [^\n]+/)?.[0] ?? fail("copyright of cpp-httplib not found");
parts.push(`cpp-httplib ${httpVersion} (MIT)\n\n${httpCopy}\n\n${mit}`);
parts.push(`Apache License 2.0 (for the Abseil parts in nlohmann/json)\n\n${readFileSync(join(root, "tools", "licenses", "Apache-2.0.txt"), "utf8").trim()}`);

const head = "Doseph – third-party licenses of the included libraries\n";
const sep = "\n\n" + "=".repeat(72) + "\n\n";
mkdirSync(dirname(out), { recursive: true });
writeFileSync(out, head + sep + parts.join(sep) + "\n");
console.log(`${parts.length} entries → ${out}`);
