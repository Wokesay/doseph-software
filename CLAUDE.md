# doseph-software – project context

The software of Doseph, a mixing robot for nutrient solution: core
(C++17), simulator and web app; later the firmware of the device. Product,
hardware and costs live in a private product repository; Claude has no
access to it, not even by attaching it during a session.

## What the product is

A standalone device at the nutrient tank that only mixes. A circulation
pump pushes tank water through the device. The robot proves flow,
measures pH, EC and temperature, doses pH− and nutrients per recipe in
small doses, and mixes again only after the run-through time the user set
for the tank – not all the time. Climate, light, watering, tank refill and
drain are out. Target: home growers, later greenhouses. Operation only
through the web app on the device: no store app, no server.

## Conversation mode

- Claude talks to the project owner in German, short, and ends each answer
  with the next 2–3 steps. Everything written to this repository and its
  GitHub pages is English: docs, code, commits, PRs, issues.
- **Idea or question** → discuss, options with a recommendation, change
  nothing. **"entschieden" (decided)** → an SD entry in `docs/DECISIONS.md`
  as a PR. **"umsetzen" (implement)** → the work as a PR, with tests.
- Open decisions and actions for the owner one at a time, with context,
  options and a recommendation; never bundled at the end of a message.

## Who decides what

- **Control details** (locks, releases, thresholds, sequences, waits) are
  decided here by the minimal principle: affordable, simple, a safety frame
  instead of guarding every scenario. A crash or restart switches off
  everything that can do harm (R6). Recorded as SD.
- **Principles, costs and hardware** are decided by the product session.
  Claude files a product question as an English issue with the label
  `product-question`: the question, its software basis (SD, RAT), the
  options and a recommendation. The product session reads this repository
  and writes issues and comments, never code.
- **Nothing private in this repository**, also not in issues, in case it
  becomes public: no hardware data (schematic, layout, bill of materials,
  exact board data), no names, numbers or texts of private sources. Refer to
  them only as "private sources" or "reference installation". No prices.
- **License:** not decided yet. Until it is, `LICENSE`,
  `ADDITIONAL_PERMISSION.md` and the SPDX header lines stay as they are.

## IDs

- `SD-xxx` software decisions (`docs/DECISIONS.md`), continuing the
  predecessor's numbering from SD-105; `RAT-xxx` rules from practice
  (`docs/RATIONALE.md`), the same IDs as in the predecessor.
- `gc#n` and `gc PD-xxx` in code and docs are issues and product decisions
  of the predecessor growcontroller-software (`docs/PROVENANCE.md`); `#n`
  alone is an issue of this repository.
- Several sessions may work in parallel: a new SD or RAT ID is final only
  on `main`; renumber right before merging if needed.

## Architecture rules (checked by `tools/arch_check.sh`)

- **R1** Only the actuator gateway (`core/src/gateway.cpp`) switches pumps.
- **R2** The watchdog reads only the read model and the configuration.
- **R5** A missing value is never 0 (no `value_or(0)`), and never a dose.
- **R6** After a crash or restart everything is off; the robot reports and
  does not resume a dose on its own.
- **R7** Settings only tighten within hard limits.
- **R8** One event loop with an injected clock; the core has no threads,
  no sleeps and no wall clock of its own.
- No dose without proven flow (SD-105). A dose is written to the dosing
  journal before its pump starts (SD-106).

## Working

- Build and test: `tools/ci.sh` (core, simulator, sanitizers, text keys,
  web app, end-to-end). Fast loop: `cmake --build build && tools/core_tests.sh`.
- Run the simulator: `docs/SIMULATOR.md`.
- New domain rule: next free RAT ID, entry in `docs/RATIONALE.md`, a test
  that names it.
- New texts: English key in `core/src/messages.cpp` with its German twin in
  `web/src/lang/msg.ts`; app texts in `web/src/lang/*.ts`, German and
  English (`tools/i18n_keys.test.mjs`).
- Commit with clear messages, open a draft PR; the owner merges.
