# Provenance

This repository started from the software of the grow controller
**growcontroller-software** (`Wokesay/growcontroller-software`, public,
AGPL-3.0-or-later), at commit
`a3b00d16fb9bb2c84fa65ebe1a803e989e69492e` (2026-10-24). The product
changed from a grow controller to a mixing robot, and the reusable parts
were carried over **without the git history**: this repository's first
commit holds them. The predecessor's history, issues and decisions stay
readable in its repository, which is no longer developed.

Contributors to the carried-over code: Wokesay and Claude (Anthropic),
working in Claude Code sessions.

## What was carried over

| Here | From growcontroller-software | Changed |
|---|---|---|
| `core/src/common.cpp`, `core/include/doseph/common.hpp` | `core/include/gc/common.hpp` | trimmed |
| `core/src/clock.cpp`, `core/src/auth.cpp`, `core/src/sha256.cpp` | the files of the same name | renamed to `doseph` |
| `core/src/events.cpp`, `core/src/history.cpp` | the files of the same name | the robot's series and event types |
| `core/src/messages.cpp` | `core/src/messages.cpp` | a new message table for the robot |
| `core/src/truth.cpp` (sensor truth) | `core/src/truth.cpp` | fixed channels pH, EC, temperature, flow; EC compensated to 25 °C (RAT-089) |
| `core/src/plan.cpp` (dose planning) | `core/src/mix.cpp` | the EC and pH rounds, partial runs and effects; the bucket mixes dropped |
| `core/src/journal.cpp` (dosing journal) | the run booking in `core/src/hub.cpp` | written before a pump starts (SD-106) |
| `core/src/gateway.cpp` (actuators, doser) | `core/src/dosing.cpp` and the actuator path of `core/src/hub.cpp` | no dose without flow (SD-105) |
| `core/src/watchdog.cpp` | `core/src/watchdog.cpp` | the robot's checks |
| `core/src/config.cpp`, `core/src/api.cpp`, `core/src/robot.cpp` | `config.cpp`, `api.cpp`, `hub.cpp` | rewritten for the robot |
| `core/src/cycle.cpp` (the mixing cycle) | `core/src/control.cpp` (EC and pH control) | rewritten as one state machine (SD-105) |
| `core/data/templates.json` | the templates in `catalog/catalog.json` | recipe templates only |
| `sim/` | `sim/` | a twin of the robot: tank, cell, flow, pumps, probes |
| `web/` | `web/` | stripped to the robot: status, recipe, tank, history, pumps & probes, settings |
| `tests/core/` | `tests/core/` | rewritten where the code was; kept where it was carried over |
| `docs/RATIONALE.md` | `docs/RATIONALE.md` | the same RAT IDs; grow-only rules marked dropped |
| `tools/i18n_keys.test.mjs`, `tools/arch_check.sh`, `cmake/` | `tools/`, `cmake/` | adapted |
| `LICENSE`, `ADDITIONAL_PERMISSION.md`, `LICENSES/` | the same | unchanged until the license is decided |

Left behind as grow-only: light, climate, watering, tank refill, drain,
devices and sensor slots on the bus, Home Assistant, mixing in a bucket or
watering can, grow phases and harvest, stock of bottles.

## References to the predecessor

- `SD-001` to `SD-103` are the predecessor's software decisions; this
  repository continues the numbering from `SD-105` (`docs/DECISIONS.md`).
- `RAT-001` to `RAT-088` keep their IDs (`docs/RATIONALE.md`).
- `gc#n` is issue or pull request n of growcontroller-software; `gc PD-xxx`
  is one of its product decisions. `#n` alone is an issue of this
  repository.
