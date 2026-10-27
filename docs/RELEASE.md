# Releases

A **prototype** is the simulator with the web app built in, as a download
for Linux, macOS and Windows (SD-116). It is for trying the software, not
for dosing into a real tank. One comes out weekly while something new is
merged, numbered `0.1.0-proto.N` from `0.1.0-proto.5` on. Firmware and
device releases follow later with their own steps (signed updates).

## Steps

1. **Release pull request** – from `main`:
   - `VERSION` and the `version` in `web/package.json` set to the new
     version (the release workflow checks that they match);
   - in `CHANGELOG.md`, the `[Unreleased]` section becomes
     `## [0.1.0-proto.N] – YYYY-MM-DD`, with a new empty `[Unreleased]`
     above it; the section is what the release notes say;
   - CI green, including `Packages` (all three systems) and the `Release`
     dry run.
2. **The owner merges** it.
3. **Tag** the merge commit on `main`: `git tag v0.1.0-proto.N <commit>`
   and push the tag. The `Release` workflow checks that the tag matches
   `VERSION`, lies on `main` and has its `CHANGELOG.md` section; builds
   the three packages, each with a smoke test; writes the two SBOMs and
   `SHA256SUMS`; attests the files' provenance; and creates the GitHub
   release, marked as a pre-release, with the section as notes.
4. **After the release:** `VERSION` and `web/package.json` go back to a
   development version (`0.1.0-dev`) with the next code change, so a build
   from `main` never claims to be the release.

A failed release run publishes nothing: fix the cause on `main`, delete
the tag, and tag again (a new prototype number if the release was
already published).

## What the release contains

| File | What |
|---|---|
| `doseph-simulator-<version>-linux-x64.zip` | the simulator for Linux (x86-64) |
| `doseph-simulator-<version>-macos-universal.zip` | for macOS (Apple silicon and Intel) |
| `doseph-simulator-<version>-windows-x64.zip` | for Windows |
| `sbom-web.cdx.json` | SBOM (CycloneDX) of the web app's runtime npm packages |
| `sbom-simulator-cpp.cdx.json` | SBOM (CycloneDX) of the C++ libraries in the simulator |
| `SHA256SUMS` | the checksums of all files above |

Each package holds the program, `README.txt` (how to start it, in English
and German), `LICENSE.txt` and `THIRD_PARTY_LICENSES.txt`. The provenance
attestation is on the release's files (`gh attestation verify <file>
--repo Wokesay/doseph-software`).

The SBOMs list what ships, not the build tools. The web SBOM is npm's SBOM
of `web/package-lock.json`, filtered to the runtime packages
(`tools/sbom_web.mjs`; `npm sbom --omit dev` would leave out `preact`,
which a build tool has as a peer); it fails rather than list something
the lockfile does not have. The C++ SBOM lists the header libraries
pinned in `cmake/deps.cmake` (`tools/sbom_cpp.mjs`), with their SHA-256;
a new library there fails it until it is added with its license. The
statically linked compiler runtimes are not listed yet.

## Supply chain

- **Actions** are GitHub's own, pinned by full commit SHA with the version
  as a comment; the release's `publish` is the only job that can write,
  and it builds nothing (`tools/workflows.test.mjs`). Dependabot proposes
  updates after a cooldown of 7 days.
- **npm packages** are pinned by their sha512 in `web/package-lock.json`,
  each from its own record on the npm registry. Wherever they are
  installed (CI, `tools/ci.sh`, the simulator packages), `npm ci` runs
  with `--ignore-scripts` and `npm audit signatures` right after it: a
  missing or invalid registry signature or an invalid provenance
  attestation fails the build. That proves the registry's signed record,
  not the safety of a release from a hijacked maintainer account; against
  that stand the cooldown, the lockfile pin and the provenance check
  below. When the check fails, `tools/signatures_report.mjs` shows npm's
  log of it and asks npm again for the reason; a check that passes when
  asked again is no clearance.
- **Provenance:** before CI installs the packages,
  `tools/check_provenance.mjs` finds for every package in the lockfile
  the highest lower version that was published with provenance. If the
  locked version has none, or has it from another source repository, the
  check fails, since a hijacked account typically publishes by hand. A
  case accepted after review goes into `tools/provenance-exceptions.json`
  with its exact problem and the reason; an entry no locked package has
  any more fails too.
- **Licenses:** CI runs `reuse lint`, with `reuse` installed only from the
  hashed `tools/requirements-reuse*.txt` (wheels only, except `reuse`
  itself, built from its hashed source with the hashed build backend),
  and `tools/check_licenses.mjs`: every npm package needs an allowed
  license, every runtime package a row in `THIRD_PARTY_NOTICES.md`, and
  every package the app imports must be a runtime dependency.
- The release uses no cache and the Node pinned in `.nvmrc`, as every job
  does.

## Security fixes

A fix for a vulnerability goes into the next prototype, with an advisory
(`SECURITY.md`); only the latest prototype gets fixes.
