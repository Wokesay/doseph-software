// SPDX-License-Identifier: AGPL-3.0-or-later
// Embeds the built web app (web/dist), each file gzip-compressed, into a C++
// source for the simulator's download (DP_SIM_WEB_ASSETS). Run after
// `npm run build`:
//   node tools/embed_web.mjs <file.cpp>
import { mkdirSync, readdirSync, readFileSync, statSync, writeFileSync } from "node:fs";
import { dirname, join, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { gzipSync } from "node:zlib";

const root = fileURLToPath(new URL("..", import.meta.url));
const dist = join(root, "web", "dist");
if (!process.argv[2]) {
  console.error("usage: node tools/embed_web.mjs <file.cpp>");
  process.exit(2);
}
const out = resolve(process.argv[2]);
const types = { html: "text/html; charset=utf-8", js: "application/javascript", css: "text/css", svg: "image/svg+xml", json: "application/json", png: "image/png", ico: "image/x-icon", woff2: "font/woff2" };

const files = [];
const walk = (d) => {
  for (const f of readdirSync(d).sort()) {
    const p = join(d, f);
    if (statSync(p).isDirectory()) walk(p);
    else files.push(p);
  }
};
walk(dist);
if (!files.some((p) => relative(dist, p) === "index.html")) {
  console.error("web/dist has no index.html – run npm run build in web/ first");
  process.exit(1);
}

let src = "// Written by tools/embed_web.mjs. Do not edit by hand.\n#include \"web_assets.hpp\"\n\nnamespace sim {\n\n";
const entries = [];
let total = 0;
files.forEach((p, i) => {
  const gz = gzipSync(readFileSync(p), { level: 9 });
  total += gz.length;
  const ext = p.split(".").pop();
  src += `static const unsigned char kF${i}[] = {`;
  for (let k = 0; k < gz.length; k++) src += (k % 24 === 0 ? "\n  " : "") + gz[k] + ",";
  src += "\n};\n";
  entries.push(`  {"/${relative(dist, p).split("\\").join("/")}", "${types[ext] ?? "application/octet-stream"}", kF${i}, sizeof(kF${i})}`);
});
src += `\nconst WebAsset kWebAssets[] = {\n${entries.join(",\n")}\n};\nconst std::size_t kWebAssetCount = ${entries.length};\n\n}  // namespace sim\n`;
mkdirSync(dirname(out), { recursive: true });
writeFileSync(out, src);
console.log(`${entries.length} files, ${(total / 1024).toFixed(1)} KB gzip → ${relative(root, out)}`);
