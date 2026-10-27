# Third-party notices

The software uses the third-party components below. This repository is
licensed under AGPL-3.0-or-later (`README.md`); all of them are
permissive.

As of 2026-10-24.

## Shipped with the software

### Web app (npm, runtime)

| Package | Version | License | Source |
|---|---|---|---|
| `preact` | 11.0.1 | MIT | https://github.com/preactjs/preact |
| `@preact/signals` | 2.11.3 | MIT | https://github.com/preactjs/signals |
| `@preact/signals-core` | 1.14.4 | MIT | https://github.com/preactjs/signals |
| `uplot` | 1.6.32 | MIT | https://github.com/leeoniya/uPlot |
| `lucide-preact` | 1.53.0 | ISC | https://github.com/lucide-icons/lucide |

### C++ (single headers, pinned with SHA-256 in `cmake/deps.cmake`)

| Library | Version | License | Source |
|---|---|---|---|
| nlohmann/json | 3.11.3 | MIT; contains Hedley (CC0-1.0) and parts of Google Abseil (Apache-2.0) | https://github.com/nlohmann/json |
| cpp-httplib | 0.54.1 | MIT (simulator only) | https://github.com/yhirose/cpp-httplib |

## Used for building and testing only, not shipped

| Tool | License |
|---|---|
| doctest 2.4.11 | MIT |
| Vite, `@preact/preset-vite`, TypeScript | MIT, Apache-2.0 |
| Playwright | Apache-2.0 |
