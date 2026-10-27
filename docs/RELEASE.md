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
   the three packages, each with a smoke test; writes `SHA256SUMS`; attests
   the files' provenance; and creates the GitHub release, marked as a
   pre-release, with the section as notes.
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
| `SHA256SUMS` | the checksums of the three packages |

Each package holds the program, `README.txt` (how to start it, in English
and German), `LICENSE.txt` and `THIRD_PARTY_LICENSES.txt`. The provenance
attestation is on the release's files (`gh attestation verify <file>
--repo Wokesay/doseph-software`).

## Security fixes

A fix for a vulnerability goes into the next prototype, with an advisory
(`SECURITY.md`); only the latest prototype gets fixes.
