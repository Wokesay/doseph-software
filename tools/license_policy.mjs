// SPDX-License-Identifier: AGPL-3.0-or-later
// Which licenses the npm packages of the web app may have, and which of them
// the app may import, checked by tools/check_licenses.mjs and tested in
// tools/check_licenses.test.mjs.
// Runtime packages end up in the device's web app and in the simulator, so
// they must be AGPL-compatible and listed in THIRD_PARTY_NOTICES.md.
// Build tools (dev) are not installed anywhere we ship; for them a few more
// licenses are fine.
import { allowed } from "./spdx_expr.mjs";

export const RUNTIME = new Set(["MIT", "ISC", "BSD-2-Clause", "BSD-3-Clause", "Apache-2.0", "0BSD", "Zlib", "CC0-1.0", "Unlicense"]);
// CC-BY-4.0: browser usage data (caniuse-lite), read by the build only.
export const DEV = new Set([...RUNTIME, "CC-BY-4.0"]);
// MPL-2.0 only for lightningcss and its platform builds: Vite runs it on our
// CSS and none of its files end up in the app. Other build tools such as
// Vite or Rolldown put code of their own into the app, so a dev package is
// not by itself never shipped; another MPL-2.0 tool needs a decision of its
// own.
const MPL_TOOLS = /^lightningcss(?:-(?:android|darwin|freebsd|linux|win32)-[a-z0-9-]+)?$/;
const DEV_MPL = new Set([...DEV, "MPL-2.0"]);

/** Problems of all packages in a package-lock.json (version 2 or later); none means OK. */
export function licenseProblems(lock, notices) {
  if (!(lock?.lockfileVersion >= 2) || !lock.packages) {
    return { count: 0, problems: ["web/package-lock.json must be lockfile version 2 or later"] };
  }
  const problems = [];
  let count = 0;
  for (const [path, pkg] of Object.entries(lock.packages)) {
    if (!path) continue;
    count++;
    const name = path.replace(/^.*node_modules\//, "");
    const lic = typeof pkg.license === "string" ? pkg.license : "";
    const ok = !pkg.dev ? RUNTIME : MPL_TOOLS.test(name) ? DEV_MPL : DEV;
    if (!lic) problems.push(`${name}: no license in package-lock.json`);
    else if (!allowed(lic, ok)) problems.push(`${name}: license ${lic} not allowed${pkg.dev ? " (build tool)" : ""}`);
    if (!pkg.dev && !notices.includes(`\`${name}\``)) problems.push(`${name}: runtime package missing in THIRD_PARTY_NOTICES.md`);
  }
  if (count === 0) problems.push("no packages found in web/package-lock.json");
  return { count, problems };
}

// Whether a package counts as runtime or build tool above comes from where
// web/package.json lists it. A package the app imports is bundled into what
// we ship, so it must be listed under "dependencies"; otherwise it would be
// checked as a build tool, with the wider list of licenses.

/** The package a bare import names ("preact/hooks" → "preact"); null for a relative or absolute path. */
export function packageOf(spec) {
  // a path into node_modules still names a package that ends up in the app
  const nm = spec.lastIndexOf("node_modules/");
  if (nm >= 0) spec = spec.slice(nm + "node_modules/".length);
  else if (/^[./]/.test(spec)) return null;
  const parts = spec.split("/");
  return spec.startsWith("@") ? parts.slice(0, 2).join("/") : parts[0];
}

/**
 * The code without its comments; strings and template literals stay as
 * they are. Regular expression literals are not recognised: a `//` in one
 * can cut the rest of its line, which never holds an import here.
 */
export function stripComments(src) {
  let out = "";
  let i = 0;
  const open = []; // per open `${ … }` in a template literal: braces opened inside it
  const template = () => {
    while (i < src.length) {
      const c = src[i];
      if (c === "\\") { out += src.slice(i, i + 2); i += 2; continue; }
      if (c === "`") { out += c; i++; return; }
      if (c === "$" && src[i + 1] === "{") { out += "${"; i += 2; open.push(0); return; }
      out += c; i++;
    }
  };
  while (i < src.length) {
    const c = src[i];
    const d = src[i + 1];
    if (c === "/" && d === "/") {
      while (i < src.length && src[i] !== "\n") i++;
    } else if (c === "/" && d === "*") {
      const end = src.indexOf("*/", i + 2);
      i = end < 0 ? src.length : end + 2;
      out += " ";
    } else if (c === '"' || c === "'") {
      const from = i++;
      while (i < src.length && src[i] !== c && src[i] !== "\n") i += src[i] === "\\" ? 2 : 1;
      i++;
      out += src.slice(from, i);
    } else if (c === "`") {
      out += c; i++;
      template();
    } else if (open.length && c === "{") {
      open[open.length - 1]++;
      out += c; i++;
    } else if (open.length && c === "}") {
      out += c; i++;
      if (open[open.length - 1]-- === 0) { open.pop(); template(); }
    } else {
      out += c; i++;
    }
  }
  return out;
}

// `import type X from`, `import type { … } from`, `import type * as N from`
// and the same with export load nothing. `import type from "x"` and
// `import type, { a } from "x"` import a default called "type".
const TYPE_ONLY = /^(?:import|export)\s+type\s+(?:\{|\*|[A-Za-z_$][\w$]*\s+from\b)/;

/**
 * What the code of one file loads at run time: static and dynamic imports,
 * side-effect imports and re-exports. Type-only imports and exports load
 * nothing. Comments are skipped; an unusual form counts rather than slips
 * through, which can only make the check stricter.
 */
export function runtimeImports(source) {
  const code = stripComments(source);
  const specs = [];
  // `from` as a string of its own, as in field("from", t("…")), is no import either
  for (const m of code.matchAll(/(?<![.\w$"'`])from\s*(["'])([^"'\n]+)\1/g)) {
    // the statement this `from` belongs to: the nearest import or export before it
    const start = [...code.slice(0, m.index).matchAll(/\b(?:import|export)\b/g)].at(-1);
    if (start && !TYPE_ONLY.test(code.slice(start.index, m.index + m[0].length))) specs.push(m[2]);
  }
  // not a key or a property named import, as in a text table's "config.import": "…"
  for (const m of code.matchAll(/(?<![.\w$"'`])import\s*(["'])([^"'\n]+)\1/g)) specs.push(m[2]);
  for (const m of code.matchAll(/\bimport\s*\(\s*(["'`])([^"'`\n]+)\1/g)) specs.push(m[2]);
  return specs;
}

/** What a style sheet imports with `@import`. */
export function cssImports(source) {
  const code = source.replace(/\/\*[\s\S]*?\*\//g, " ");
  return [...code.matchAll(/@import\s+(?:url\(\s*)?(["']?)([^"')\s;]+)\1/g)].map((m) => m[2]);
}

/** Packages the app imports that web/package.json does not list under "dependencies". */
export function importProblems(files, pkg) {
  const runtime = new Set(Object.keys(pkg?.dependencies ?? {}));
  const seen = new Set();
  const problems = [];
  for (const { name, source } of files) {
    for (const spec of name.endsWith(".css") ? cssImports(source) : runtimeImports(source)) {
      const p = packageOf(spec);
      if (!p || seen.has(`${p} ${name}`)) continue;
      seen.add(`${p} ${name}`);
      if (!runtime.has(p)) problems.push(`${p}: imported by web/src/${name}, but not under "dependencies" in web/package.json`);
    }
  }
  const count = new Set([...seen].map((k) => k.split(" ")[0])).size;
  return { count, problems };
}
