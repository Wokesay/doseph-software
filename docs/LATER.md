# Later

What later steps of the order of work (#10) rebuild, as the grow
controller this software started from had it, in our own words. It holds
what is not a rule from practice: those stay in `docs/RATIONALE.md`, where
a dropped rule names under **Later** the step that would bring it back.
The numbers here are the old starting points, not decisions; each step
decides its own as an SD (`docs/DECISIONS.md`).

## Grow phases and harvest (step 6)

**The grow run.** One run at a time: a name (at most 40 characters), its
start, a list of at least one phase, the current phase and when it began,
and the time of the harvest. Its states are none, running and completed;
a new run starts only once the last one is completed. The start, each
phase change, the harvest and the end are events, and a run groups the
history.

**Phases provide parameters, never names (RAT-076).**

- A phase is a free name (a label only), its planned days (0–999) and a
  set of parameters.
- A phase could set the pH target and tolerance, the EC target and
  tolerance, and the recipe that EC dosing uses. Planned for the later
  areas: light hours, dimming, day and night temperature, humidity or
  VPD, watering shots per day (RAT-009, RAT-010, RAT-083; step 9).
- The value in force is the built-in default, overridden by the user's
  setting, overridden by the current phase's own value while a run is
  running. A phase that sets no target follows the setting, which may
  change during the run; the app marks such a value "from the settings".
- The core reads only values in force; a check in CI refuses phase names
  in the core. A run is checked like any configuration change when it
  starts, so a phase value outside its hard limits is refused (R7).
- The robot's recipe in force with how long it applies (SD-105) is what it
  has today; step 6 decides how the two fit together.

**Moving on.** The user moved to the next phase ("Next phase", refused on
the last one). Required but not built: per phase, the user sets whether it
moves on by itself after its days or only once confirmed. At a change the
new targets apply at once; the start-up grace is the watchdog's only
(RAT-005). A lower EC target cannot be reached by dosing (RAT-012): a drop
of more than 0.2 mS/cm brings the advice to drain (RAT-065), and the robot
asks for water (SD-115).

**Harvest and the end (RAT-002, RAT-077).** Only the user records the
harvest, as an event of a running run; its time shows on the run's card.
Once the planned days are over, a daily reminder counts the days beyond
the plan. Completing is a step of its own; the card then names the
completed run and offers a new one. Kept with the run: water, nutrient
amounts and their cost, the phases' durations and a yield entered by hand,
which can be added later (RAT-064); its history exports with gaps left
empty (RAT-016).

**Starting a run.** The dialog proposed two phases, "Growth" of 21 days
with the first recipe and "Flowering" of 56 days with the second (else the
first); "Add phase" added one of 14 days. Each phase's targets came from
its recipe: EC from the recipe's chart EC at its amounts, pH from the
middle of its pH range (RAT-066). A value the user typed stays when the
recipe changes; one cleared means "use the setting". The dialog warns when
a phase's EC target lies below the EC floor of pH dosing, since pH would
then never be dosed (RAT-046).

**Recipe templates per phase.** A template has an ID, a neutral name in
English and German (SD-116), a note, its source and basis (an assumption
or a chart, RAT-066), the chart's EC at its amounts, a pH range, and its
parts in dosing order (RAT-080), each with a name, its pair and ml per
litre; a part may be a fixed addition (RAT-085). The robot's templates are
in `core/data/templates.json`. Applied to the canisters, a template maps
its parts by role, then by name, and lists what has no match; a pair name
already in use gets a free one, and the parts of a pair scale together
(RAT-054). Phase defaults: 18 h of light in the vegetative phase and 12 h
in flowering, whose week 1 begins with the switch to 12 h; for a recipe
without a pH range, pH 5.9 vegetative and 5.9–6.1 flowering (RAT-066).

**Not built.** A calendar and the cultivation system. The setup asked for
the kind of area (room, tent, greenhouse) and its name; the watering
defaults assume rock wool (RAT-010).

## The setup assistant (step 7, commissioning)

From the first start to the first mix, without an account. The device
detects what is there; the user does not pick it from a list.

| # | Step | Required to go on |
|---|---|---|
| – | First password, with the language choice above it; on the device it needs the setup code the device shows | always first; cannot be skipped |
| 1 | Basics: language, the device's name, time zone, kind and name of the area; on the device also its Wi-Fi | a time zone that exists |
| 2 | Devices: what was detected, "Add all" | the dosing pumps detected and added |
| 3 | Tank: name, usable volume, water (RO or tap), the outputs of a circulation pump and an inlet valve; a minimum level only with a level sensor | the usable volume |
| 4 | Nutrients: a template with a preview, or an own recipe; which canister is on which pump | at least one canister with contents |
| 5 | Calibrate: prime the lines, calibrate each pump, then the probes | nothing, but a pump not calibrated never doses |
| 6 | Done: what works now and what is missing; "Go to first mix" or the overview; what can be added later | – |

- **Time zone.** On the first run, while the device still has its factory
  zone, the browser's zone is proposed. A zone that does not exist is said
  at its field ("written like Europe/London"), and Next stays on the step.
- **Nutrients.** A template is laid on the pumps in order; the first pump
  after its parts becomes pH−, any further one "Additive n". A template
  with more parts than pumps warns. Each row has contents, a type
  (nutrient or pH−), a pair and ml per litre. The recipe is saved only when
  at least one nutrient has an amount, and one of the same name is updated,
  not doubled. The device cannot see what a canister holds, so coloured
  clips on pump and canister guard against mix-ups.
- **Calibration.** A pump runs 10–60 s (30 s proposed) into a measuring
  cup, with the real liquid, not water, which then goes back into the
  canister; the flow comes from the actual run time (RAT-070). Probes: pH
  with buffers 7 and 4, EC with 1.413 mS/cm. "Skip" is offered only while
  some pump is not calibrated, with the warning that it will not dose.
- **Done.** Only the last step marks the setup as done, as an event. It
  suggested a small first batch, measured by hand afterwards.
- **Throughout.** A bar at the bottom always shows Back, Skip where
  allowed, and Next; Next saves the step. Steps done can be opened again,
  later ones not yet. A side panel explains the step's terms and shows
  live what is still missing for mixing, from the same checks the status
  page uses.
- **Cancel.** Nothing is undone: each step keeps what it saved. Until the
  setup is done, every other page leads back to the assistant; afterwards
  it opens from the menu and offers "Open app". A pump calibration closed
  after its run failed is cancelled, so it does not stay the device's
  current job. A probe calibration closed without saving is cancelled on
  the device: the probe keeps its old calibration, and its jumps count
  again (RAT-042). One the device ended, by a restart, says so and offers
  to start again.
- **Decided for the robot.** A step shows where canisters must stand and
  asks for its confirmation; until then no line is filled and nothing
  doses, and a backup never brings the confirmation (SD-118). Filling the
  lines drips into the tank and is the first dose (SD-111). A calibration
  stops the check and the feed pump first (SD-109). Channels, heads and
  the flow method come from the device's profile, and at most one canister
  holds pH− (SD-108).

## The scenario harness (step 3)

Rebuilt in step 3.4 as `tests/core/harness.hpp`, with the scenarios of the
new water path in `tests/core/test_scenarios.cpp`. What the grow
controller's harness had beyond that – plugs, clock steps, a controller
that restarts alone – comes back with the steps that need it.

Scenario tests run the real core against the twin in simulated time and
drive it only through the API, as the web app does. They test sequences
end to end; unit tests test the parts.

**Driving the simulator.**

- Each test builds its own simulation: a scenario (a new device that asks
  for its first password, or the demo, set up and filled), no history
  computed ahead, a fixed start time, the demo password, everything in
  memory.
- A test client sends method, path, query and a JSON body to the request
  handler the HTTP server uses, without a socket, and keeps the session
  from the cookie the answer sets. One call fails the test on any status
  but 200, naming method, path, status and body; another returns status
  and body for a refusal the test expects.
- Time moves only when the test moves it: a step of so many milliseconds,
  with the core ticking at most once a second (coarser for long tests),
  or whole hours. A wait-until helper polls a condition in simulated time
  and fails the test when its deadline passes. Hours run in seconds, the
  same every time.
- Setup goes through the API as a user's would: first password, devices,
  canisters, a recipe, the tank. A calibration helper starts the run,
  waits until it asks for the cup's amount, reads from the twin what is
  really in the cup (the true flow × the actual run time) and enters it,
  or a share of it for a wrong entry.
- The world and its faults through the twin: the tank filled with a
  volume, EC and pH; a pump blocked or run empty; a probe frozen or
  offline; plugs that go offline, ignore commands, stick or crash; a power
  cut (how long, whether time is secured afterwards, or the controller
  alone restarting); clock steps; a buffer at the probe. After a power cut
  a new client signs in again.
- The same actions answer over HTTP at `POST /api/v1/sim/<action>` behind
  the login; the scenario changes without it only with `--allow-reset`.
  Tool tests start the built server on a free port; browser tests go
  through the web app.
- A harness over HTTP also needs simulated time moved on request. The old
  simulator could only jump whole hours (1–72 h); its scenario tests
  stepped time in the test's own process.

**What they assert.**

- The API's answer: its status, and the error's key and arguments; the
  translated text only where the text is the point.
- Events of one type: their title and text keys, arguments and data, such
  as ml per canister.
- The state: the job and the last job, latches and their cause, outputs,
  each function's line (key and arguments), whether a reading is usable.
- The twin's truth, which the core cannot see: that no pump ran, the
  tank's true EC including what is not mixed in yet, a pump's true flow
  against its calibration.
- Numbers within a tolerance: amounts within 3 % of the plan, a pair 1:1
  within 3 %, EC as the start effects expect.
- After a restart: everything off, nothing resumed, the cut reported (R6,
  RAT-007).
- A test's name names the rules it covers, as the register's tests do.

## The dry-run latch of the circulation pump (step 4)

How the grow controller kept its circulation pump from running dry
(RAT-047, RAT-051, RAT-062, RAT-063), next to what SD-109 decides for the
feed pump.

**What latched it.**

- Below the tank's minimum level the pump may not start. That lock lifts
  by itself 0.5 L above the minimum and is no alarm.
- The latch is set only when the protection had to cut a pump that was
  running: a usable level fell below the minimum while it ran. Its
  reason, time and cause ("level") are frozen when it is set, and an alarm
  event says whether it will release itself and from which level.
- The gateway enforces it on every path: a pump found running while
  latched, switched by hand at the device or by a relay that did not hold,
  is cut again. Every other function reads the lock and does not ask for
  the pump; dosing runs only with the pump running and stops when it is
  cut.
- The latch is saved the moment it is set or released and survives a
  restart; one restored without a cause, or with an unknown one, holds as
  the stricter kind.
- Planned, not built: a dry run seen in the pump's power draw (RAT-049)
  would latch and hold for a person.

**How a person released it.**

- The latch stands as a line on the tank page with a "Release" button.
  Releasing clears the latch even while the cause lasts; the switch-on lock
  still keeps the pump off until the level is back. The release is an
  event naming the latch and took effect within about 50 ms. Releasing is
  always possible.
- While latched, the pump's control shows "blocked" with the latch as its
  reason. The texts say which kind it is: one that releases itself names
  the level and asks for maintenance mode when filling by hand; one that
  holds says why and what to check; without a level sensor, to tap
  "Release".
- A latch caused only by a low level released itself once a valid level
  stayed at or above the minimum plus the larger of 0.5 L and 2 % of the
  capacity, and not above the capacity, for 5 min with the pump read as
  off. Any other reading or a restart started the 5 min again; maintenance
  mode held it until 5 min after its end; during a Stop it could release,
  but the pump ran only after Resume. It held for a person when switching
  off was not confirmed or the pump ran again after it was read as off,
  when the level was low again within 60 min of the last self-release (a
  time kept across restarts and clock steps), when the level jumped while
  latched with nothing to explain it, and when its cause was missing or
  unknown. A release level above the capacity never released, and said
  so. Accepted risk: a level sensor that wrongly reads a refilled tank can
  restart a pump that then runs dry until the level reads low again.

**Next to SD-109.** The feed pump has no level sensor: the flow proof
takes its place, and after three failed tries in a row the robot latches
"no flow" for a person to release in the app. What carries over: a latch
only after the robot's own attempt failed, its reason frozen; the gateway
keeps the pump off on every path; no dose without proven flow (SD-105);
the latch survives a restart, and an unknown one holds (RAT-063); a person
releases it once the cause is fixed (SD-110). A release starts nothing at
that moment: the grow controller left the pump to its control, and for
the robot that is the next check. Self-release and the switch-on lock by
level come back only with a level sensor or float switch (step 9), and
then only for a latch whose cause was a low level, never for one of the
flow proof.
