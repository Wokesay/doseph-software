// SPDX-License-Identifier: AGPL-3.0-or-later
// Size budget of the web app: it lies gzip-compressed in the robot's flash
// (at most about 250 KB gzip). Breaks the build when it is exceeded.
import { readdirSync, readFileSync, statSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { gzipSync } from "node:zlib";

const BUDGET = 250 * 1024;
const dir = fileURLToPath(new URL("../dist", import.meta.url));
let total = 0;
const rows = [];
const walk = (d) => {
  for (const f of readdirSync(d)) {
    const p = join(d, f);
    if (statSync(p).isDirectory()) walk(p);
    else {
      const gz = gzipSync(readFileSync(p), { level: 9 }).length;
      total += gz;
      rows.push([p.slice(dir.length + 1), gz]);
    }
  }
};
walk(dir);
for (const [f, s] of rows) console.log(`${(s / 1024).toFixed(1).padStart(7)} KB  ${f}`);
console.log(`${(total / 1024).toFixed(1).padStart(7)} KB  total (gzip), budget ${BUDGET / 1024} KB`);
if (total > BUDGET) {
  console.error("Size budget exceeded");
  process.exit(1);
}
