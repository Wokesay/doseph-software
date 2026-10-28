// SPDX-License-Identifier: AGPL-3.0-or-later
// The core's German message table is the web app's, written for C++.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { generate, readTable, render } from "./gen_msg_de.mjs";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");

test("core/src/messages_de.inc is what tools/gen_msg_de.mjs makes of web/src/lang/msg.ts", () => {
  assert.equal(readFileSync(join(root, "core/src/messages_de.inc"), "utf8"), generate(), "run: node tools/gen_msg_de.mjs > core/src/messages_de.inc");
});

test("the table is read with its escapes and written sorted, with quotes, backslashes and line breaks escaped for C++", () => {
  const t = readTable('// x\nexport const msgDe: Record<string, string> = {\n  // a comment\n  "b.key": "zwei\\u00a0L",\n  "a.key":\n    "sagt \\"Hallo\\" \\\\ und\\nmehr",\n};\n');
  assert.equal(t["b.key"], "zwei L");
  const out = render(t);
  assert.ok(out.indexOf('"a.key"') < out.indexOf('"b.key"'));
  assert.match(out, /\{"a\.key", "sagt \\"Hallo\\" \\\\ und\\nmehr"\},/);
});
