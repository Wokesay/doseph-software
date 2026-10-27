# Doseph – software

Doseph is a mixing robot for nutrient solution: a standalone device next
to the nutrient tank. It draws tank water through itself, proves the
flow, measures pH, EC and temperature, and doses pH− and up to three
nutrients per recipe in small doses. It mixes again only once the tank
has mixed, not all the time.

This repository holds its software: the core (C++17), a simulator with a
digital twin of tank and device, and the web app the device serves. The
firmware of the device follows. Control always runs on the device: it
works offline on the home network, and a server or an account is never
needed for it.

> **Status: prototype.** For trying in the simulator only; do not dose
> into a real tank with it. A prototype comes out weekly while there is
> something new.

## Try a prototype

Download the latest prototype for your system under
[Releases](https://github.com/Wokesay/doseph-software/releases), start it
and open the address it prints. Tell us what you think with the
[feedback form](https://github.com/Wokesay/doseph-software/issues/new?template=feedback.yml);
where to report what is in [`SUPPORT.md`](SUPPORT.md).

## Build and try

Requirements: CMake 3.20+, a C++17 compiler, Ninja, Node.js 22
(`.nvmrc`).

```sh
cmake -S . -B build -G Ninja && cmake --build build
(cd web && npm ci && npm run build)
build/doseph_sim_server --scenario demo --web web/dist --password demo-password
```

Then open http://localhost:8080 and sign in with `demo-password`. The
simulator button opens the twin: time-lapse, the true tank, faults.
More in `docs/SIMULATOR.md`.

All checks, as CI runs them: `tools/ci.sh`.

## Layout

| Path | What |
|---|---|
| `core/` | platform-neutral core: sensor truth, dose planning, the mixing cycle, gateway, watchdog, configuration, API |
| `sim/` | the simulator: twin of tank, measuring chamber, flow, pumps and probes; HTTP server for the web app |
| `web/` | the web app (Preact, TypeScript); end-to-end tests against the simulator |
| `tests/core/` | the core's tests (doctest) |
| `tools/` | checks: architecture rules, text keys, test runner |
| `docs/` | concept, decisions (SD), rationale register (RAT), simulator, provenance |

## Documentation

- `docs/CONCEPT.md` – how the software is built and the rules it keeps
- `docs/DECISIONS.md` – software decisions (SD-105 the mixing cycle)
- `docs/RATIONALE.md` – the rules learned from practice (RAT-xxx)
- `docs/SIMULATOR.md` – running and steering the simulator
- `docs/PROVENANCE.md` – where the code comes from
- `docs/RELEASE.md` – how a prototype is released
- `SUPPORT.md`, `SECURITY.md`, `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md` –
  getting help, reporting a vulnerability, how the project takes
  contributions, how we treat each other

## License

AGPL-3.0-or-later (`LICENSE`), with an additional permission for the
binary-only libraries of the device's platform
([`ADDITIONAL_PERMISSION.md`](ADDITIONAL_PERMISSION.md)). Decided by the
owner on 2026-10-27 (SD-116).
