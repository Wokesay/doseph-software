# Changelog

All notable changes to this software. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- A return pump that runs a minute without a pause is dry or blocked
  (SD-109): the robot holds it off, latches it with an alarm and holds
  every pump until a person releases it; a restart holds it off again.
  The robot sees whether the return pump runs, not the chamber's level;
  working, it empties the chamber in seconds. The hardware interface can
  hold the return pump off (`allowReturnPump`), and the architecture
  check now names the suction pump's and the return pump's switches
  among those only the gateway may call (R1).
- The suction pump is the device's own (SD-109): it runs for a check and
  is off while the robot holds or waits. Flow is proven by the device's
  method: an outlet level sensor while it reports water and the pump
  runs, or a flow meter at 0.5 L/min, each for 10 s. Without flow within
  2 minutes the pump goes off and the check tries again in 5 minutes;
  after the third try in a row a dry-run lock holds it until it is
  released under Status. A probe or channel calibration, or a line fill,
  stops the check and the suction pump first, and the check starts anew
  once it ends.
- Filling a line drips into the tank (SD-111): where the outlet has a
  drop sensor, the pump runs in runs of at most 60 s until the first
  drops fall, at most 5 minutes in all, with the outlet on the device.
  The fill is written to the dosing journal before it starts, booked
  from the canister, logged with the doses, and the next check mixes it
  in first; a restart reports it like a cut dose. Without drops after
  5 minutes it ends with the reason. A channel without a drop sensor
  still primes into a cup for the time given. The simulator empties a
  line (`lineEmpty`; a line holds 6 ml, an assumption).
- After a dose the line is pulled back (SD-111): where the pump can run
  backwards, it pulls about 0.2 ml back (at most 1 s), so nothing dries
  at the tip. The channel's next run first fills the line the same time
  forwards, not booked, so the dose stays the planned amount; how long
  each line is pulled back survives a restart. In the twin, a
  calibration into a cup fills a pulled-back line first, too.
- The drop sensor confirms each run into the tank (SD-111): when no drop
  falls within the wait (four drops' time at the calibrated flow, at
  least 5 s), the run is cut at once and the channel locked with an
  alarm naming the likely causes; drops while a pump stands lock its
  channel too, as a canister above the device siphons, and the check
  mixes first. A locked channel doses nothing until it is released under
  Status; the check waits for it with the reason, and the channels page
  marks it. With drop sensors an empty canister is caught at its first
  run; two rounds without effect (RAT-055) still lock nutrients whose
  canister drips but does not work.
- Channels know their pump head (SD-108): a calibration belongs to the
  head it was measured with. When the device reports another head, or
  none, the calibration is void with an event, and the channel doses
  nothing until it is calibrated again; a channel without a head runs
  nothing. There is one pH− canister at most. The channels page shows
  each channel's head and, before a head is changed, that the canisters
  must stand no higher than the device. The simulator changes or takes
  off a head (`channelHead`), and a larger head moves more.
- The robot reacts to the hardware release (SD-110): a leak or high level
  in the measuring chamber switches everything off, raises an alarm and
  latches; the lock can be released in the app only once the device no
  longer reports its cause, and a restart never releases it. The stop
  button on the device is the Stop of the app. While the release holds,
  every run and the suction pump are refused with the reason, the check
  waits with it, and the watchdog lists it.
- The simulator panel switches the water path's faults (#10, step 3.5):
  a dry suction line, a blocked return or a failed return pump, a leak,
  the stop button, and per channel a blocked outlet or a siphoning
  canister. It shows whether the suction pump primes, the chamber's
  water, the outlet, the return, the hardware release with its reason,
  and each channel's drops.
- Scenario tests of the new water path (#10, step 3.4): the core against
  the twin through the API, for priming, no flow, doses and their drops,
  a blocked outlet, high level and the return pump
  (`tests/core/test_scenarios.cpp`, harness in `tests/core/harness.hpp`).
- The twin's outlets have drop sensors (SD-111): each channel counts its
  drops; a blocked outlet or an empty canister gives none while the pump
  runs; a canister above the device siphons and drips without a running
  pump; a pump that can runs backwards to pull its line back, and the
  next run fills the line before it drips. The hardware interface
  reports each channel's drops and whether its pump can run backwards;
  the state shows the drops per channel.
- The twin has the new water path (SD-109, SD-110): the suction pump
  primes before water flows; the measuring chamber keeps its water at
  the weir; the water returns by gravity or by a return pump; a blocked
  return or a failed return pump fills the chamber to high level, and
  a leak, high level or the stop button switch the hardware release off,
  which stops every pump below the robot. The state reports the release
  and the water path as `hardware`. A device without a flow meter
  reports the flow from the low end of its suction pump's delivery. New
  simulator actions: `returnFault`, `leak`, `stopButton`, and
  `flowFault` `dry`.
- The device reports what it has (SD-108): line, variant, its channels
  with pump type, head and drop sensor, the flow method, the return and
  the network, in `GET /api/v1/info` as `profile` and in the diagnostics
  package. The simulator has the example profiles `home` and `pro`
  (`--profile`); their numbers are assumptions.
- The mixing robot: one check as a state machine – pump on, prove flow,
  flush, measure until stable, EC before pH, dose in partial doses, mix in
  with flow, measure again and learn, hold; no dose without proven flow; a
  small test dose that checks the volume of a new tank (SD-105).
- The dosing journal: a dose is on record before its pump starts, and one
  cut by a restart is reported and mixed before the next measurement
  (SD-106).
- EC compensated to 25 °C; without a water temperature EC is no value for
  control (RAT-089).
- Web app: status, recipe with how long it applies, tank with volume, flow,
  turnover time and mixing waits, history of pH, EC, temperature and flow,
  calibration of the dosing pumps and probes, settings, and the Stop on
  every page.
- Simulator with a twin of tank, measuring chamber, flow, pumps and probes.
- History as CSV, from the history page or `GET /api/v1/export.csv`
  (`series`, `from`, `to`, `lang`; a week of all four curves by
  default): a row per time in UTC; in German with semicolons and a
  decimal comma, in English with commas and a decimal point; a gap stays
  an empty field (RAT-016).
- Maintenance mode on the tank page, for 30, 60 or 120 minutes (at most
  240 through `POST /api/v1/maintenance`): while someone works at tank or
  device the robot doses nothing, the pump is off, jumps of the probes
  count as the work and the targets are not judged; locks stay, and a
  restart ends it (RAT-042).
- Downloads of the simulator with the web app built in, for Linux, macOS
  and Windows: started without arguments it takes the next free port
  from 8080 and opens the browser. A tag on `main` releases them with
  checksums and a provenance attestation (`docs/RELEASE.md`).
- "Why it doses – or not" on the status page: a checklist of what
  nutrients and pH− each need now – recipe and tank volume, flow at the
  last check, readings, canisters on calibrated pumps, locks, the EC gate
  and the rest after a nutrient dose – each ticked or not, with the
  reason (RAT-037). The API has it as
  `cycle.checks`.
- "Report a problem" in the settings: the robot packs a diagnostics
  package – settings, recipes, calibrations, readings, states and the
  events of the last three days, with a report ID – that can be looked
  at and downloaded, and opens the bug form on GitHub filled in with the
  version, the report ID and what happened. Never in it: the password
  file's values, sessions, addresses, Wi-Fi details
  (`GET /api/v1/diagnostics`). The bug form asks for the report ID.
- `docs/INVARIANTS.md`: the domain rules by module (cycle, dose
  planning, gateway and canisters, sensor truth, restart, watchdog,
  history, simulator) and the code rules, with where they are kept.
  `tools/rationale.test.mjs` checks that every rule in force is placed
  there once with a fitting status, and that the files and tests the
  register names exist. RAT-043, RAT-046 and RAT-053 got tests of their
  own.
- A pair with a part missing is caught up first (SD-114, SD-120): when a
  nutrient round stops part way – a failed run, a Stop, a restart – the
  next nutrient round doses only what the pair misses, so the parts keep
  their ratio (RAT-054), mixes it in and measures, and learns nothing
  from it (RAT-057). A new tank, another recipe or "Don't catch up" on
  the status page drops it with a note; a missing part that cannot be
  dosed holds the other parts of its pair. The state has `catchUp`.
- The canister level, counted from use (SD-119): a size and the level
  now per canister, a "Refilled" button and "Enter level" on the pumps
  page. Every run of a canister's pump takes from it, doses and line
  fills from the run time, a calibration from the cup; an unknown level
  stays unknown and a count below zero is kept and reported. The
  watchdog warns below 150 ml of a nutrient or 20 ml of pH− (RAT-071);
  a dose the count cannot cover waits for a refill. The API:
  `capacityMl` and `stockMl` on `POST /api/v1/canisters`,
  `POST /api/v1/canisters/<id>/stock` (`{"ml": n}` or `{"full": true}`),
  and `stock` in the state. The demo starts with full canisters.
- Backup and restore in the settings: all settings as a file
  (`GET /api/v1/config/export`) and back (`POST /api/v1/config/import`).
  A file is checked like any change and its probe calibrations as if made
  now (RAT-025); a refusal lists every reason and changes nothing. A
  backup of the older schema is migrated. It never sets or clears the
  lock against a second first setup, is refused while a dose, a pump run
  or a probe calibration goes on, switches off what runs and starts the
  check again. What the robot learned stays only for the recipes and the
  pH− canister the file keeps (RAT-085); another tank volume makes the
  next dose a test dose.
- One simulator per data folder: with `--data` it holds `simulator.lock`
  in the folder while it runs, and a second start on the same folder ends
  with a message before it reads a file. A link under that name is not
  followed.

### Changed

- The circulation modes of the tank (`cycle`, `always`, `external`) are
  gone (SD-109): the device's own suction pump runs for every check. A
  setting `circulation` in a saved or imported configuration is read
  over; a circulation pump of your own comes later as a role on a smart
  plug. The tank page shows the flow and the turnover instead.
- Doseph's terms reach the API, the configuration and the app (SD-107):
  a dosing pump is a **channel** ("Channels & probes", `channel` on a
  canister, `channels` in the configuration and the state,
  `POST /api/v1/channels/…` to calibrate and prime, `channelCount` in
  the info), and the robot's own pump is the **suction pump** (the
  cycle's step `suction_on`, `suctionPump` in the state). Saved settings
  and backups of schema 2 are migrated to schema 3 when they are read;
  dosing journals written before keep their channel. Messages that named
  a pump number name the channel.
- The sensor truth calls pH, EC, temperature and flow signals, so
  "channel" names a dosing channel only (SD-107). The messages
  `truth.implausible` and `truth.jump` carry the signal as `signal`.
- Web app: `lucide-preact` 1.59.0, `vite` 8.3.4, `@playwright/test`
  1.64.1.
- `README.md` says what the prototype does and how to try it, step by
  step; `docs/CONCEPT.md` lists what is not built yet.
- Started from the software of a grow controller, without its history
  (`docs/PROVENANCE.md`); grow-only parts left behind.
- Doseph stands alone (SD-117): no references to the grow controller's
  issues, decisions, commits or repositories are left in code, tests or
  docs, and `tools/arch_check.sh` keeps it so. `docs/PROVENANCE.md` and
  the ID section of `CLAUDE.md` need nothing outside this repository.
- The repository is public, under AGPL-3.0-or-later with the additional
  permission (SD-116). New: `SUPPORT.md`, `CODE_OF_CONDUCT.md`, issue
  forms for bugs and feedback; `SECURITY.md` reports through GitHub's
  private vulnerability reporting.
- Two recipe templates name no manufacturer any more: "Three-part feed,
  vegetative wk 1–4" and "… flowering wk 1–2", with the same values
  (SD-116).
- Doseph's terms (SD-107): a bottle is now a canister, and what it holds
  is its role (nutrient or pH−); the flow cell is the measuring chamber.
  The API names them `canisters`, `canister` and `role`
  (`POST /api/v1/canisters`, `DELETE /api/v1/canisters/<id>`). A saved
  configuration of schema 1 is read and brought to schema 2.
- Every dropped rule in `docs/RATIONALE.md` keeps its substance and names
  the step of #10 that would bring it back; `docs/LATER.md` holds what
  later steps rebuild: grow phases and harvest, the setup assistant, the
  scenario harness and the dry-run latch.

### Security

- A password file the robot cannot read is kept as it is: no first
  password is offered or set over it, the sign-in page says so instead of
  showing a form, and an alarm is logged. Nobody is told to reset the
  robot, since a restart may read the file again (`GET /api/v1/info`
  has `passwordProblem`).
- Supply-chain checks in CI: `reuse lint`, with `reuse` installed only
  from hashed requirement files; the licenses of the web app's npm
  packages against an allow list; no npm package may have lost the
  provenance a lower version had (accepted cases in
  `tools/provenance-exceptions.json`); `npm audit signatures` right after
  every `npm ci`, which runs without install scripts, and a report of
  npm's reasons when it fails; every action pinned by commit SHA with its
  version (`tools/workflows.test.mjs`).
- A release carries two SBOMs (CycloneDX): the web app's runtime npm
  packages and the simulator's C++ libraries, covered by `SHA256SUMS`
  and the provenance attestation (`docs/RELEASE.md`).
