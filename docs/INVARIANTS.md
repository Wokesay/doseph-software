# Invariants: the domain rules by module

The domain rules of [`RATIONALE.md`](RATIONALE.md), grouped by the
module that keeps them, with where they are implemented and the test
files that cover them. The register holds each rule in full, its reason
and the names of its tests; this page is the map. `tools/rationale.test.mjs`
checks that every rule in force is placed here exactly once, that the
files and tests the register names exist, and that the status fits.

Status: **✓** implemented and tested · **◐** implemented, without a test
of its own.

## Code rules and the two dosing invariants

| Rule | Source | Status | Kept by |
|---|---|---|---|
| R1 Only the actuator gateway switches pumps | `CLAUDE.md` | ✓ | `tools/arch_check.sh` |
| R2 The watchdog reads only the read model and the configuration | `CLAUDE.md` | ✓ | `tools/arch_check.sh` (RAT-074) |
| R5 A missing value is never 0, and never a dose | `CLAUDE.md` | ✓ | `tools/arch_check.sh` (no `value_or(0)`); RAT-006, RAT-015 |
| R6 After a crash or restart everything is off; nothing resumes | `CLAUDE.md` | ✓ | RAT-007, RAT-028 |
| R7 Settings only tighten within hard limits | `CLAUDE.md` | ✓ | `Limits::bounded`, `validateConfig` · test_config.cpp |
| R8 One event loop with an injected clock; no threads, sleeps or wall clock in the core | `CLAUDE.md` | ✓ | `tools/arch_check.sh` |
| No dose without proven flow | SD-105 | ✓ | `MixCycle`, `Actuators::startRun` · test_cycle.cpp |
| A dose is written to the dosing journal before its pump starts | SD-106 | ✓ | `Doser`, `DoseJournal` · test_journal.cpp, test_cycle.cpp |

## Cycle: the check (SD-105)

| Rule | Rationale | Status | Implementation · tests |
|---|---|---|---|
| Recipes hold nutrients only; pH is corrected after EC and waits while a nutrient round is open | RAT-004 | ✓ | `plan.hpp`, `config.cpp`, `cycle.cpp` · test_config.cpp, test_cycle.cpp |
| EC is only raised; an EC above target is left as it is, with the reason | RAT-012 | ✓ | `cycle.cpp` · test_cycle.cpp |
| One line says what the robot does; everything it leaves as it is stands as a note with its reason, and the checklist says why it doses or not | RAT-037 | ✓ | `cycle.hpp`, `cycle.cpp`, `status.tsx` · test_cycle.cpp, robot.spec.ts |
| pH is only lowered; pH− without effect latches; a pH below target is left as it is, with the reason | RAT-041 | ✓ | `cycle.hpp`, `cycle.cpp`, `truth.cpp` · test_cycle.cpp, test_truth.cpp |
| No pH dose on an old reading: the cycle measures afresh after flushing before every decision | RAT-043 | ✓ | `cycle.cpp` · test_cycle.cpp |
| No pH dose below the EC floor, nor within 240 s after a nutrient dose | RAT-046 | ✓ | `config.hpp`, `cycle.hpp`, `cycle.cpp` · test_cycle.cpp |
| After a dose it mixes with flow before it measures again; pH waits longer than EC | RAT-052 | ✓ | `config.hpp`, `cycle.cpp`, `world.cpp` · test_cycle.cpp |
| An EC round keeps a reserve for the pH− dose that follows | RAT-053 | ✓ | `cycle.cpp` · test_cycle.cpp |
| The wait after a dose covers the time to tolerance (the user's wait, else the suggestion from volume and flow) | RAT-058 | ◐ | `cycle.cpp` · no test of its own |
| A latch only after a dose without effect or a test dose that did not fit; its reason is frozen | RAT-062 | ✓ | `watchdog.cpp` · test_cycle.cpp |

## Dose planning and learning

| Rule | Rationale | Status | Implementation · tests |
|---|---|---|---|
| A pH− dose is capped by its effect (0.3 pH, 0.3 ml/L, the set maximum); no run under 1 s | RAT-050 | ✓ | `config.hpp`, `plan.hpp`, `cycle.cpp`, `gateway.cpp`, `plan.cpp` · test_plan.cpp |
| A pair is scaled by one factor at every limit and keeps its ratio; a part missing from a round cut short is caught up first (SD-114) | RAT-054 | ✓ | `config.hpp`, `plan.hpp`, `config.cpp`, `plan.cpp`, `world.cpp`, `cycle.cpp` · test_config.cpp, test_plan.cpp, test_cycle.cpp |
| Each round doses 0.8 × gap ÷ effect × volume; large doses go in equal partial runs | RAT-055 | ✓ | `config.hpp`, `cycle.hpp`, `plan.hpp`, `cycle.cpp`, `gateway.cpp` · test_cycle.cpp, test_plan.cpp |
| A small learned effect never widens the EC cap | RAT-056 | ✓ | `config.hpp`, `plan.hpp`, `config.cpp`, `plan.cpp` · test_config.cpp, test_plan.cpp |
| An effect is learned only from clean rounds; a test dose only when it passed | RAT-057 | ✓ | `config.hpp`, `cycle.hpp`, `plan.hpp`, `cycle.cpp` · test_cycle.cpp |
| Recipe templates follow the chart; the pH target is the middle of the range | RAT-066 | ◐ | `templates.json`, `config.hpp`, `store.ts` · no test of its own |
| The order of a recipe's parts is the dosing order | RAT-080 | ◐ | `config.hpp` · no test of its own |
| Start effects err on the side of the smaller dose | RAT-081 | ✓ | `plan.hpp` · test_plan.cpp |
| A recipe gives ratio and order; a fixed part goes in per litre and is never scaled by EC | RAT-085 | ✓ | `config.hpp`, `plan.hpp`, `config.cpp`, `plan.cpp`, `robot.cpp`, `api.ts` · test_config.cpp, test_plan.cpp, test_api.cpp |

## Gateway, pumps and canisters

| Rule | Rationale | Status | Implementation · tests |
|---|---|---|---|
| One pump job at a time and none while the cycle doses; a senseless calibration result is refused | RAT-003 | ✓ | `config.cpp`, `robot.cpp` · test_cycle.cpp |
| After a restart every pump is off and nothing resumes; a run is at most 60 s | RAT-007 | ✓ | `journal.hpp`, `robot.cpp`, `world.cpp` · test_cycle.cpp |
| One dosing run at a time; a run that does not report back in time is switched off and booked as failed | RAT-018 | ✓ | `gateway.cpp` · test_journal.cpp |
| Stop switches everything off at once, books what ran and holds across a restart until Resume | RAT-036 | ✓ | `config.hpp`, `cycle.hpp`, `gateway.hpp`, `robot.cpp` · test_cycle.cpp |
| A calibration or line fill is no dose into the tank; flows and amounts come from the actual run time | RAT-070 | ✓ | `config.hpp`, `gateway.hpp`, `hardware.hpp`, `journal.hpp`, `robot.hpp`, `gateway.cpp`, `robot.cpp` · test_cycle.cpp, test_journal.cpp |
| A canister below 150 ml (pH− 20 ml) by the count is a problem; a dose the count cannot cover waits (SD-119) | RAT-071 | ✓ | `config.hpp`, `config.cpp`, `cycle.cpp`, `gateway.cpp`, `robot.cpp`, `watchdog.cpp` · test_cycle.cpp |

## Sensor truth

| Rule | Rationale | Status | Implementation · tests |
|---|---|---|---|
| A missing value is never 0 | RAT-006 | ✓ | `common.hpp`, `plan.hpp`, `truth.hpp`, `robot.cpp` · test_config.cpp, test_truth.cpp, arch_check.sh |
| pH is dosed only on a usable reading and only while it has an effect | RAT-020 | ✓ | `cycle.hpp`, `truth.cpp` · test_truth.cpp |
| A failed sensor and a missing calibration are separate findings | RAT-021 | ✓ | `watchdog.cpp` · test_watchdog.cpp |
| Freshness and a standing raw value are checked separately | RAT-023 | ✓ | `readmodel.hpp`, `truth.hpp`, `truth.cpp` · test_truth.cpp |
| Control reads only usable values; without a valid calibration, no control value; a backup's calibration is checked as if made now | RAT-025 | ✓ | `common.hpp`, `config.hpp`, `readmodel.hpp`, `truth.hpp`, `truth.cpp` · test_truth.cpp, test_api.cpp |
| A calibration is refused outside its slope, zero point or factor limits | RAT-026 | ✓ | `truth.cpp` · test_truth.cpp |
| A signal is stale by the age of its last measurement, never by a change of value | RAT-027 | ✓ | `truth.cpp` · test_truth.cpp |
| An unexplained jump locks the signal until 15 min of calm; a skipped dose is never silent | RAT-039 | ✓ | `config.hpp`, `readmodel.hpp`, `truth.cpp` · test_truth.cpp |
| An announced intervention explains jumps: a new tank, the cycle's own dose, maintenance mode | RAT-042 | ✓ | `robot.hpp`, `truth.hpp`, `cycle.cpp`, `robot.cpp`, `truth.cpp` · test_cycle.cpp, test_truth.cpp, robot.spec.ts |
| Jump locks survive a restart; no dose on a reading from before it | RAT-044 | ✓ | `config.hpp`, `readmodel.hpp`, `truth.hpp`, `truth.cpp` · test_truth.cpp |
| A standing raw value is a fault only for a signal that should be noisy | RAT-059 | ✓ | `readmodel.hpp`, `truth.hpp`, `truth.cpp` · test_truth.cpp |
| Sensor truth always runs, also while stopped or calibrating | RAT-075 | ◐ | `truth.hpp` · no test of its own |
| EC is compensated to 25 °C; without a water temperature it is no value for control | RAT-089 | ✓ | `readmodel.hpp`, `truth.hpp`, `robot.cpp`, `truth.cpp` · test_truth.cpp |

## State across a restart

| Rule | Rationale | Status | Implementation · tests |
|---|---|---|---|
| After a restart outputs are off, while recorded facts survive: latches, locks, effects, the Stop, levels, a pair's missing parts | RAT-028 | ✓ | `robot.cpp` · test_config.cpp |
| Latches survive a restart; an unreadable state starts stopped | RAT-063 | ✓ | `config.hpp`, `config.cpp`, `robot.cpp`, `watchdog.cpp` · test_config.cpp |

## Watchdog

| Rule | Rationale | Status | Implementation · tests |
|---|---|---|---|
| Target checks are neutral for 120 s after a start; the cycle works at once | RAT-005 | ✓ | `watchdog.hpp` · test_watchdog.cpp |
| A tolerance is never 0 | RAT-008 | ✓ | `config.hpp`, `config.cpp`, `cycle.cpp` · test_config.cpp |
| No stand-in values: missing data is a problem, never neutral and never a dose | RAT-015 | ✓ | `config.hpp`, `plan.hpp`, `gateway.cpp`, `watchdog.cpp` · test_plan.cpp, test_watchdog.cpp |
| The alarm band is wider than the control band | RAT-033 | ✓ | `watchdog.hpp`, `watchdog.cpp` · test_watchdog.cpp |
| Normal states are counted, only deviations are listed | RAT-072 | ✓ | `watchdog.cpp` · test_watchdog.cpp |
| An assessment older than 3 min is red | RAT-073 | ✓ | `watchdog.hpp`, `robot.cpp` · test_watchdog.cpp |
| The watchdog only assesses and has no path to the pumps | RAT-074 | ✓ | `watchdog.hpp` · arch_check.sh |

## History and messages

| Rule | Rationale | Status | Implementation · tests |
|---|---|---|---|
| The history keeps gaps as gaps, also as CSV; pH, EC and temperature only with flow | RAT-016 | ✓ | `robot.cpp`, `api.cpp` · test_api.cpp, robot.spec.ts |
| A message stays short enough to become a notification | RAT-045 | ✓ | `messages.cpp` · i18n_keys.test.mjs |

## Simulator

| Rule | Rationale | Status | Implementation · tests |
|---|---|---|---|
| The simulator's noise is measured; anything else is marked as an assumption | RAT-082 | ◐ | `world.cpp` · no test of its own |
