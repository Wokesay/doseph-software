// SPDX-License-Identifier: AGPL-3.0-or-later
// Text keys of the web app (gc#18): web/src/lang/de.ts and en.ts merge the
// base texts and one file per area. A key used in two files would silently
// overwrite the other; every file has the same keys in German and English.
// The robot's messages (SD-032): core/src/messages.cpp (English) and
// web/src/lang/msg.ts (German) have the same keys and placeholders, and
// every say("…") in the core and the simulator names a key of the table.
//   node --test tools/*.test.mjs
import assert from "node:assert/strict";
import { readdirSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { test } from "node:test";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const dir = join(root, "web", "src", "lang");
const coreSrc = join(root, "core", "src");

// Every key stands at the start of its own line as "key": – a key in any
// other form (single quotes, no quotes, a second key on the line) would be
// skipped and a collision missed, so it fails instead.
export function keys(text, file = "text") {
  const out = [];
  for (const line of text.split("\n")) {
    const m = line.match(/^\s*"([^"]+)"\s*:/);
    if (!m) {
      assert.doesNotMatch(line, /^\s*(?:'[^']*'|[A-Za-z_$][\w$.-]*)\s*:/, `${file}: key not written as "key": ${line.trim()}`);
      assert.doesNotMatch(line, /[{,]\s*"[^"]+"\s*:/, `${file}: one key per line: ${line.trim()}`);
      continue;
    }
    assert.doesNotMatch(line.slice(m[0].length), /^\s*"(?:[^"\\]|\\.)*"\s*,\s*"[^"]+"\s*:/, `${file}: one key per line: ${line.trim()}`);
    out.push(m[1]);
  }
  return out;
}

// Placeholders per key, e.g. "Pair {p}" → "p" and "{ph:2}" → "ph:2"; a
// value may span lines.
export function placeholders(text) {
  const out = new Map();
  let key = null;
  for (const line of text.split("\n")) {
    const m = line.match(/^\s*"([^"]+)"\s*:/);
    if (m) {
      key = m[1];
      out.set(key, new Set());
    } else if (/^\s*(\/\/|[}\]]|\.\.\.|export\b)/.test(line)) {
      key = null;
      continue;
    }
    if (key === null) continue;
    for (const v of (m ? line.slice(m[0].length) : line).matchAll(/\{(\w+(?::\d)?)\}/g)) out.get(key).add(v[1]);
  }
  return new Map([...out].map(([k, v]) => [k, [...v].sort().join(",")]));
}

// German and English part of each file
function parts(file) {
  const text = readFileSync(join(dir, file), "utf8");
  if (file === "de.ts") return { de: keys(text, file), en: null };
  if (file === "en.ts") return { de: null, en: keys(text, file) };
  const m = text.match(/^export const en\b/m);
  assert.ok(m, `${file}: export const en`);
  return { de: keys(text.slice(0, m.index), file), en: keys(text.slice(m.index), file) };
}

// msg.ts holds only German (the robot sends English); it has its own tests.
const files = readdirSync(dir).filter((f) => f.endsWith(".ts") && f !== "msg.ts");

// The core's table: one {"key", "template"} per line; any other line in
// the table must be blank or a comment, so no entry is skipped unseen.
export function coreMessages(text) {
  const out = new Map();
  const start = text.indexOf("kMessages[] = {");
  assert.ok(start > 0, "messages.cpp: kMessages");
  const body = text.slice(text.indexOf("\n", start) + 1, text.indexOf("\n};", start));
  for (const line of body.split("\n")) {
    const m = line.match(/^\s*\{"([^"]+)",\s*"((?:[^"\\]|\\.)*)"\},?\s*$/);
    if (!m) {
      assert.match(line, /^\s*(\/\/.*)?$/, `messages.cpp: line not read: ${line.trim()}`);
      continue;
    }
    assert.ok(!out.has(m[1]), `messages.cpp: ${m[1]} twice`);
    out.set(m[1], m[2]);
  }
  return out;
}

const sorted = (text) => [...new Set([...text.matchAll(/\{(\w+(?::\d)?)\}/g)].map((v) => v[1]))].sort().join(",");
const core = () => coreMessages(readFileSync(join(coreSrc, "messages.cpp"), "utf8"));

test("the robot's messages have a German text with the same placeholders", () => {
  const table = core();
  assert.ok(table.size > 50, "messages.cpp read");
  const text = readFileSync(join(dir, "msg.ts"), "utf8");
  const german = keys(text, "msg.ts");
  assert.equal(new Set(german).size, german.length, "msg.ts: duplicate key");
  assert.deepEqual([...german].sort(), [...table.keys()].sort(), "same keys in messages.cpp and msg.ts");
  const de = placeholders(text);
  for (const [k, tmpl] of table) assert.equal(de.get(k), sorted(tmpl), `${k}: placeholders differ`);
});

test("every say() in the core and the simulator names a key of the table", () => {
  const table = core();
  const cpp = (d) => readdirSync(join(root, d)).filter((f) => /\.(cpp|hpp)$/.test(f) && f !== "messages.cpp" && f !== "messages.hpp").map((f) => join(root, d, f));
  let n = 0;
  for (const path of [...cpp("core/src"), ...cpp("core/include/doseph"), ...cpp("sim")]) {
    const f = path.slice(root.length + 1);
    const text = readFileSync(path, "utf8").replace(/^\s*\/\/.*$/gm, "");
    for (const m of text.matchAll(/\bsay\(([^,)]*)/g)) {
      // The keys in the call: a literal, one per branch of a ternary, or a prefix with a part added ("latch." + id)
      const literals = [...m[1].matchAll(/"([a-z_]+\.[a-z0-9_.]*)"/g)].map((x) => x[1]);
      if (!literals.length) continue;
      for (const k of literals) {
        n++;
        if (k.endsWith(".")) assert.ok([...table.keys()].some((x) => x.startsWith(k)), `${f}: no key starts with ${k}`);
        else assert.ok(table.has(k), `${f}: say("${k}") is not in messages.cpp`);
      }
    }
  }
  assert.ok(n > 150, `say() calls found: ${n}`);
});

test("no file of the core or the simulator writes a message's text next to its key", () => {
  // The web app fills a known key with the table's template; a different text
  // under the same key would show that template with missing values.
  const table = core();
  for (const d of ["core/src", "sim"])
    for (const f of readdirSync(join(root, d)).filter((x) => x.endsWith(".cpp") && x !== "messages.cpp")) {
      const text = readFileSync(join(root, d, f), "utf8").replace(/\bsay\(\s*(?:"[^"]*"|[^,)]*)/g, "say(");
      for (const k of table.keys()) assert.ok(!text.includes(`{"${k}", "`), `${d}/${f}: "${k}" with a text of its own`);
    }
});

test("German and English use the same placeholders", () => {
  const read = (file) => {
    const text = readFileSync(join(dir, file), "utf8");
    if (file === "de.ts" || file === "en.ts") return placeholders(text);
    const m = text.match(/^export const en\b/m);
    return { de: placeholders(text.slice(0, m.index)), en: placeholders(text.slice(m.index)) };
  };
  const pairs = files.filter((f) => f !== "de.ts" && f !== "en.ts").map((f) => [f, read(f)]);
  pairs.push(["de.ts/en.ts", { de: read("de.ts"), en: read("en.ts") }]);
  for (const [f, { de, en }] of pairs) for (const [k, v] of de) assert.equal(en.get(k), v, `${f}: ${k}: placeholders differ`);
  assert.deepEqual(placeholders('  "a": "Pair {p} of {n}",\n  "b":\n    "x {y}",\n  // {z}\n'), new Map([["a", "n,p"], ["b", "y"]]));
});

test("the key reader refuses keys it cannot read", () => {
  assert.deepEqual(keys('  "a.b": "x",\n  "c":\n    "long value: with colon",\n  // note: y\n'), ["a.b", "c"]);
  assert.throws(() => keys("  'a.b': \"x\",\n"));
  assert.throws(() => keys("  ab: \"x\",\n"));
  assert.throws(() => keys('  "a": "x", "b": "y",\n'));
});

test("no text key is defined in two files, and every file has the same keys in German and English", () => {
  const seen = new Map();
  for (const f of files) {
    const p = parts(f);
    for (const k of p.de ?? p.en) {
      const where = f === "en.ts" ? "de.ts" : f;
      if (seen.has(k) && seen.get(k) !== where) assert.fail(`${k}: in ${seen.get(k)} and ${where}`);
      seen.set(k, where);
    }
  }
  for (const f of files.filter((f) => f !== "de.ts" && f !== "en.ts")) {
    const { de, en } = parts(f);
    assert.deepEqual([...en].sort(), [...de].sort(), f);
    assert.equal(new Set(de).size, de.length, `${f}: duplicate key`);
  }
  assert.deepEqual([...parts("en.ts").en].sort(), [...parts("de.ts").de].sort(), "de.ts and en.ts");
});

test("every term with an explanation has its name and its text", () => {
  const ui = readFileSync(join(root, "web", "src", "ui.tsx"), "utf8");
  const m = ui.match(/export type HelpTopic = ([^;]+);/);
  assert.ok(m, "ui.tsx: HelpTopic");
  const topics = [...m[1].matchAll(/"(\w+)"/g)].map((x) => x[1]);
  const base = keys(readFileSync(join(dir, "de.ts"), "utf8"), "de.ts");
  for (const tp of topics) for (const k of [`term.${tp}`, `help.${tp}`]) assert.ok(base.includes(k), `de.ts: ${k}`);
});

// A jump lock's reason is the text of its alarm, later also of a
// notification: at most 200 characters with the longest values (RAT-045)
test("a jump lock's reason fits a notification in both languages (RAT-045)", () => {
  const table = core();
  const german = new Map();
  for (const m of readFileSync(join(dir, "msg.ts"), "utf8").matchAll(/^\s*"([^"]+)": ("(?:[^"\\]|\\.)*"),$/gm)) german.set(m[1], JSON.parse(m[2]));
  const longest = { from: "999.5", to: "999.5", unit: "\u00a0mS/cm" };
  const jumps = [...table.keys()].filter((k) => k.startsWith("truth.jump"));
  assert.ok(jumps.length >= 2, "the jump lock reasons read");
  for (const k of jumps)
    for (const [lang, tmpl] of [["en", JSON.parse(`"${table.get(k)}"`)], ["de", german.get(k)]]) {
      const text = tmpl.replace(/\{(\w+)\}/g, (_, name) => longest[name] ?? "");
      assert.ok(text.length <= 200, `${k} (${lang}): ${text.length} characters`);
    }
});

// Durations keep the number and its unit on one line (gc#106): they are
// joined with a no-break space, in the robot's messages and in the app's texts.
test("a number and its unit of time are joined with a no-break space", () => {
  const plain = /(\}|\d)[ \t]+(s|min|h|Tage|Tagen|days)\b/;
  for (const [k, tmpl] of core()) assert.doesNotMatch(JSON.parse(`"${tmpl}"`), plain, `messages.cpp: ${k}`);
  for (const file of [...files, "msg.ts"])
    for (const line of readFileSync(join(dir, file), "utf8").split("\n")) {
      const m = line.match(/^\s*"[^"]+":\s*("(?:[^"\\]|\\.)*")/);
      if (m) assert.doesNotMatch(JSON.parse(m[1]), plain, `${file}: ${line.trim()}`);
    }
});

// The repository's language is English (README): German lives only in the
// app's German texts and in the German names of the recipe templates.
test("no German outside the German texts", () => {
  const umlaut = /[äöüßÄÖÜ„“]/;  // ok: German
  // msg.ts: the robot's texts in German; README.txt: the download's readme, in both languages
  const skip = new Set(["msg.ts", "README.txt"]);
  const walk = (d) =>
    readdirSync(join(root, d), { withFileTypes: true }).flatMap((e) =>
      e.isDirectory() ? walk(join(d, e.name)) : /\.(cpp|hpp|ts|tsx|mjs|css|cmake|sh|txt)$|CMakeLists\.txt$/.test(e.name) ? [join(d, e.name)] : [],
    );
  const sources = ["core", "sim", "tests", "tools", "cmake", join("web", "src"), join("web", "e2e"), join("web", "scripts")].flatMap(walk);
  assert.ok(sources.length > 40, "sources found");
  for (const f of sources) {
    if (skip.has(f.split("/").pop())) continue;
    let text = readFileSync(join(root, f), "utf8");
    // An area's texts: the German half before "export const en"
    if (f.includes(join("web", "src", "lang"))) text = f.endsWith("de.ts") ? "" : text.slice(text.search(/^export const en\b/m));
    // A line that has to name German text says so
    text = text.replace(/^.*\/\/ ok: German\b.*$/gm, "");
    for (const [i, line] of text.split("\n").entries()) assert.doesNotMatch(line, umlaut, `${f}:${i + 1}: ${line.trim()}`);
  }
});
