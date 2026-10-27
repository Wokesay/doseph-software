// SPDX-License-Identifier: AGPL-3.0-or-later
// Says why the web app's registry signature check failed. The check stays
// where it is, right after `npm ci` and before any package's code runs,
// and its plain output names a package but not the reason. After a failed
// job this
//   - shows the end of npm's own log of the check, the only record of the
//     run that failed (or that the check passed and the job failed
//     elsewhere),
//   - asks npm again with --json and prints each invalid or missing package
//     with npm's code and message, or that npm could not check at all,
// with the Node and npm that checked. It changes nothing and never fails the
// job itself: the check already did. Text from npm goes to the log between
// ::stop-commands:: markers, which is what keeps it from acting as a
// workflow command (the runner finds "##[" anywhere in a line); on top of
// that each piece is one line with the command markers broken up.
//   node tools/signatures_report.mjs [web folder]
import { spawnSync } from "node:child_process";
import { randomUUID } from "node:crypto";
import { existsSync, readdirSync, readFileSync, realpathSync, statSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const MAX_LINE = 4000;
const MAX_ENTRIES = 200;
const LOG_LINES = 60;
const TIMEOUT_MS = 60_000;

// One line of text from npm: no line breaks, control or direction
// characters, workflow command markers broken up, cut short.
export function oneLine(v) {
  return String(v ?? "")
    .replace(/[\u0000-\u001f\u007f-\u009f\u202a-\u202e\u2066-\u2069]+/g, " ")
    .replace(/::/g, ": :")
    .replace(/##\[/g, "## [")
    .trim()
    .slice(0, MAX_LINE);
}

// The lines for npm's JSON answer (an object with "invalid" and "missing"),
// for its error answer, or for whatever else came back.
export function describe(stdout) {
  let report;
  try {
    report = JSON.parse(stdout);
  } catch {
    const text = oneLine(stdout);
    return [`npm gave no JSON${text ? ":" : "."}`, ...(text ? [text] : [])];
  }
  // npm that could not check at all (no registry, no Sigstore) answers with
  // an error, not with a report; that is no pass.
  if (report?.error) {
    const e = report.error;
    const lines = [`npm could not check the signatures: ${oneLine([e.code, e.summary].filter(Boolean).join(": ") || JSON.stringify(e))}`];
    if (e.detail) lines.push(`  ${oneLine(e.detail)}`);
    return lines;
  }
  if (!Array.isArray(report?.invalid) || !Array.isArray(report?.missing)) return [`npm gave no report: ${oneLine(stdout)}`];
  const lines = [];
  let shown = 0;
  const all = report.invalid.length + report.missing.length;
  for (const kind of ["invalid", "missing"]) {
    for (const p of report[kind]) {
      if (shown++ >= MAX_ENTRIES) break;
      if (!p || typeof p !== "object") {
        lines.push(`${kind}: ${oneLine(JSON.stringify(p))}`);
        continue;
      }
      const what = [p.predicateType && `attestation ${p.predicateType}`, p.keyid && `key ${p.keyid}`].filter(Boolean).join(", ");
      lines.push(oneLine(`${kind}: ${p.name ?? "?"}@${p.version ?? "?"}${what ? ` (${what})` : ""}`));
      if (p.code || p.message) lines.push(`  ${oneLine([p.code, p.message].filter(Boolean).join(": "))}`);
      if (p.location) lines.push(`  at ${oneLine(p.location)}`);
      if (p.resolved) lines.push(`  from ${oneLine(p.resolved)}`);
      if (p.integrity) lines.push(`  integrity ${oneLine(p.integrity)}`);
    }
  }
  if (all > MAX_ENTRIES) lines.push(`… and ${all - MAX_ENTRIES} more`);
  if (lines.length === 0)
    lines.push("npm audit signatures passes now. If this job failed at the signature check, that did not repeat; a re-run is no clearance.");
  return lines;
}

// The end of npm's own log of the last `npm audit signatures` written after
// `after` and before `before` (ms), or a note when there is none. `after`
// is when this job's npm ci finished: older logs come from a cache.
export function failedRunLog(logsDir, before, after = 0) {
  let logs = [];
  try {
    logs = readdirSync(logsDir)
      .filter((f) => f.endsWith(".log"))
      .map((f) => ({ f, t: statSync(join(logsDir, f)).mtimeMs }))
      .filter((x) => x.t < before && x.t >= after)
      .sort((a, b) => b.t - a.t);
  } catch {
    return [`No npm logs in ${oneLine(logsDir)}.`];
  }
  for (const { f } of logs) {
    let text;
    try {
      text = readFileSync(join(logsDir, f), "utf8");
    } catch {
      continue;
    }
    if (!/verbose title npm audit signatures\b/.test(text)) continue;
    const exit = text.match(/verbose exit (\d+)/)?.[1];
    if (exit === "0") return [`The signature check passed in this job (npm log ${oneLine(f)}); the job failed elsewhere.`];
    const tail = text.split(/\r?\n/).filter((l) => l.trim()).slice(-LOG_LINES).map(oneLine);
    return [`End of npm's log of the check (${oneLine(f)}, exit ${exit ?? "unknown"}):`, ...tail.map((l) => `  ${l}`)];
  }
  return ["No npm log of a signature check found; the job may have failed before it, or elsewhere."];
}

// The npm that comes with the running Node, started without PATH and
// without a shell (on Windows npm is a .cmd, which needs one).
export function npmCli(execPath = process.execPath, platform = process.platform) {
  const dir = dirname(execPath);
  return platform === "win32" ? join(dir, "node_modules", "npm", "bin", "npm-cli.js") : join(dir, "..", "lib", "node_modules", "npm", "bin", "npm-cli.js");
}

function runNpm(args, cwd) {
  const cli = npmCli();
  const opts = { cwd, encoding: "utf8", maxBuffer: 16 * 1024 * 1024, timeout: TIMEOUT_MS };
  // The arguments are constants of this file; on Windows without the file
  // next to Node, the shell starts npm.cmd.
  const r = existsSync(cli)
    ? spawnSync(process.execPath, [cli, ...args], opts)
    : process.platform === "win32"
      ? spawnSync(`npm ${args.join(" ")}`, { ...opts, shell: true })
      : spawnSync("npm", args, opts);
  return { out: r.stdout ?? "", err: r.stderr ?? "", status: r.status, error: r.error };
}

// The report, line by line to `print`: npm's log of the failed run first,
// so it is out before anything can take long. `npm` runs npm with
// arguments in a folder.
export function report(web, { npm = runNpm, logsDir, now = Date.now(), print = console.log } = {}) {
  if (!existsSync(join(web, "node_modules"))) return print(`No packages installed in ${oneLine(web)}: the job failed before npm ci, nothing to report.`);
  let installed = 0;
  try {
    installed = statSync(join(web, "node_modules", ".package-lock.json")).mtimeMs;
  } catch {}
  const dir = logsDir ?? (process.env.npm_config_cache ? join(process.env.npm_config_cache, "_logs") : null);
  if (dir) for (const l of failedRunLog(dir, now, installed)) print(l);
  print(`Node ${process.version}, npm ${oneLine(npm(["-v"], web).out) || "?"}`);
  if (!dir) for (const l of failedRunLog(join(oneLine(npm(["config", "get", "cache"], web).out), "_logs"), now, installed)) print(l);
  const r = npm(["audit", "signatures", "--json"], web);
  print("Asked again now:");
  if (r.error) print(`  npm could not be started or did not finish: ${oneLine(r.error.code ?? r.error.message)}`);
  for (const l of describe(r.out)) print(`  ${l}`);
  for (const l of r.err.split(/\r?\n/).filter((x) => x.trim()).slice(-20)) print(`  ${oneLine(l)}`);
}

function isMain() {
  try {
    return process.argv[1] && realpathSync(process.argv[1]) === realpathSync(fileURLToPath(import.meta.url));
  } catch {
    return false;
  }
}

if (isMain()) {
  const token = randomUUID();
  console.log(`::stop-commands::${token}`);
  try {
    report(resolve(process.argv[2] ?? "web"));
  } catch (e) {
    console.log(`The report itself failed: ${oneLine(e?.stack ?? e)}`);
  } finally {
    console.log(`::${token}::`);
  }
}
