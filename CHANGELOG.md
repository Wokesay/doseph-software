# Changelog

All notable changes to this software. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Changed

- The device's pump lies in the tank (SD-127, from PD-476 and PD-477). It
  is now called the feed pump, and the device profile says where it sits.
  - In the tank it has no priming.
  - No flow for 6 s, at its start or later, switches it off before the
    hardware's own cut.
  - The second try in a row without flow locks it, since each try can
    spill water.
  - When the hardware cuts it for no flow, the check says so and counts a
    try.
  - Maintenance cleans the pump and its inlet screen instead of a suction
    filter.
  - The simulator's flow faults add "hose off".

### Added

- Access tokens with three roles (SD-129), for the app, the account and a
  coupled device. They are made in the settings with a name and a role,
  shown once, kept as a hash and revoked one by one.
  - An app token may do everything the owner may, except change the
    password and manage tokens.
  - An account token may read, stop, acknowledge alarms, and change
    recipes, the recipe in force and plants.
  - A peer token may read.
  - A call outside the role is refused with 403. Creating and revoking a
    token are events. Tokens survive a password change.
- An OpenAPI file of the API, `docs/openapi.json` (SD-129). It covers
  every route and the roles that may call it, and tests keep both in
  step with the device.
- Commissioning (SD-129): the steps of #10 on a page of their own, kept
  by the robot so they resume after a restart.
  - The robot ticks off what it sees: password, flow, probes, channels,
    lines filled, and tank and recipe.
  - The user confirms the placement and the test run, and may skip push.
  - Until the placement is confirmed, nothing runs towards the tank
    (SD-118), and the status says so. A backup never brings a
    confirmation.
  - Until all are done, every page shows the next step. The demo counts
    as set up.
- The device has a random ID of its own (SD-129). It is shown in `/info`,
  in the diagnostics and on the settings page, and never in a backup.

- The cultivation system per tank: hydro, coco, rock wool or soil
  (SD-128). It sets the pH range a new phase is offered when its recipe has
  none, and which templates come first; control never reads it.
- A plants page (SD-128). It shows which plant sets the water, each plant
  with its day, phase and what is due (next phase, harvest, complete, set
  the water), a start dialog, and an editor for plant recipes. A calendar
  lists the next 8 weeks: phase changes, planned ends and the maintenance
  dates (`GET /api/v1/calendar`). The status and recipe pages name the
  plant and phase that set the water.
- Plants and their phases set the water (SD-128, #10 step 6).
  - A plant recipe holds phases, each with a name, its days, whether it
    moves on by itself or once confirmed, and a recipe with its own EC
    target and pH range where they differ.
  - A plant started from it copies the phases.
  - The leading plant's phase sets the recipe in force; without a running
    plant the recipe chosen by hand applies.
  - Phases move on at the local midnight after their days. One waiting
    for confirmation, and the last one until the harvest is recorded,
    remind daily in the morning report.
  - Harvest and completing are the user's.
  - API: `GET` and `POST /api/v1/plants`, `PUT` and `DELETE
    /api/v1/plants/<id>`, `POST /api/v1/plants/<id>/next|harvest|complete|lead`,
    `PUT /api/v1/plant-recipes`, `DELETE /api/v1/plant-recipes/<id>`.

- pH and EC are compared with the feed pump running and standing at the
  end of each check (SD-127, from PD-476). A damaged cable in the water
  can shift them. A difference beyond 0.1 pH or 0.05 mS/cm is checked
  once more at once. Confirmed, it locks the pump and dosing with a
  critical alarm, "check the pump and its cable", until it is released.
  The simulator panel has a damaged cable as a fault.
- A dry run of the feed pump is seen in its current (SD-127, RAT-049).
  The device learns the current while the pump moves water. Once the pump
  has moved water in a check, less than 60 % of it for 2 s switches the
  pump off at once, as a try without flow. The water page shows the
  current and its usual value.
- The history keeps five years at one hour (SD-126, #10 step 5): a
  fourth level with min, mean and max per hour after 10 s for a day,
  1 min for a week and 15 min for a year. The history page offers 1 and 5
  years; the CSV export reaches as far. A saved history from before is
  read, and its hourly level is worked out from the last year's 15-min
  values.
- Maintenance dates (SD-125, #10 step 5). Each item comes from what the
  device has, with an interval that can be set from 7 to 730 days:
  - calibrate the pH probe (30 days) and the EC probe (90);
  - calibrate each channel (90);
  - clean the measuring chamber, the feed pump and its inlet screen (30);
  - check the pump heads (180);
  - replace the pH probe (365).

  What falls due within a week goes onto one date. On that date it comes
  as a note in the morning report and stands on the tank page until it is
  done. A calibration resets its item by itself; everything else has
  "Done". API: `GET` and `PUT /api/v1/service`,
  `POST /api/v1/service/<id>/done`.
- Push without an account (SD-124, #10 step 5): alarms and the morning
  report go out through ntfy, Telegram or e-mail.
  - The channels are set up in the settings, each with "On" and a test
    message, and with how it fared last. A channel that stops getting
    through is logged once.
  - Tokens and passwords stay in a file of their own: they are not in the
    backup, the diagnostics or `GET /config`, and are never shown again.
    No field may hold a line break.
  - Pushes come in the device's language: the core now has the web app's
    German texts, copied by `tools/gen_msg_de.mjs` and checked against them.
  - The simulator sends through the computer's curl, with the secret in a
    private config file that is deleted after the run.
  - API: `GET` and `PUT /api/v1/push`, `POST /api/v1/push/test`.
- Alarms with three levels, quiet hours and a morning report (SD-123,
  RAT-022, #10 step 5): every watchdog problem becomes an alarm, a latch
  at once and anything else once it has stood 5 min; a target out of
  range only while the check holds or waits.
  - Critical alarms (leak, high level, a canister dripping on its own,
    dry run, return pump held off, pH out of range) are pushed at once,
    at night too, and repeated after 5, 15 and 60 min.
  - Warnings are pushed once by day; at night they wait for the morning
    report when the quiet hours (22:00–07:00, set in the app) end.
  - Notes come only in the morning report.
  - Acknowledging pauses the reminders for 12 h at most; the alarm stays.
  - The status page lists the alarms with their level and "Acknowledge";
    the settings set the quiet hours and list what was notified.
  - API: `state.alarms`, `POST /api/v1/alarms/<id>/ack` (or
    `/alarms/ack` for all), `GET /api/v1/notifications`,
    `PUT /api/v1/notify`. Delivery through ntfy, Telegram or e-mail
    follows.
- Local time in the device's own time zone (#10, step 5): the core works
  out the offset from UTC from the rules of the IANA time zone database,
  embedded as a table (`core/src/tzdb.inc`, made by `tools/gen_tzdb.mjs`),
  the same in the simulator and later on the device. Quiet hours, the
  morning report and maintenance dates build on it.
- Step 4's acceptance in the twin (#10, step 4.9): from tap water to the
  demo recipe and held there for 12 h at 50 and 100 L (Home example) and
  100 L (Pro example): pH within ± 0.2, EC within ± 0.1, an overshoot of
  at most 0.2 pH, and every dose seen by its drop sensor. 5 L waits for
  the product question on the smallest tank (#48).
- A water path page (#10, step 4.8): how the device proves flow (flow
  meter or the outlet's level sensor), whether the suction pump runs and
  the flow is proven, the measured flow and the turnover; the measuring
  chamber with its readings, the outlet, the high level and the probe
  calibration (moved here from the channels page); the return by gravity
  or pump, and the hardware release with its reason. The dry-run and
  return locks show here with a pointer to Status. The channels page
  shows each channel's drop sensor (last drop, or none fitted) and
  whether it pulls back after a dose.
- Large tanks wait for their learned settling (SD-112): above 150 L the
  flow through the device is only a sample stream and the customer's pump
  mixes, so the robot's own wait is not a turnover but the settling time
  it learns from its doses. A clean dose's settling time is read from the
  history: from the end of its dosing until the readings stay within the
  stability band of their final value. The wait is 1.5 × the longest of
  the last three, 5 to 600 minutes, and 60 minutes until one is learned;
  the user may still lengthen it. The tank page says which applies.
- "Top up water" (SD-115): when EC is above its band at a check and
  nothing was dosed since a check found it within, water is missing. The
  robot raises "top up water" once, as an event, a watchdog item and a
  note of the check; nutrients wait anyway (RAT-012), pH is still
  corrected. It goes when EC is back within the band. pH− counts as a
  dose here, since it adds a little EC; a new tank or another recipe or
  target starts afresh. A refill valve comes back only with a float
  switch.
- A daily amount per channel that cannot be switched off (SD-113): a
  channel's run time into the tank, doses and line fills, per rolling
  24 hours stays within its head's value from the device's profile and
  never passes 60 minutes in code. A run that would pass it is refused
  with the reason, and the check waits for it instead of trying. A user
  may set ml per day per canister below it. The count survives a restart.
  The channels page shows each channel's run time in the last 24 hours
  against its cap; the example profiles give 40 min (S) and 20 min (M),
  assumptions.
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

- The reserve an EC round keeps for the pH− dose that follows (RAT-053,
  SD-121): until the pH− effect is learned it is planned at half the
  start effect, without the 0.3 mS/cm cap, and never below the EC floor
  plus its tolerance; once pH is in range, a last round tops EC up to its
  target. Before, a device's first tank of tap water ended about 0.25
  mS/cm above the EC target, which only fresh water can take back.
- A nutrient dose too small for a pump (SD-122): a test dose is made as
  large as the smallest round every part can run, in the recipe's ratio,
  within the round's cap and never past the aim; any other round under
  1 s for a part is not dosed, and the check notes it and goes on to pH.
  Before, the check waited for it without end.
- The mixing wait (SD-112, answers #1): after a dose the robot waits one
  turnover, at least 2 minutes, after nutrients and after pH− alike,
  then measures until the readings hold within their band over 60 s or
  a fifth of a turnover, at most 10 minutes. The tank's two wait fields
  become one value, "at least N min after a dose" (0 to 600), which
  lengthens the wait and never shortens it; a saved configuration keeps
  the longer of its two waits as that value. The tank page shows the
  wait in force and why.
- The circulation modes of the tank (`cycle`, `always`, `external`) are
  gone (SD-109): the device's own suction pump runs for every check. A
  setting `circulation` in a saved or imported configuration is read
  over; a circulation pump of your own comes later as a role on a smart
  plug. The water path page shows the flow and the turnover instead.
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
