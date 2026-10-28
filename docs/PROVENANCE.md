# Provenance

This repository started on 2026-10-24 from the software of a grow
controller by the same contributors, under the same license
(AGPL-3.0-or-later). The product changed from a grow controller to a
mixing robot, and the reusable parts were carried over **without the git
history**: this repository's first commit holds them. Everything Doseph
needs – code, rules and their reasons, decisions – is in this repository
(SD-117); nothing here needs the grow controller's repositories.

Contributors to the carried-over code: Wokesay and Claude (Anthropic),
working in Claude Code sessions.

## What was carried over

| Here | Carried over from the grow controller | Changed |
|---|---|---|
| `core/src/common.cpp`, `core/include/doseph/common.hpp` | the common helpers | trimmed |
| `core/src/clock.cpp`, `core/src/auth.cpp`, `core/src/sha256.cpp` | clock, sign-in, SHA-256 | renamed to `doseph` |
| `core/src/events.cpp`, `core/src/history.cpp` | event log and history | the robot's series and event types |
| `core/src/messages.cpp` | the message table | a new message table for the robot |
| `core/src/truth.cpp` (sensor truth) | sensor truth | fixed channels pH, EC, temperature, flow; EC compensated to 25 °C (RAT-089) |
| `core/src/plan.cpp` (dose planning) | the mixing logic | the EC and pH rounds, partial runs and effects; the bucket mixes dropped |
| `core/src/journal.cpp` (dosing journal) | the booking of runs | written before a pump starts (SD-106) |
| `core/src/gateway.cpp` (actuators, doser) | dosing and the actuator path | no dose without flow (SD-105) |
| `core/src/watchdog.cpp` | the watchdog | the robot's checks |
| `core/src/config.cpp`, `core/src/api.cpp`, `core/src/robot.cpp` | configuration, API, the controller's core | rewritten for the robot |
| `core/src/cycle.cpp` (the mixing cycle) | EC and pH control | rewritten as one state machine (SD-105) |
| `core/data/templates.json` | the templates of the catalog | recipe templates only |
| `sim/` | the simulator | a twin of the robot: tank, cell, flow, pumps, probes |
| `web/` | the web app | stripped to the robot: status, recipe, tank, history, channels & probes, settings |
| `tests/core/` | the tests | rewritten where the code was; kept where it was carried over |
| `docs/RATIONALE.md` | the rationale register | the same RAT IDs; grow-only rules marked dropped |
| `tools/i18n_keys.test.mjs`, `tools/arch_check.sh`, `cmake/` | the checks and build files | adapted |
| `LICENSE`, `ADDITIONAL_PERMISSION.md`, `LICENSES/` | the same | unchanged; the license was confirmed with SD-116 |

Left behind as grow-only: light, climate, watering, tank refill, drain,
devices and sensor slots on the bus, Home Assistant, mixing in a bucket or
watering can, grow phases and harvest, stock of bottles. What later steps
of the order of work (#10) rebuild from them is in `docs/LATER.md`.

## Security fixes that came along

All fixes of the grow controller came along with the carry-over; it got
no code changes after it (checked on 2026-10-27 for #10). The security
fixes among them, and where they stand here:

| Fix | Here |
|---|---|
| The first password needs a setup code that only the device shows | `core/src/auth.cpp`, `core/src/api.cpp`: the device shows the code, the simulator prints it |
| The sign-in lockout counts per client | `core/src/auth.cpp` |
| Random numbers from the operating system, not RDSEED (CVE-2025-62626) | `sim/simulation.cpp` (`getentropy()`) |
| An import gives a calibration only to a class that offers it | `core/src/robot.cpp` (`importConfig`): the robot has no other classes; a backup's probe calibrations are checked as if made now, and a broken one refuses the file |
| A new sign-in ends the session used longest ago | `core/src/auth.cpp` |

## IDs

- Software decisions start at SD-105 (`docs/DECISIONS.md`).
- `RAT-001` … keep the IDs the rules had in the grow controller; a rule
  the robot does not need stays listed as dropped (`docs/RATIONALE.md`).
- `#n` is an issue or pull request of this repository.
- Nothing refers to the grow controller's issues, decisions, commits or
  repositories (SD-117); `tools/arch_check.sh` checks it.
