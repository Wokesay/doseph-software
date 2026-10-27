// SPDX-License-Identifier: AGPL-3.0-or-later
// License check of all npm dependencies of the web app (tools/ci.sh).
// Reads web/package-lock.json and fails if a package has no license or one
// that is not on the allow list, or if a runtime package is missing in
// THIRD_PARTY_NOTICES.md; reads web/src and fails if the app imports a
// package that is not a runtime dependency (tools/license_policy.mjs).
// It does not see what reaches the app other than by an import in web/src:
// web/index.html, url() in style sheets, the JSX runtime the build adds
// (preact/jsx-runtime) and helper code Vite and Rolldown add themselves.
// The C++ libraries are not npm packages: tools/sbom_cpp.mjs names their
// licenses and fails for a library it does not know.
//   node tools/check_licenses.mjs
import { readFileSync, readdirSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { importProblems, licenseProblems } from "./license_policy.mjs";

const root = fileURLToPath(new URL("..", import.meta.url));
const web = join(root, "web");
const lock = JSON.parse(readFileSync(join(web, "package-lock.json"), "utf8"));
const notices = readFileSync(join(root, "THIRD_PARTY_NOTICES.md"), "utf8");
const pkg = JSON.parse(readFileSync(join(web, "package.json"), "utf8"));
const src = join(web, "src");
const files = readdirSync(src, { recursive: true })
  .filter((f) => /\.(?:[cm]?[jt]sx?|css)$/.test(f))
  .map((f) => ({ name: f.split("\\").join("/"), source: readFileSync(join(src, f), "utf8") }));

const licenses = licenseProblems(lock, notices);
const imports = importProblems(files, pkg);
if (files.length === 0) imports.problems.push("no source files found in web/src");
else if (imports.count === 0) imports.problems.push("no package imports found in web/src: the import scan is broken");
const problems = [...licenses.problems, ...imports.problems];
if (problems.length) {
  console.error("License check failed:\n- " + problems.join("\n- "));
  process.exit(1);
}
console.log(`License check: ${licenses.count} npm packages OK.`);
console.log(`Import check: the web app imports ${imports.count} packages, all runtime dependencies.`);
