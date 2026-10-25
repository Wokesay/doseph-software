# Doseph – software

Doseph is a mixing robot for nutrient solution: a standalone device at the
nutrient tank that only mixes. A circulation pump pushes tank water through
the device. The robot proves the flow, measures pH, EC and temperature,
doses pH− and nutrients per recipe in small doses, and mixes again only
after the run-through time the user set for the tank – not all the time.

This repository holds its software: the core (C++17), a simulator with a
digital twin of tank and device, and the web app the device serves. The
firmware of the device follows. The software is operated only through the
web app on the device, on the home network: no cloud, no account, no
store app.

> Status: development. Not released; for testing only.

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
| `sim/` | the simulator: twin of tank, cell, flow, pumps and probes; HTTP server for the web app |
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

## License

Not decided yet. The files carry the license of the code they come from
(`LICENSE`, AGPL-3.0-or-later, with `ADDITIONAL_PERMISSION.md`) until the
decision is made.
