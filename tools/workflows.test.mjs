// SPDX-License-Identifier: AGPL-3.0-or-later
// The supply chain of the workflows. Every action is made by GitHub and
// pinned by its full commit SHA with the version as a comment; Dependabot
// keeps both up to date. Only the release's publishing job can write, and
// it builds nothing. Tools come pinned: reuse by hashes. Every job runs the
// Node pinned in .nvmrc, whose npm checks the web app's packages against
// their registry signatures right after they are installed, and a failed
// check reports why. The provenance of the packages is checked before they
// are installed.
//   node --test tools/workflows.test.mjs
import assert from "node:assert/strict";
import { readdirSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { test } from "node:test";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const dir = join(root, ".github", "workflows");
const files = readdirSync(dir).filter((f) => /\.ya?ml$/.test(f));
const read = (f) => readFileSync(join(dir, f), "utf8");

function job(text, id) {
  const m = text.match(new RegExp(`^ {2}${id}:\\n((?: {4}.*\\n|\\s*\\n)*)`, "m"));
  assert.ok(m, `job ${id}`);
  return m[1];
}

test("actions are GitHub's own and pinned by commit SHA with a version comment", () => {
  assert.ok(files.length >= 3, "workflows found");
  let n = 0;
  for (const f of files) {
    for (const line of read(f).split("\n")) {
      const m = line.match(/^\s*(?:-\s+)?uses:\s*(\S+)(.*)$/);
      if (!m) continue;
      n++;
      const [, ref, rest] = m;
      if (ref.startsWith("./")) continue; // reusable workflow of this repository
      assert.match(ref, /^actions\/[\w.-]+(\/[\w.-]+)*@[0-9a-f]{40}$/, `${f}: ${ref}`);
      assert.match(rest, /^\s+# v\d+\.\d+\.\d+\s*$/, `${f}: ${ref} needs a "# vX.Y.Z" comment`);
    }
  }
  assert.ok(n > 10, "uses: lines found");
});

test("the same action uses the same commit everywhere", () => {
  const seen = new Map();
  for (const f of files) {
    for (const m of read(f).matchAll(/uses:\s*(actions\/[\w.-]+)(?:\/[\w.-]+)*@([0-9a-f]{40})/g)) {
      const [, action, sha] = m;
      if (seen.has(action)) assert.equal(sha, seen.get(action), `${f}: ${action}`);
      else seen.set(action, sha);
    }
  }
});

test("every uses: is in a form the pin check reads", () => {
  // A flow mapping like "- { uses: x@main }" would slip past the check above.
  for (const f of files) {
    const text = read(f);
    const tokens = text.match(/\buses\b["']?\s*:/g) ?? [];
    const lines = text.split("\n").filter((l) => /^\s*(?:-\s+)?uses:\s*\S/.test(l));
    assert.equal(tokens.length, lines.length, f);
  }
});

test("only the publishing job of a release can write", () => {
  for (const f of files) {
    const text = read(f);
    assert.match(text, /^permissions:\n {2}contents: read\n/m, `${f}: read-only by default`);
    const writes = text.split("\n").filter((l) => /:\s*write\b/.test(l));
    if (f !== "release.yml") assert.deepEqual(writes, [], f);
  }
  const release = read("release.yml");
  const publish = job(release, "publish");
  const writesOutside = release.replace(publish, "").split("\n").filter((l) => /:\s*write\b/.test(l));
  assert.deepEqual(writesOutside, []);
  assert.match(publish, /if: github\.event_name == 'push' && startsWith\(github\.ref, 'refs\/tags\/v'\)/);
});

test("the publishing job builds nothing and the release build uses no cache", () => {
  const release = read("release.yml");
  const publish = job(release, "publish");
  assert.doesNotMatch(publish, /actions\/(checkout|setup-node|cache)@/);
  assert.doesNotMatch(publish, /\b(npm|npx|cmake|pip|pipx)\b|\bnode\s/);
  assert.doesNotMatch(release, /actions\/cache/);
  // Merged downloads would let one artifact overwrite another's files.
  assert.doesNotMatch(release, /merge-multiple/);
  let setups = 0;
  for (const m of release.matchAll(/uses: actions\/setup-node@\S+.*\n((?: {8}.*\n)*)/g)) {
    setups++;
    assert.match(m[1], /package-manager-cache: false/);
    assert.doesNotMatch(m[1], /^\s*cache:/m);
  }
  assert.ok(setups >= 1, "the build job sets up Node for the SBOMs");
  assert.match(job(release, "packages"), /release: true/);
  const packages = read("packages.yml");
  assert.doesNotMatch(packages, /actions\/cache/);
  assert.match(packages, /cache: \$\{\{ !inputs\.release && 'npm' \|\| '' \}\}/);
  assert.match(packages, /package-manager-cache: \$\{\{ !inputs\.release \}\}/);
});

test("the release attaches both SBOMs, covered by SHA256SUMS and the attestation", () => {
  const build = job(read("release.yml"), "build");
  const web = build.indexOf("> out/dist/sbom-web.cdx.json");
  const cpp = build.indexOf('node tools/sbom_cpp.mjs "$v" > out/dist/sbom-simulator-cpp.cdx.json');
  const sums = build.indexOf("(cd out/dist && sha256sum -- * > SHA256SUMS)");
  assert.ok(web > 0 && cpp > 0 && sums > 0, "the SBOMs and the checksums are written in the build job");
  assert.ok(web < sums && cpp < sums, "the SBOMs are in out/dist before the checksums are taken");
  assert.match(read("release.yml"), /^ {6}- tools\/sbom_cpp\.mjs$/m, "a change to the C++ SBOM runs the release dry run");
});

test("steps get no secrets passed on, and a failing command in a pipe fails the step", () => {
  for (const f of files) assert.doesNotMatch(read(f), /secrets:\s*inherit/, f);
  // GitHub's `shell: bash` runs with -eo pipefail; the default shell does not
  assert.match(read("release.yml"), /^defaults:\n {2}run:\n(?: {4}#.*\n)* {4}shell: bash\n/m, "release.yml");
  assert.match(read("packages.yml"), /^ {4}defaults:\n {6}run:\n {8}shell: bash\n/m, "packages.yml");
});

test("tools and containers come pinned: reuse by hashes, images by digest", () => {
  // Nothing installed by name alone in the workflows.
  for (const f of files) {
    const text = read(f);
    assert.doesNotMatch(text, /\bpipx (install|run)\b|\buv (pip|tool) install\b/, `${f}: installs by name`);
    // also `"$RUNNER_TEMP/reuse/bin/pip" install`: a quote may close the path
    const installs = [...text.matchAll(/\bpip3?(?:\.\d+)?["']?\s+install\b[^\n]*/g)].map((m) => m[0]);
    for (const line of installs) assert.match(line, /--require-hashes .*--no-deps .*--only-binary :all:/, `${f}: ${line}`);
    for (const m of text.matchAll(/^\s+(?:container|image): (\S+)/gm)) assert.match(m[1], /@sha256:[0-9a-f]{64}$/, `${f}: ${m[1]}`);
  }
  // CI installs reuse exactly this way and runs it
  const ci = read("ci.yml");
  assert.match(ci, /pip" install --require-hashes --no-deps --only-binary :all: -r tools\/requirements-reuse-build\.txt\n/, "the build backend");
  assert.match(ci, /pip" install --require-hashes --no-deps --no-build-isolation --only-binary :all: --no-binary reuse -r tools\/requirements-reuse\.txt\n/, "reuse");
  assert.match(ci, /^ {10}"\$RUNNER_TEMP\/reuse\/bin\/reuse" lint$/m, "reuse lint runs");
  for (const file of ["requirements-reuse.txt", "requirements-reuse-build.txt"]) {
    const req = readFileSync(join(root, "tools", file), "utf8");
    const pinned = [...req.matchAll(/^([a-z0-9._-]+)==/gim)].map((m) => m[1]);
    const hashed = [...req.matchAll(/^([a-z0-9._-]+)==\S+ \\\n {4}--hash=sha256:[0-9a-f]{64}/gim)].map((m) => m[1]);
    assert.ok(pinned.length > 0, file);
    assert.deepEqual(hashed, pinned, `${file}: every package has hashes`);
    assert.doesNotMatch(req, /^\s*(-e|--index-url|--extra-index-url|--find-links)\b|^\S+ @ /m, `${file}: no other source`);
  }
  // Dependabot proposes updates of the pinned files
  const dependabot = readFileSync(join(root, ".github", "dependabot.yml"), "utf8");
  assert.match(dependabot, /- package-ecosystem: pip\n {4}directory: \/tools\n/);
});

test("every job that sets up Node runs the one pinned in .nvmrc, and Node's types are its", () => {
  // npm, which checks the registry signatures, comes with Node: one exact
  // version, raised by hand with a review, never by a runner image.
  const pin = readFileSync(join(root, ".nvmrc"), "utf8");
  assert.match(pin, /^\d+\.\d+\.\d+\n$/, ".nvmrc holds one exact version");
  const major = pin.split(".")[0];
  let setups = 0;
  for (const f of files) {
    const text = read(f);
    const n = text.match(/uses:[ \t]*actions\/setup-node@/g)?.length ?? 0;
    setups += n;
    assert.equal(text.match(/^[ \t]+node-version-file: \.nvmrc$/gm)?.length ?? 0, n, `${f}: every setup-node reads .nvmrc`);
    assert.doesNotMatch(text, /node-version:/, `${f}: no version of its own`);
  }
  assert.ok(setups >= 3, "setup-node found");
  const web = JSON.parse(readFileSync(join(root, "web", "package.json"), "utf8"));
  assert.equal(web.devDependencies["@types/node"]?.split(".")[0], major, "@types/node in web/package.json");
  // Dependabot does not raise them to another major on its own.
  const dependabot = readFileSync(join(root, ".github", "dependabot.yml"), "utf8");
  const at = dependabot.indexOf("package-ecosystem: npm");
  const next = dependabot.indexOf("- package-ecosystem:", at);
  const npm = dependabot.slice(at, next < 0 ? undefined : next);
  assert.match(npm, /directory: \/web\n/);
  assert.match(npm, /- dependency-name: "@types\/node"\n[ \t]+update-types: \["version-update:semver-major"\]/);
});

test("a job whose signature check fails says why, right after its last step that checks", () => {
  // A step that runs only on failure and asks npm for the reason. It comes
  // right after the job's last step with the check, so it runs whichever of
  // them failed; the check line stays as it is.
  const report =
    "      - name: Registry signatures, npm's reasons if their check failed\n" +
    "        if: failure()\n" +
    "        timeout-minutes: 5\n" +
    "        run: node tools/signatures_report.mjs web\n";
  let jobs = 0;
  for (const f of files) {
    const text = read(f);
    for (const chunk of text.split(/\n(?= {2}[\w-]+:[ \t]*(?:#.*)?\n)/)) {
      const steps = chunk.split(/(?=\n {6}- )/);
      const checks = steps.map((s, i) => (/npm audit signatures/.test(s) || /^\s+(?:run:\s+)?tools\/ci\.sh$/m.test(s) ? i : -1)).filter((i) => i >= 0);
      const reports = steps.filter((s) => s.includes("signatures_report.mjs")).length;
      if (checks.length === 0) {
        assert.equal(reports, 0, `${f}: a report without a check`);
        continue;
      }
      jobs++;
      const at = checks.at(-1);
      assert.equal(steps[at + 1]?.slice(1) + "\n", report, `${f}: ${chunk.trim().split("\n")[0]} has the report right after its last check`);
      assert.equal(reports, 1, `${f}: one report per job`);
    }
  }
  // CI's checks job and the simulator packages
  assert.ok(jobs >= 2, `jobs found: ${jobs}`);
});

test("the web app's packages are checked against their registry signatures right after every install", () => {
  // Wherever they are installed, before any of them runs: each place
  // installs the builds for its own platform, and only installed ones are
  // checked. --ignore-scripts: no install script runs before the check.
  const places = [...files.map((f) => join(".github", "workflows", f)), join("tools", "ci.sh")];
  let installs = 0;
  for (const place of places) {
    for (const line of readFileSync(join(root, place), "utf8").split("\n")) {
      if (!/\bnpm ci\b/.test(line) || /^\s*#/.test(line)) continue;
      installs++;
      assert.doesNotMatch(line, /\|\||;|#/, `${place}: no way around the check: ${line}`);
      const steps = line.split("&&").map((x) => x.trim().replace(/^run:\s*/, "").replace(/\)$/, ""));
      const at = steps.findIndex((x) => /^npm ci\b/.test(x));
      assert.ok(at >= 0, `${place}: npm ci starts a step: ${line}`);
      assert.match(steps[at], /--ignore-scripts/, `${place}: no install script runs before the check: ${line}`);
      assert.equal(steps[at + 1], "npm audit signatures", `${place}: right after npm ci: ${line}`);
    }
  }
  // CI's browser step, tools/ci.sh, the simulator packages
  assert.ok(installs >= 3, `npm ci found ${installs} times`);
  assert.match(read("ci.yml"), /^ {8}run: tools\/ci\.sh$/m, "CI runs tools/ci.sh");
  const ci = readFileSync(join(root, "tools", "ci.sh"), "utf8");
  assert.match(ci, /\necho "Node \$\(node -v\), npm \$\(npm -v\)[^"\n]*"\n\(cd web && npm ci /, "tools/ci.sh says which Node and npm check");
  // The check proves the registry's own record is signed; the lockfile must
  // point at that record, not at a tarball elsewhere.
  const lock = JSON.parse(readFileSync(join(root, "web", "package-lock.json"), "utf8"));
  const entries = Object.entries(lock.packages).filter(([path]) => path !== "");
  assert.ok(entries.length > 100, "packages in the lockfile");
  for (const [path, pkg] of entries) {
    const name = path.slice(path.lastIndexOf("node_modules/") + "node_modules/".length);
    const file = `${name.split("/").pop()}-${pkg.version}.tgz`;
    assert.equal(pkg.resolved, `https://registry.npmjs.org/${name}/-/${file}`, `${path} comes from the npm registry`);
    assert.match(pkg.integrity ?? "", /^sha512-/, `${path} is pinned by its sha512`);
  }
});

test("CI checks the provenance of the lockfile's packages before it installs them", () => {
  const steps = job(read("ci.yml"), "checks").split(/(?=\n {6}- )/);
  const check = steps.findIndex((s) => /^ {8}run: node tools\/check_provenance\.mjs$/m.test(s));
  const install = steps.findIndex((s) => /\bnpm ci\b/.test(s));
  assert.ok(check >= 0, "ci.yml runs tools/check_provenance.mjs as a step of its own");
  assert.ok(install > check, "before npm ci");
  assert.doesNotMatch(steps[check], /continue-on-error|\bif:/, "nothing lets it pass by");
});
