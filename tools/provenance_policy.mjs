// SPDX-License-Identifier: AGPL-3.0-or-later
// Provenance must not drop: once a web app package was published with
// provenance, built in a CI from its source repository, it must not be
// locked at a higher version without it, or with provenance from another
// repository. A hijacked maintainer account typically publishes by hand from
// a stolen token, and the registry signs that release too, so
// `npm audit signatures` alone does not see it. Used by
// tools/check_provenance.mjs; tested by tools/check_provenance.test.mjs.

// The packages web/package-lock.json installs from the npm registry.
export function lockedPackages(lock) {
  const out = [];
  for (const [path, p] of Object.entries(lock.packages ?? {})) {
    if (path === "" || p.link) continue;
    out.push({ path, name: path.slice(path.lastIndexOf("node_modules/") + "node_modules/".length), version: p.version });
  }
  return out;
}

// SemVer 2.0.0 precedence; build metadata is ignored.
function parse(v) {
  const m = /^(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$/.exec(v);
  return m ? { core: [+m[1], +m[2], +m[3]], pre: m[4] ? m[4].split(".") : [] } : null;
}
export function compareVersions(a, b) {
  const x = parse(a);
  const y = parse(b);
  if (!x || !y) throw new Error(`not a version: ${!x ? a : b}`);
  for (let i = 0; i < 3; i++) if (x.core[i] !== y.core[i]) return x.core[i] - y.core[i];
  if (!x.pre.length || !y.pre.length) return y.pre.length - x.pre.length; // a release ranks above its pre-releases
  for (let i = 0; i < Math.max(x.pre.length, y.pre.length); i++) {
    if (i >= x.pre.length) return -1;
    if (i >= y.pre.length) return 1;
    const [p, q] = [x.pre[i], y.pre[i]];
    const [np, nq] = [/^\d+$/.test(p), /^\d+$/.test(q)];
    if (np && nq && +p !== +q) return +p - +q;
    if (np !== nq) return np ? -1 : 1;
    if (p !== q) return p < q ? -1 : 1;
  }
  return 0;
}

// The highest version below `locked` for which `has(v)` holds, a pre-release
// only when `locked` is one too; versions that are no version at all are
// left out. With `has` = "was published with provenance" this is the
// version a dropped or moved provenance is measured against: two releases
// published by hand in a row do not hide each other.
export function previousVersion(versions, locked, has = () => true) {
  const pre = parse(locked)?.pre.length > 0;
  let best = null;
  for (const v of versions) {
    const p = parse(v);
    if (!p || (p.pre.length && !pre) || compareVersions(v, locked) >= 0 || !has(v)) continue;
    if (best === null || compareVersions(v, best) > 0) best = v;
  }
  return best;
}

// The source an npm attestation bundle names in its SLSA provenance: the
// repository (v1: the workflow's repository; v0.2: the config source) as
// https://host/owner/repo in lowercase, "" when it names none, and GitHub's
// id of that repository (v1 only), which survives a rename and changes when
// someone else takes over a freed name; "" when it is not given.
export function sourceOf(attestations) {
  for (const a of attestations?.attestations ?? []) {
    if (!/^https:\/\/slsa\.dev\/provenance\//.test(a.predicateType ?? "")) continue;
    let statement;
    try {
      statement = JSON.parse(Buffer.from(a.bundle?.dsseEnvelope?.payload ?? "", "base64").toString("utf8"));
    } catch {
      continue;
    }
    const p = statement?.predicate ?? {};
    const url = p.buildDefinition?.externalParameters?.workflow?.repository ?? p.invocation?.configSource?.uri ?? "";
    const repository = String(url)
      .replace(/^git\+/, "")
      .replace(/^(https?:\/\/[^@]+?)@.*$/, "$1") // v0.2 adds "@refs/..." or "@<commit>"
      .replace(/\/+$/, "")
      .replace(/\.git$/, "")
      .toLowerCase();
    const id = p.buildDefinition?.internalParameters?.github?.repository_id;
    if (repository) return { repository, repositoryId: id === undefined || id === null ? "" : String(id) };
  }
  return { repository: "", repositoryId: "" };
}
export const repositoryOf = (attestations) => sourceOf(attestations).repository;

// What is wrong with one locked package, given whether it has provenance and
// from where, and the highest lower version that has it (`previous`, null if
// none) and from where: [] when nothing.
export function provenanceProblems({ name, version, previous, lockedHas, locked = {}, earlier = {} }) {
  if (!previous) return [];
  const at = `${name}@${version}`;
  if (!lockedHas) return [`${at} has no provenance, but ${previous} had`];
  if (!earlier.repository) return [`${at}: cannot read where ${previous} was built from its provenance`];
  if (!locked.repository)
    return [`${at} names no source repository in its provenance, but ${previous} named ${earlier.repository}`];
  if (locked.repository && earlier.repository && locked.repository !== earlier.repository)
    return [`${at} has provenance from ${locked.repository}, but ${previous} from ${earlier.repository}`];
  if (locked.repositoryId && earlier.repositoryId && locked.repositoryId !== earlier.repositoryId)
    return [`${at} has provenance from ${locked.repository} with repository id ${locked.repositoryId}, but ${previous} with id ${earlier.repositoryId}`];
  return [];
}
