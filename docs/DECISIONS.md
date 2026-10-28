# Software decisions (SD)

Binding decisions for the software. Format `## SD-XXX: Title`, sequential,
chronological, two blank lines between entries. No status field; entries
are not deleted, a later SD replaces an earlier one and refers to it.

Principles, costs and hardware are decided by the product session; a
product question is filed as an issue with the label `product-question`.
Control details (locks, releases, thresholds, sequences, waits) are decided
here by the minimal principle: affordable, simple, a safety frame instead
of guarding every scenario; a crash or restart switches off everything
that can do harm.

**Numbering.** This log starts at SD-105; lower numbers are not used.


## SD-105: The mixing robot: one check as a state machine, flow first, a test dose for a new tank

2026-10-24. The product is no longer a grow controller but a mixing robot
at the nutrient tank (decided by the product session). This entry decides
the control flow; the values are starting points, not fixed.

**The check.** One state machine (`core/src/cycle.cpp`):

1. **Pump on** – with circulation `cycle` the robot switches the
   circulation pump on; with `always` it runs anyway; with `external` the
   user's own pump has to deliver.
2. **Prove flow** – at least 0.5 L/min without a break for 10 s. No flow
   within 2 min: the pump goes off, the check waits 5 min and tries again,
   and the watchdog reports it.
3. **Flush** – 60 s of flow, so the measuring cell holds tank water.
4. **Measure until stable** – pH within 0.05 and EC within 0.03 for 60 s;
   not stable within 15 min: wait and try again.
5. **Evaluate EC before pH** (RAT-004). EC rounds aim below the target by
   the reserve for the pH− that follows (RAT-053); pH waits below the EC
   floor and 240 s after a nutrient dose (RAT-046).
6. **Dose in partial doses** (RAT-054, RAT-055, RAT-050).
7. **Mix in** – the user's mixing wait; the clock runs only while water
   flows. 30 min without flow: wait.
8. **Re-measure and learn** the effect from clean rounds (RAT-057); two
   rounds without effect latch (RAT-055, RAT-020).
9. **Hold** – on target, the circulation pump off (with `cycle`), the next
   check after the pause between checks (default 60 min).

**Never dose without proven flow.** The gateway refuses a run into the
tank without flow (`act.no_flow`) and stops a running one at once when the
flow stops. A calibration or priming into a cup needs no flow.

**The mixing wait** after a dose is set by the user per tank, for EC and
for pH. A value from tank volume and measured flow is only a suggestion:
about 2 turnovers after nutrients, 3 after pH−, in whole minutes, between 2
and 600 min. Without a value of the user's, the suggestion applies. Whether
the suggestion acts as a lower bound is open (product question #1); until
it is decided it does not, and the web app says when a wait is shorter.

**A new tank** – the first start, a changed volume or "New tank" – gets a
small test dose first: EC rounds aim at a rise of 0.1 mS/cm at the largest
effect the recipe may have; a pH− test dose at a drop of 0.1. If the rise
is more than twice or less than a third of what the entered volume
expects, dosing stops (latch `tank.volume`) until the user corrects the
volume or confirms it. A test dose that passed is learned; one that did not
is not. For pH only the "much stronger" side is judged, since pH− acts on
a buffer the robot does not know.

**The recipe in force** is chosen with how long it applies: a number of
days (1–366), until a date (at most a year ahead) or without an end. Once it
has ended, the robot doses nothing and says so.

**Out of scope:** climate, light, watering, tank refill and drain; mixing in
a bucket or watering can; stock of bottles; notifications and a store app
(later).


## SD-106: A dose is written to the dosing journal before its pump starts

2026-10-24. Every run that goes into the tank is recorded in
`journal.json` (written atomically) before the gateway starts the pump,
and its end afterwards. A run whose record cannot be written does not
start. After a restart an open record is reported once as cut ("up to
x ml may be in the tank – how much is unknown"), and the first check mixes
the tank before it measures and learns nothing from it (R6, RAT-028). A
damaged journal counts as "a dose may be in", never as nothing (R5).
Calibration and priming runs go into a cup and are only recorded. The
journal is cleared when the tank is on target.


## SD-107: Doseph's terms

2026-10-27. The product brief (#9, #10) names the parts of the device; the
software uses the same words, in code, API, texts and docs. An older
term that names something else is replaced when its module is next
touched; the import (#10, step 2) renames canister and measuring
chamber, the twin (#10, step 3) renames channel and suction pump.

| Term | Meaning | Replaces |
|---|---|---|
| channel | one dosing path: pump with head, line, outlet with drop sensor, the canister on it; numbered only for the user | dosing pump (`pump` 1..n) |
| role | what a channel's canister holds: `ph_down`, `nutrient`; logic reads roles, never channel numbers (SD-108) | bottle `kind` |
| canister | the liquid on a channel, with its name, colour, pair and level | bottle |
| head | the pump head of a channel: size `S`, `M`, `L` or none | – |
| suction pump | the device's own self-priming pump that draws tank water through it | circulation pump |
| measuring chamber | the open chamber with pH, EC and temperature probes; a weir keeps them wet | flow cell, cell |
| mixing chamber | where the outlets drip through an air gap, downstream of the probes | – |
| return | the way back to the tank: by gravity, or by a return pump | – |
| hardware release | the switch-off below the firmware: stop, leak, high level, a hung controller; no flow locks dosing (SD-110) | – |
| check | one run of the mixing cycle (SD-105) | – |

`Channel` in the sensor truth (pH, EC, temperature, flow) becomes
`Signal`, so "channel" means a dosing channel only.


## SD-108: The device reports what it has; logic reads roles and capabilities

2026-10-27. One firmware for Home and Pro (#9, PD-447, PD-450). At start
the hardware layer reports a profile: line (`home`, `pro`), variant name
(e.g. "Pro 30"), and capabilities: the channels with pump type
(`brushed`, `brushless`, `stepper`) and head size, whether a channel has a
drop sensor, the flow method (`turbine`, `outlet_level`), the return
(`gravity`, `pump`), the network interfaces. Coupled peers follow with
#10 step 8.

- **Logic reads roles and capabilities**, never a channel number or a
  line name. Which code path runs follows from a capability ("has a
  return pump"), never from "is a Pro".
- **Roles** are set by the user per canister: one `ph_down` at most, the
  rest `nutrient`. pH− never in a recipe (RAT-004) stays.
- **A head that changes** (fitted, removed, another size) makes the
  channel's calibration invalid: it doses nothing until it is calibrated
  again (PD-407). A channel without a head doses nothing. Before a head
  is changed the app says that canisters stand no higher than the device
  (PD-455).
- **Board profiles are hardware data** and stay private, outside this
  repository. Numbers the control needs from a profile (flow of the
  suction pump over speed, the daily cap per head, SD-113) come through
  the profile; the code holds a safe ceiling of its own. The simulator
  ships example profiles "Home" and "Pro" whose numbers are assumptions,
  marked as such.


## SD-109: The water path: suction pump, flow proof by method, calibration with everything stopped

2026-10-27. The water path of #9 (PD-444, PD-456) replaces the circulation
pump of SD-105. The steps of the check stay.

- **Suction pump.** The device's own pump runs for a check and is off in
  Hold and Wait. The modes `cycle`, `always` and `external` of SD-105 go
  away; a customer's circulation pump becomes an optional role on a
  smart plug later (#10 step 9, PD-421).
- **Flow proof by method.** `turbine`: at least 0.5 L/min for 10 s without
  a break (SD-105). `outlet_level`: the outlet's level sensor reports
  water and the suction pump runs, for 10 s without a break. The hardware
  locks dosing without flow on its own; the core proves flow as well and
  never doses without it (SD-105).
- **Flow for the turnover.** `turbine`: the flow measured while proven.
  `outlet_level`: estimated from the suction pump's speed with the
  profile's curve, taken at its low end, so a turnover errs long and the
  mixing wait with it (SD-112).
- **Priming.** No flow within 2 min: the pump goes off, the check waits
  5 min and tries again (SD-105). After 3 failed tries in a row the
  robot latches "no flow" (the pump may run dry: suction line out of the
  water, tank empty, filter blocked); a person releases it in the app
  (the dry-run pattern of RAT-047 and RAT-051).
- **Return.** By gravity there is nothing to switch. A return pump runs
  while the chamber reports water to return; a return pump that runs for
  60 s without the level falling is off and latched ("return pump dry or
  blocked"). High level in the chamber is the hardware's (SD-110).
- **Calibration.** A probe or channel calibration stops the check and the
  suction pump first (#10). A channel is calibrated with its outlet
  taken off and held over a measuring cup; a probe is lifted out and put
  in buffer cups. The check starts anew afterwards, and the probes'
  readings are announced as changing (RAT-042).


## SD-110: The hardware release and the lock reasons

2026-10-27. Below the firmware, leak, high level, the stop button and a
hung controller switch off, and no flow locks dosing (#10). The
core reads the release state and never overrides it.

- **The gateway refuses** a run while the release is off, and says which
  input holds it. The cycle shows it as its line; the watchdog lists it.
- **Leak**: everything off, latched "leak" until a person releases it.
- **High level** in the chamber: the hardware stops the suction pump and
  dosing; the core latches "high level (return blocked or return pump
  failed)" until a person releases it.
- **Stop button**: the same as Stop in the app (RAT-036); it survives a
  restart, and Resume is in the app.
- **No flow**: as SD-105 and SD-109.
- **Per channel**: outlet blocked and drops without a running pump
  (SD-111), no head, not calibrated. A channel lock holds only that
  channel; a lock on the `ph_down` channel holds pH dosing, one on a
  nutrient channel holds every recipe that uses it.
- **Release off without a known input**: no dose, shown as "hardware
  release off".
- **Releases**: a person releases a latch in the app once the cause is
  fixed (#10). A latch survives a restart (RAT-063); a restart
  never releases one.


## SD-111: The drop sensor confirms each run; the outlet is pulled back after a dose

2026-10-27. Each outlet has a drop sensor (PD-445). The numbers below are
starting points until bench tests give better ones.

- **A run needs drops.** The expected time per drop is a drop of about
  0.05 ml at the channel's calibrated flow. If no drop falls within four
  times that, at least 5 s, from the start of a run, or a gap of the same
  length opens while it runs, the gateway stops the run at once and
  latches the channel: "no drops: outlet or line blocked, canister empty,
  tube kinked or pump head failed". A run shorter than that is judged at
  its end plus the same time. What ran is booked from the run time
  (RAT-070) and the check learns nothing from that round (RAT-057).
- **Drops without a running pump** (a canister above the device siphons,
  PD-455), outside 10 s after a run of that channel: an alarm with the
  channel, the channel latched, and the next check mixes first and learns
  nothing, as after a cut dose (SD-106).
- **Pull back after a dose.** After the last run of a dose the pump runs
  backwards briefly, about 0.2 ml and at most 1 s (pump types that can),
  so nothing dries at the tip. The next run of that channel first runs
  the same time forwards, not booked as a dose, so the dose itself stays
  the planned amount.
- **Filling a line** runs in runs of at most 60 s until the first drops
  fall, at most 5 min in all, and stops. It drips into the tank, so it is
  a dose: written to the journal before it starts (SD-106) and mixed and
  measured by the next check. The first fill of the lines counts as the
  first dose (PD-425). A channel calibration still runs into a cup.
- **The acceptance target** of the twin and tests: the drop sensor sees
  every dose on every channel (#10).


## SD-112: The mixing wait: one turnover, then stable; the user may only lengthen it

2026-10-27. Answers #1 (PD-433) and replaces the mixing wait of SD-105 and
the user's two waits per tank.

- **At least one turnover** after a dose: volume ÷ flow through the
  device (SD-109), at least 2 min. The mixing clock runs only with flow
  (SD-105).
- **Then stable.** The re-measurement after the wait is the existing one:
  pH within 0.05 and EC within 0.03, now over a window of 60 s or a fifth
  of a turnover, whichever is longer, at most 10 min. Not stable within
  the window plus 15 min: wait and try again (SD-105).
- **The user may lengthen it**, never shorten it: one value per tank, "at
  least N min after a dose", 0 to 600 min. It applies when it is longer
  than the robot's own wait.
- **pH waits like EC.** The fixed 2 and 3 turnovers go away; pH keeps its
  rest of 240 s after an EC dose (RAT-046), and stability decides the
  rest (RAT-052, RAT-058).
- **Large tanks** (more than 150 L, where the flow through the device is
  only a sample stream and the customer's pump mixes, PD-450): the minimum
  is the settling time learned from earlier doses instead of the
  turnover. A dose's settling time is the time from its end until the
  readings stay within the band of their final value; the robot reads it
  from its own history after the dose. Learned: 1.5 × the longest of the
  last three clean doses (RAT-057), 5 to 600 min. Before anything is
  learned: 60 min. The first dose of a new tank is small (the test dose,
  SD-105).
- The web app drops the two wait fields; it shows the wait in force and
  why (turnover, learned settling, or the user's).


## SD-113: Caps that cannot be switched off; users set values below them

2026-10-27. The firmware's caps are in code and the gateway applies them
to every run (#10; R7). Settings only tighten them.

- **One channel at a time** (RAT-018), also across coupled devices later
  (#10 step 8).
- **Run time per run**: at most 60 s, at most 6 partial runs per canister
  and round (RAT-055); a run under 1 s is not made (RAT-050). The
  smallest dose comes in short pulses with the smallest head (#10); a
  pulse shorter than 1 s needs bench data first.
- **Daily amount per channel**: run time in a rolling 24 h, at most the
  profile's value for the head (SD-108), never more than 60 min in code.
  A run that would pass it is refused with the reason; the user may set
  ml per day below it. The count survives a restart (RAT-028).
- **Releases**: locks are released by a person in the app, and only the
  app sets limits and releases; MQTT does not (PD-461).


## SD-114: A pair with a part missing is caught up first

2026-10-27. Catching up a pair after an error, now in the cycle. A round
of a pair can stop with some parts in and others not (a failed run, no
drops, a Stop, a restart).

- The robot keeps the missing amount of each part, with the recipe, in
  the run-time state (it survives a restart, RAT-028).
- The next EC round of that recipe doses only the missing parts, mixes
  them in and measures; then the cycle goes on as usual. It learns
  nothing from that round (RAT-057).
- The catch-up is dropped, with a note naming the parts and amounts, when
  the recipe changed its parts, a new tank starts, or a person ends it in
  the app. While a missing part's channel is locked the robot doses no
  other part of that pair and says so.


## SD-115: EC rising without a dose means water is missing

2026-10-27. Refill comes back only with a float switch (PD-460); without
one, the robot only tells. When EC is above its target band at a check
and the robot dosed no nutrients since the last check that found it
within the band, the robot raises "top up water" (also as a push, #10
step 5). It doses no nutrients while EC is above the band anyway
(RAT-012); pH dosing goes on. The message goes when EC is back within the
band.


## SD-116: Public under AGPL-3.0-or-later; a prototype every week

2026-10-27. Decided by the owner.

- **License and visibility.** The repository is public. The software is
  licensed under AGPL-3.0-or-later with the additional permission in
  `ADDITIONAL_PERMISSION.md`, as its files already say.
- **Contributions.** Bug reports, feedback and ideas come as issues, with
  the forms in `.github/ISSUE_TEMPLATE/`. Code comes only from the
  project's sessions and its owner; pull requests from outside are not
  merged, so the project keeps one copyright holder.
- **Prototypes.** A prototype is the simulator with the web app, as a
  download for Linux, macOS and Windows, built from a tag on `main`. It
  is for trying the software, not for dosing into a real tank. One comes
  out weekly while something new is merged, numbered `0.1.0-proto.N`
  from `0.1.0-proto.5` on (the predecessor's prototypes ended at 4).
  The notes come from `CHANGELOG.md`. Security fixes go into the next
  prototype only.
- **Reports.** Until "Report a problem" is in the app (#10, step 2.6),
  the issue forms are the way to report.
- **Recipe templates** name no manufacturer until the product side has
  checked brand rights (#10); the values of a manufacturer's chart stay,
  with a neutral name.


## SD-117: Doseph stands alone; the predecessor is deleted

2026-10-27. Decided by the owner.

- **No advisories.** The predecessor's software runs on no device and
  nobody downloaded it, so advisories would protect nobody.
- **The predecessor's repositories** are private and archived. The owner
  deletes them once everything Doseph needs is in this repository. No
  backup is kept.
- **No references to the predecessor.** Code, rules and their reasons
  live here. No `gc#n`, no `gc PD-`, `gc SD-` or `gc RAT-` IDs, no commit
  hashes, no links. Where a reference carries a reason, the reason is
  written here (this log, `docs/RATIONALE.md`, or the comment itself);
  otherwise the reference is dropped. RAT and SD IDs keep their numbers.
  New work adds no such reference.
- **Last import step:** the remaining references go, `docs/PROVENANCE.md`
  and the ID section of `CLAUDE.md` are rewritten to need nothing outside
  this repository, and `tools/arch_check.sh` checks that none comes back.
  Then the predecessor can be deleted.
- **Copyright.** `REUSE.toml` names "The doseph-software contributors".
  The predecessor's contributors are the same, so the copyright holder
  does not change.


## SD-118: Canisters stand below the device, confirmed at commissioning and before a head change

2026-10-27. Product requirement (from PD-463). It replaces "canisters
stand no higher than the device" in SD-108 and the reason given for
drops without a running pump in SD-111.

- **The rule.** The top of every canister stands lower than the bottom of
  the device. Then no canister can siphon through a stopped pump into the
  tank.
- **Confirmed at commissioning.** The setup assistant (#10, later step)
  has a step that shows the rule and asks for its confirmation; until it
  is confirmed no line is filled and nothing doses. The robot keeps the
  confirmation with its time in the configuration and logs it as an
  event. A backup brings everything but this: placement is physical, so a
  restored or new device asks again, as it never takes `passwordSet`
  from a file.
- **Confirmed before every head change.** The app shows the rule and asks
  again before it lets a head be fitted, removed or changed; without it
  the head change does not start. The head's channel doses nothing until
  it is calibrated again, as before (SD-108).
- **The safety net stays.** Drops without a running pump (SD-111) still
  raise an alarm and latch the channel; its text now names the likely
  cause: a canister stands higher than the bottom of the device.
- The confirmation is a statement of the owner, not a measurement: the
  robot cannot see where a canister stands. It is kept simple on purpose
  (minimal principle); the drop sensor is what catches a wrong one.


## SD-119: The canister level is counted from use

2026-10-27. Step 2.7 of #10. The robot cannot see into a canister; it
counts.

- **Size and level.** A canister may have a size (10 ml to 25 L). Its
  level is unknown until the owner enters it, or marks the canister
  refilled, which sets it to its size. It is never assumed full
  (RAT-015). A new canister starts with the level entered, else unknown;
  an edit keeps the count; deleting a canister drops it.
- **Every run counts, as it goes.** A dose and a line fill take from the
  canister run by run, from the actual run time and the pump's
  calibrated flow, as the dose itself is booked (RAT-070). A calibration
  takes what was in the cup, once its amount is entered: the liquid left
  the canister wherever it went. A level that is unknown stays unknown.
- **Below zero is kept.** The count may fall below zero; it is never
  hidden as 0. Then the canister ran dry, or the level entered or the
  pump's calibration is wrong. An event reports it once when the count
  crosses zero; the watchdog shows it until a level is entered.
- **Low and used up.** Below 150 ml of a nutrient or 20 ml of pH−
  (RAT-071) the watchdog reports "refill soon". A dose that needs more
  than the counted level is not started: the check waits and names the
  canister; "Refilled" or a level entered starts it again at once. An
  unknown level holds nothing. The checklist (RAT-037) names a canister
  used up by the count.
- **Where it lives.** In the run-time state, so it survives a restart and
  a dose makes no new configuration revision. A backup carries the
  sizes, not the levels; a restore keeps the level of each canister the
  robot had and the file keeps (#18).
- **Later:** the drop sensor (SD-111) is what sees a canister run dry;
  the count only warns ahead of it.


## SD-120: How a pair's missing parts are counted

2026-10-27. How SD-114 is built; SD-114 stands.

- **What is missing.** The robot keeps, for the pair parts of each
  nutrient round, the amount planned and the amount that went in, booked
  as each part's dose ends. Once the round is over (measured, cut, or
  ended by a Stop), every part is brought to the share of its plan that
  the furthest part reached, so the pair keeps its ratio (RAT-054). A
  part missing less than 0.1 ml is left as it is.
- **A restart.** The part that was dosing counts what its runs booked in
  the journal, and a run the restart cut as if it went in whole. The
  other side of the pair is then caught up; the robot never doses more
  of a part than its plan.
- **The catch-up round** doses the missing amounts in the recipe's order
  and owes them whole: cut short again, what is still missing is caught
  up next. It counts as a nutrient round of the check, learns nothing
  (RAT-057), and waits like any dose for the canister's level (SD-119).
  An amount too small for one run, or one the run limits cannot take,
  drops the catch-up with a note.
- **The app** shows a pending catch-up on the status page with the parts
  and amounts, and "Don't catch up" ends it (`DELETE /api/v1/catchup`).


## SD-121: The EC reserve for pH− is planned cautiously until the effect is learned

2026-10-28. Found by step 4's acceptance in the twin (#10, step 4.9): from
tap water at pH 7.6, the pH− dose after the nutrients raised EC to about
1.85 at an EC target of 1.6, at 50 and 100 L. The reserve of RAT-053 was
planned at the start effect of 5 pH per ml/L and capped at 0.3 mS/cm; in
water with more alkalinity pH− works less, so more of it goes in, and EC
cannot be taken back.

- **Until the pH− effect is learned** (RAT-057), the reserve is planned
  at half the start effect, 2.5 pH per ml/L; once learned, at the learned
  effect, as before.
- **No cap** of 0.3 mS/cm. The aim never falls below the EC floor of
  RAT-046 plus the EC tolerance, so pH can always be dosed after the
  nutrients.
- **Topped up afterwards.** Once pH is within its range, the reserve no
  longer applies and the next round raises EC to its target; that round
  is measured like any other, and pH is checked again after it.
- The cost is one more nutrient round on a device's first tank. Too
  little EC can be dosed again; too much can only be diluted.

RAT-053 is changed accordingly.


## SD-122: A dose too small for a pump

2026-10-28. Found by step 4's acceptance in the twin (#10, step 4.9): at
5 L, the test dose of a new tank (SD-105) would run its smallest part
under 1 s (RAT-050), and the check waited for it without end.

- **A test dose** whose smallest part would run under the minimum run
  time is made as large as the smallest round every part can run, by one
  factor for all parts (RAT-054), as long as it stays within the round's
  cap (RAT-056) and does not go past the aim. It still checks the volume
  (SD-105), with a larger expected rise.
- **Otherwise**, and for every other nutrient round that is too small,
  nothing is dosed: the check notes it with the reason and goes on to pH,
  as a pH− dose that is too small already does (RAT-050).
- Whether a device can dose a small tank at all is a question of its
  pumps (#48).


## SD-123: Alarms with three levels, quiet hours and a morning report

2026-10-28. Decided by the owner for #10, step 5. RAT-022 is in force
again.

- **From the watchdog.** Every watchdog item that is a problem becomes an
  alarm: a latch at once, anything else once it has stood 5 min. A target
  out of range counts only while the check holds or waits, not while it
  doses and mixes towards it. An alarm ends with its problem.
- **Levels.** Critical: a leak, high level, a canister that drips without
  its pump, the dry-run lock, the return pump held off, pH out of its
  range. Info: what can wait for a maintenance date (step 5). Everything
  else is a warning: EC out of range, a canister low or empty, top up
  water, an outlet that does not drip, a probe without a valid reading.
- **Pushes.** Critical: at once, also at night, and again 5, 15 and 60 min
  later. Warning: once, by day; at night it waits for the morning report.
  Info: only in the morning report and the app.
- **Quiet hours** are 22:00 to 07:00 in the device's local time, set in the
  app; equal times mean none. The morning report comes when they end and
  lists what waited through the night and the notes.
- **Acknowledging** a raised alarm pauses its reminders for 12 h at most;
  the alarm stays raised and the status red. The pause ends at once when
  another alarm is raised, and a pause that ends starts a new round of
  reminders.
- **Across a restart** the alarms' state is kept in the run-time state, so
  nothing already known is pushed again.
- **Delivery.** The core queues the notifications in an outbox and sends
  nothing itself; the platform delivers them. Until a channel is set up,
  the app lists what would have been sent.

