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

## What the prototype does

The prototype is the robot's real software running against a simulated
device, tank, pumps and probes. In it you can watch and steer:

- **The mixing check** – pump on, flow proven, measuring chamber flushed,
  readings stable, nutrients before pH−, small doses in partial runs,
  mixed in with flow, measured again, then a hold. The first dose of a
  new tank is a small test dose that checks the tank volume you entered.
- **Why it doses – or not** – a checklist on the status page for
  nutrients and pH−, and every lock and hold with its reason.
- **Recipes** – your own or from templates, pairs that keep their ratio,
  a recipe in force for a number of days.
- **Canisters and pumps** – roles, sizes and levels counted from use,
  "Refilled", pump calibration into a cup, filling the lines.
- **Probes** – pH calibration with two buffers, EC with one reference
  solution, temperature compensation.
- **Safety** – no dose without flow, the Stop on every page, locks after
  a restart, maintenance mode while you work at tank or device.
- **Data** – history of pH, EC, temperature and flow with CSV export,
  backup and restore of all settings, and "Report a problem" with a
  diagnostics package.
- **The simulator panel** – time-lapse, the true tank, refill or top up,
  and faults to try: no flow, a blocked or empty pump, a probe that stops
  answering, freezes or jumps, a power cut.

The app speaks English and German.

## Try a prototype

1. Download the package for your system (Windows, macOS or Linux) from
   [Releases](https://github.com/Wokesay/doseph-software/releases) and
   unpack it. Each release lists the checksums of its files, with SBOMs
   and a provenance attestation ([`docs/RELEASE.md`](docs/RELEASE.md)).
2. Start `doseph-simulator` (on Windows `doseph-simulator.exe`) with a
   double-click. Windows may warn about an unknown app, and macOS needs
   one command first: both are explained in the `README.txt` of the
   package.
3. Your browser opens the app at http://127.0.0.1:8080 (8081 … if the
   port is busy). The window shows the address and the demo password;
   sign in with it.
4. Watch the status page work through a check. Open the simulator panel
   to speed up time, change the tank or switch on a fault, and see how
   the robot reacts.
5. Tell us what you think with the
   [feedback form](https://github.com/Wokesay/doseph-software/issues/new?template=feedback.yml),
   or report a bug from Settings → Report a problem. Where to report what
   is in [`SUPPORT.md`](SUPPORT.md).

The demo runs in memory only: closing it starts it over. To keep your
data, start it with `--data` and a folder ([`docs/SIMULATOR.md`](docs/SIMULATOR.md)).

## Build from source

Requirements: CMake 3.20+, a C++17 compiler, Ninja, Node.js 22
(`.nvmrc`).

```sh
cmake -S . -B build -G Ninja && cmake --build build
(cd web && npm ci && npm run build)
build/doseph_sim_server --scenario demo --web web/dist --password demo-password
```

Then open http://localhost:8080 and sign in with `demo-password`. The
simulator button opens the twin: time-lapse, the true tank, faults.
More in [`docs/SIMULATOR.md`](docs/SIMULATOR.md).

All checks, as CI runs them: `tools/ci.sh`.

## Layout

| Path | What |
|---|---|
| `core/` | platform-neutral core: sensor truth, dose planning, the mixing cycle, gateway, watchdog, configuration, API |
| `sim/` | the simulator: twin of tank, measuring chamber, flow, pumps and probes; HTTP server for the web app |
| `web/` | the web app (Preact, TypeScript); end-to-end tests against the simulator |
| `tests/core/` | the core's tests (doctest) |
| `tools/` | checks: architecture rules, text keys, the rule register, licenses and provenance of dependencies; packaging, SBOMs and release helpers |
| `docs/` | concept, decisions (SD), rationale register (RAT), invariants, later steps, simulator, release, provenance |

## Documentation

- [`docs/CONCEPT.md`](docs/CONCEPT.md) – how the software is built and the rules it keeps
- [`docs/DECISIONS.md`](docs/DECISIONS.md) – software decisions (SD-105 the mixing cycle)
- [`docs/RATIONALE.md`](docs/RATIONALE.md) – the rules learned from practice (RAT-xxx)
- [`docs/INVARIANTS.md`](docs/INVARIANTS.md) – the rules by module, with their code and tests
- [`docs/LATER.md`](docs/LATER.md) – what later steps of the order of work rebuild
- [`docs/SIMULATOR.md`](docs/SIMULATOR.md) – running and steering the simulator
- [`docs/PROVENANCE.md`](docs/PROVENANCE.md) – where the code comes from
- [`docs/RELEASE.md`](docs/RELEASE.md) – how a prototype is released
- [`CHANGELOG.md`](CHANGELOG.md) – what changed in each prototype
- [`SUPPORT.md`](SUPPORT.md), [`SECURITY.md`](SECURITY.md),
  [`CONTRIBUTING.md`](CONTRIBUTING.md), [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md) –
  getting help, reporting a vulnerability, how the project takes
  contributions, how we treat each other

## License

AGPL-3.0-or-later ([`LICENSE`](LICENSE)), with an additional permission for the
binary-only libraries of the device's platform
([`ADDITIONAL_PERMISSION.md`](ADDITIONAL_PERMISSION.md)). Decided by the
owner on 2026-10-27 (SD-116).
