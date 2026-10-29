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


## SD-124: Push without an account: ntfy, Telegram, e-mail through the platform

2026-10-28. Decided by the owner for #10, step 5; the channels themselves
are the product's (#10, new function 4).

- **Channels.** ntfy (any server; the topic is chosen like a password, an
  access token is optional), Telegram (a bot's token and a chat ID) and
  e-mail (an SMTP server with TLS: `smtps://`, or `smtp://` with STARTTLS
  required). Each can be switched on and off; a test message checks it.
  Home Assistant follows with MQTT (step 7).
- **Secrets apart.** The channels' settings live in a file of their own,
  not in the configuration: a backup, the diagnostics and `GET /config`
  never carry them. The API shows a token or password only as set or not;
  left empty in the app it stays, and it is never shown again. A file that
  cannot be read is no channel at all.
- **No line breaks.** No field may hold a control character, so nothing
  can add a header line or a recipient of its own.
- **The core sends nothing.** Each notification (SD-123) becomes one
  delivery per enabled channel, titled with the device's name, in the
  device's language. The German texts are the web app's own, copied into
  the core by `tools/gen_msg_de.mjs` and checked against them. The
  platform sends them and reports back; a channel that stops getting
  through is logged once.
- **The simulator sends through the system's curl**, on a thread of its
  own. The address and the secret go into a curl config file that only
  the user can read and that is deleted after the run; the command line
  names only that file. Without curl nothing is sent, and the app says
  why. The device will send with its own TLS.


## SD-125: Maintenance dates: fixed items, bundled

2026-10-28. Decided by the owner for #10, step 5 (new function 7).

- **Items, by what the device has:** calibrate the pH probe every 30 days,
  the EC probe every 90; calibrate each channel that holds a canister
  every 90; clean the measuring chamber and the suction filter every 30;
  check the pump heads and their tubes every 180 while any channel is in
  use; replace the pH probe every 365. The numbers are assumptions; each
  kind's interval can be set from 7 to 730 days.
- **Last done:** a calibration resets its item by itself; anything else is
  marked done in the app. An item with neither counts from when the
  device first had it, so a new device or channel is not due at once.
- **Bundled:** the earliest item opens a maintenance date, the local
  midnight of its day; whatever falls due within a week of it joins that
  date, so one visit does them all.
- **Reminded:** on its date an item is a note (SD-123): it comes in the
  morning report and stands in the app until it is done. It does not turn
  the watchdog red.
- **Defects** of a pump, the outlet, the flow or a probe are already
  alarms (SD-109, SD-111, SD-123).


## SD-126: The history keeps five years at one hour

2026-10-28. Decided by the owner for #10, step 5 ("compacted, kept for
several years").

- **A fourth level:** min, mean and max per hour for five years, after
  10 s for a day, 1 min for a week and 15 min for a year. Asked for a
  period, the finest level that still covers it answers; the CSV export
  is the same query.
- **Older snapshots** with three levels are read; the hourly level is then
  worked out from the 15-min level of the last year, so nothing is lost
  with the update. A snapshot is saved in the new form.
- **Space:** about 1.1 MB per series in full, 4.4 MB for pH, EC,
  temperature and flow. Whether the device's flash holds that is the
  product's question (#55); the simulator keeps it in a file.


## SD-127: The feed pump lies in the tank (product requirement, from PD-476 and PD-477)

2026-10-28. The product moved the device's own pump into the tank (#10,
update of 28 Oct). This changes SD-109 where it says so. The owner decided
the control details in this session.

- **Term.** The device's own pump is the *feed pump*: it moves tank water
  through the measuring chamber. The profile says where it sits:
  - `tank`: on the tank's bottom, pushing water up a hose (both example
    profiles);
  - `device`: drawing water through the device (SD-109), in case the bench
    test brings the suction pump back (PD-476).

  Logic reads the position, never the line.
- **No priming.** In the tank the pump has no priming: water reaches the
  chamber within seconds. No flow for 6 s switches it off, before the
  hardware's own cut at about 8–10 s (PD-477). This holds at the start and
  at any time while it runs, mixing included. A pump in the device keeps its
  2 min to prime (SD-109).
- **Two tries.** Each try without flow can spill water if a hose is off.
  - In the tank the check tries once more after 5 min.
  - A second try in a row without flow locks the pump ("no flow", a
    critical alarm, SD-123) until a person releases it in the app.
  - In the device it takes three tries, as before (SD-109).

  A round cut by no flow learns nothing, and what went in is mixed first on
  the next try (SD-106).
- **The hardware's cut.** The hardware may switch the pump and dosing off
  for no flow. The device then reports it (`release.noFlow`) and holds the
  pump off until it is switched off again. The check stops and says "No
  flow: the hardware switched the feed pump off". It counts this as a try,
  and switching the pump off re-arms the hardware.
- **Dry run from the current** (RAT-049).
  - While flow is proven, the device learns the pump's normal current at its
    speed.
  - Below 60 % of it for 2 s, the pump goes off at once. That counts as a try
    without flow, with the reason "dry run".
  - Until the current is learned, only the 6 s window protects.
  - The thresholds are assumptions until the bench test.
- **pH and EC with the pump on and off.**
  - At the end of each check, the readings of the last 10 s with the pump
    running are compared with those 10–20 s after it stopped.
  - A difference beyond 0.1 pH or 0.05 mS/cm (assumptions) repeats the
    comparison once, at once.
  - If confirmed, the pump and dosing are locked with a critical alarm
    ("check the pump and its cable") until a person releases it. This adds
    a critical alarm to SD-123.
  - A comparison during which no flow comes back says nothing; the next
    check compares again.
- **Maintenance.** "Clean the measuring chamber, the feed pump and its inlet
  screen" replaces the suction filter (SD-125).
- **Later.** Commissioning (step 7) brings:
  - the step "Place the pump in the tank" instead of priming;
  - the confirmation that the tank is an open container;
  - the cable check at commissioning;
  - an optional flow calibration with a measuring jug, for a profile
    without a flow meter.

  Setting the pump's speed waits until a use needs it; the check runs at the
  profile's speed.


## SD-128: Plants and their phases set the water

2026-10-28. Step 6 of #10 (plant recipes, phases, calendar, cultivation
system; PD-451). The owner decided the four questions of this step in
this session.

- **Plant recipes and plants.**
  - A plant recipe holds phases. Each phase has a free name (a label only,
    RAT-076), its days (1–999), and whether it moves on by itself or only
    once the user confirms. It also has the water it asks for: a recipe,
    with an EC target and a pH range of its own where they differ from the
    recipe's (RAT-066).
  - A plant is started from a plant recipe and copies its phases, so a
    later change of the recipe does not move it.
  - Plants and their progress are part of the configuration, so a backup
    carries them.
- **The water plan.**
  - The leading running plant's current phase sets the recipe and the
    targets. The leading plant is the one chosen, else the earliest
    started running plant.
  - Without a running plant, the recipe chosen by hand applies, as before
    (SD-105).
  - Starting the first plant ends the hand choice. While a plant runs, a
    hand choice is refused.
  - Once the last plant is completed, the robot holds and says so. No old
    hand choice comes back.
  - Another running plant on the same tank whose targets differ is said as
    a note (PD-318).
- **Phases in days.**
  - Day 1 is the day the plant started.
  - A phase that moves on by itself does so at the local midnight after its
    days, also after the device was off.
  - A phase that waits for confirmation keeps its targets after its days
    and reminds daily, counting the days past the plan. So does the last
    phase, until the harvest is recorded (RAT-002).
  - "Next phase" starts the next phase today.
  - A phase that began today is noted, so the morning report says it.
  - All these notes are info alarms (SD-123).
- **Harvest and the end** are the user's (RAT-002, RAT-077). The harvest
  is an event with its time. Completing is a step of its own, after which
  the next running plant leads.
- **Cultivation system** per tank: hydro, coco, rock wool or soil. Every
  plant on the tank gets the same water.
  - It sets the pH range a new phase is offered when its recipe has none:
    hydro and rock wool 5.5–6.1, coco 5.6–6.2, soil 6.2–6.8 (assumptions,
    not from a chart).
  - Templates made for it come first.
  - Control never reads it (RAT-076).
- **Calendar:** a list of the next 8 weeks by day. It shows phase changes,
  planned ends, harvest reminders and the maintenance dates (SD-125), with
  today at the top. What is past its plan stands at today, marked overdue.
  Across coupled devices it comes with step 8.
- **Later:** the end-of-run summary (RAT-064), and phase parameters for
  light, climate and watering (step 9).



## SD-129: Access tokens with three roles, a device ID, commissioning in the device

2026-10-28. Step 7 of #10 (API for the app, and the commissioning flow).
The owner chose three fixed roles and commissioning steps kept in the
device in this session. Where updates come from is a product question
(#62).

- **Access tokens.**
  - Only the owner creates them: signed in with the password, in the web
    app's settings. Each token gets a name (1–40 characters) and a role.
    There are at most 16.
  - A token is 256 random bits with the prefix `dpt_`. It is shown once.
    The device keeps only its SHA-256 hash, so a plain hash is enough and
    no slow password hash is needed. It is sent as
    `Authorization: Bearer`, and it never goes into a cookie.
  - The owner revokes each token on its own, and the revocation takes
    effect at once. Creating and revoking a token are events, with the
    token's name and role, never the token itself.
  - The list shows each token's last use since the start. That time is
    kept in memory only, so a call does not write to flash.
  - A password change ends every session; the tokens stay, and the event
    says so. A reset of access with the device security items will clear
    them (#10).
- **OpenAPI.** `docs/openapi.json` describes every route, by hand. Each
  operation names its roles as `x-roles`. Tests check that every route of
  the router is in the file and nothing else, and that the roles named
  are exactly the ones the device lets call it. The device does not serve
  the file: it stays in the repository and costs no flash.
- **Roles.** A call that a role may not make is refused with 403. The
  message names the role.

  | Role | May |
  |---|---|
  | Owner (session) | everything |
  | `app` | everything but the password and the access tokens |
  | `account` | read; Stop; acknowledge alarms; recipes, the recipe in force, plants and plant recipes |
  | `peer` | read; its coupling calls come with step 8 |

  The account role never reaches limits, releases (latches, resume),
  calibration, single pumps, settings, the backup or maintenance. This is
  the same set that MQTT commands will get (PD-461). The simulator's
  controls count as changes: owner and app only.
- **Device ID.** It is 64 random bits as 16 hex digits, made at the first
  start and kept in its own file. It is in `/info` and in the diagnostics.
  It is never in a backup, so restoring a backup on another device does
  not copy the identity. A damaged file gives a new ID.
- **Commissioning.** The steps of #10 are kept in the device, in their
  order, so they resume after a restart:
  - Language and password, placement, flow, probes, channels and pump
    heads, lines filled, test run, tank and recipe, push. Network and
    account are shown as "later" until they exist.
  - A step the robot sees is done while it sees it, and nothing of it is
    stored: the password is set, flow was proven, both probes are
    calibrated, every canister's channel is calibrated for its head with
    pH− among them, a run went through each of those lines to its end
    (filled until it drips, or calibrated into the cup), a tank volume and
    a recipe in force.
  - The user confirms the rest: the placement (the pump lies in the tank,
    the container is open, the canisters stand below the device; PD-476),
    and the test run of drop sensors, high level and Stop. Push is
    optional and may be skipped. These marks are kept in the
    configuration, with their time, and can be opened again.
  - The web app has a page for it, and shows the next open step above
    every other page until all are done or skipped. The state carries
    whether it is complete and the next step.
  - The placement confirmation is the one of SD-118. Until it is given,
    the cycle stays off and says why. The gateway lets nothing run
    towards the tank: no dose, no line fill into the tank, no refill of
    a pulled-back line. Calibrating or priming into a cup still runs.
    Confirming it is an event. A backup never brings any commissioning
    mark: the device keeps its own, so a restored or new device asks
    again.
  - Apart from that, commissioning only shows the way. Dosing keeps its
    own checks.
  - The simulator's demo is set up as a device in use.


## SD-130: MQTT with Home Assistant discovery

2026-10-28. Step 7 of #10 (PD-461: MQTT with Home Assistant discovery,
read and write, at launch). The owner decided that values go out also
without TLS, and commands only over TLS with a user.

- **Off by default.**
  - The settings are the broker's host and port, TLS, a user and
    password, whether commands are taken, and Home Assistant's discovery
    prefix.
  - They live in a file of their own, as the push channels' do (SD-124).
    A backup, the diagnostics and `GET /config` never carry the password,
    and the API says only whether it is set.
  - The owner and an app token change them; every role reads them
    (SD-129).
- **Topics** under `doseph/<device ID>`:
  - `state`, retained JSON: pH, EC, temperature, flow proven, stopped,
    the cycle's phase, the recipe in force, its EC and pH target, whether
    an alarm stands, and the canister levels. A value the robot cannot
    use is null, never 0 (R5).
  - `availability`: "online", and "offline" as the broker's last will.
  - `result`: the answer to a command.
- **Discovery.** Retained configs under
  `<prefix>/<component>/doseph_<device ID>/<object>/config`:
  - sensors for pH, EC, temperature, the cycle, the recipe in force and
    its targets, and one per canister;
  - binary sensors for flow proven, stopped and alarm;
  - the device itself with its name, line and version.
- **Reading** goes to the broker also without TLS. The app warns that
  anyone on the network can then read along.
- **Commands** come only over TLS with a user, once the owner switches
  them on:
  - Stop;
  - the recipe in force, by its name;
  - the EC target of the recipe in force;
  - its pH target, where the range keeps its width (0.4 without one).

  Each command is a call of the account role (SD-129) through the same
  API path, so it passes the same checks and locks as one from the app.
  While a plant leads, its phase is changed in the app. Limits,
  releases, calibration and single pumps are never commands (PD-459).
  Each known command is logged with its result. While commands are off,
  their entities are removed from Home Assistant.
- **Transport.** The core builds the topics, the payloads and the packets
  of MQTT 3.1.1, at QoS 0. The platform holds the connection (R8).
  - On the device, the firmware's MQTT client carries them, with TLS.
  - The simulator connects on a thread of its own, over plain TCP
    without TLS. It sends its values and takes no commands. With TLS on,
    it says that it has none.
  - The connection says it is online, with "offline" as its last will.
    It sends the state every 2 s when it changed and at least once a
    minute, and the discovery again when the configuration changed.
  - It pings when nothing went out for 30 s, and it counts a broker
    silent for 90 s as gone.
  - A lost connection is tried again after 10 s. The settings show
    whether it is connected, or why not.


## SD-131: Two devices on one tank pair with a code

2026-10-28. Step 8 of #10 (coupling two devices, PD-457). The owner
decided the three questions of this step in this session.

- **The group.**
  - Two devices on one tank form a group. The one where the owner starts
    the pairing leads: it measures, controls and plans. The other doses
    on request and measures as a check (PD-457).
  - The link is direct, in the home network, without a cloud.
- **Pairing with a code.**
  - The second device shows a pairing code: 8 characters (40 bits) in two
    groups of four. It is valid for 10 minutes and kept in memory only.
    The fifth wrong try voids it, and each wrong try is an event.
  - The owner enters the code and the second device's address on the
    leading one.
  - The leader calls the second device's `POST /coupling/join`, where the
    code is the credential. Each device then makes a peer token (SD-129)
    for the other, so each holds the token it uses to call the other.
  - A failed pairing says why (wrong code, unreachable, refused), and the
    leader revokes the token it made.
  - A device is never coupled with itself. It has one peer at most.
- **The record.**
  - The peer, with the token this device uses to call it, lives in a file
    of its own, apart from the configuration (as SD-124). A backup never
    carries it, and the API never shows the token.
  - A record that cannot be read leaves the device alone, with an event.
- **Ending.** Either device ends the coupling, and revokes the token it
  made for the other at once.
  - The leader also tells the follower, which ends it too.
  - Only the coupled device's own token may end it there. An account
    token, or a peer token made by hand, may not.
- **The link.**
  - The leader calls the follower's `POST /peer/beat` every 2 s, saying
    whether its feed pump runs.
  - The answer has the follower's readings, its flow and pump, and the
    run the leader asked for. The leader shows whether the follower was
    heard within 10 s.
- **The follower.**
  - It has no recipe of its own. Its feed pump runs while the leader's
    does, through its own check with every guard: the start window, the
    tries without flow, the dry run and the cable check. The pump is off
    otherwise.
  - It doses only on the leader's request (`POST /peer/run`): one run on
    one channel, for EC or pH. Its own gateway decides, as for its own
    doses: flow, placement, calibration, locks, daily amount, drops.
    Only one run goes on there at a time.
  - After 10 s without a call, it cuts a running run, books what ran, and
    takes no new run until the leader calls again. Losing and regaining
    the link are events. After a restart it waits for a call (R6).
  - Only the token made for the leader opens these calls.
- **Dosing across both.**
  - On the leader, a canister may stand at a channel of the follower.
    It names the follower by its ID and the channel there.
  - The leader plans with the follower's calibrations, which the
    follower reports with each answer. While the link is down, those
    canisters are not planned, and the watchdog says so.
  - The leader's doser sends each run of such a step to the follower
    and books its result. The follower journals, refills, pulls back
    and books its own channel.
  - A run there that does not end in its time plus 12 s is booked as
    run and fails, the safe direction.
  - The leader's doser runs one order at a time, so never more than one
    channel runs across both devices for dosing.
  - The follower's own channel jobs (calibration, priming) run only
    while the leader is in maintenance.
  - Stop, a hardware hold and the placement on the leader hold the
    follower's runs too.
- **The watchdog** on both devices shows the link, as linked or lost,
  and the check of the readings.
- **The check.** The follower measures as well. While both pumps move
  water, readings that differ by more than 0.3 pH or 0.2 mS/cm for
  10 minutes raise a warning, with an event. Readings that agree again
  end it. The leader keeps dosing by its own readings.
- **The platform** carries the requests between the devices (R8). The
  simulator uses plain HTTP in the home network; the device will use
  HTTPS with the device security items (#10).


## SD-132: Shelly and ESP devices: a refill valve and a circulation pump first

2026-10-29. Step 9 of #10 (Shelly and ESP devices). The owner decided the
order and the questions of this part in this session. Light, climate and
watering follow with decisions of their own.

- **Order.** 9.1 the network devices; 9.2 the refill valve (PD-460);
  9.3 the circulation pump (PD-421); then light, climate and watering,
  each as a change of its own. Mixing comes first (PD-402).
- **Switching only through Shelly Gen2+.**
  - The robot switches network outputs only on Shelly devices of the
    second generation or later, over their local RPC interface. Their
    safety settings can be written and read back there.
  - Gen1 Shelly devices and ESP relays are not switched. A Shelly
    without the RPC interface counts as the first generation.
  - A device is added by its address and known by its own ID, which it
    says at first contact and in every answer. The address is only where
    it is reached now: another device answering there is offline and
    says so. An address is added once.
  - A device with a password: the robot keeps the password apart from
    the configuration, never in a backup and never shown (as SD-124).
- **Binding an output to a role** (RAT-019, RAT-060).
  - The robot writes "off after power loss" and "never on by itself"
    into the output. Where the device has an input of its own, the input
    is detached from the output: the robot reads it as a value.
  - For a pulse role, such as the refill valve, it also writes an
    auto-off at 1.11 × the role's longest run, rounded up to a whole
    minute. A continuous role, such as the circulation pump, gets none.
  - The output may switch only once these were read back as written.
  - Before every switch-on the robot reads them back again. If they
    differ it writes them once more; if they still differ, it does not
    switch on, and the watchdog says why. It tries again after a minute.
  - An output bound anew, or whose device was away, or that is on
    without the robot having switched it after a check, is switched off
    and its settings are checked before it may switch on. Off goes at
    once, and again while the device still says on. A Stop and a start
    switch every output off, whatever it said last.
  - An answer counts only for the request it answers.
  - An output let go (its role released or moved, its device removed or
    moved, a backup restored) is switched off, and the off is repeated
    until the device answers as itself; after a minute, or at once where
    another device answers at its address, an alarm says it may still be
    on. A want from before never switches a later binding on.
  - Every output switches only through the gateway (R1).
- **Values from devices.** A role that reads a value (a float switch, a
  temperature) takes it from one of these:
  - a Shelly Gen2+, its inputs and sensors, read every 5 s;
  - an ESPHome device's web server, read every 5 s, by the URLs of its
    REST interface (`/binary_sensor/<id>`, `/sensor/<id>`; to be checked
    with a real device before devices are offered);
  - an MQTT topic on the broker of SD-130 (ESPHome, Tasmota and others),
    as a plain value or a field of a JSON message.
  - A polled value is stale 30 s after it was read. An MQTT value counts
    while the broker connection stands and, where the device has one,
    its availability topic says online; after an outage only once it
    comes again.
  - A role with no device is "not assigned" and never has a value. An
    assigned device that stops answering is "offline", a problem
    (RAT-024). A missing value is never 0 (R5).
- **The refill valve** (PD-460).
  - It needs a valve output on a Shelly Gen2+ and a float switch in the
    tank that reports "full", from any of the sources above.
  - It opens when the robot raises "top up water" (SD-115), or by a
    button in the app. Not through an account token or MQTT.
  - It does not open during Stop, maintenance or a hardware hold, before
    the placement is confirmed, while the float reports full or has no
    value, or while the refill is latched.
  - It closes when the float reports full, at the longest fill (15 min
    by default, 1 to 25 min, RAT-079), on Stop, maintenance or a hold,
    and when the float loses its value. The Shelly's own auto-off is at
    28 min.
  - While it fills, the robot doses nothing and starts no round. Then
    the next check waits for the mixing wait (SD-112).
  - A fill that reaches its longest time without "full", or whose float
    lost its value, latches the refill with an alarm. No fill opens
    until a person has looked and released it (RAT-031).
  - After a restart the valve is off and a cut fill is reported, not
    resumed (R6).
  - Each fill is an event, with its time and why it ended.
- **The refill valve's details** (9.2, decided here by the minimal
  principle).
  - "Top up water" asks for one fill each time it is raised. The request
    waits while a dose or a channel job runs, while the valve is checked
    and while the float has no value. It lapses during Stop, maintenance
    or a hold, while the refill is latched, when the float says full, and
    when "top up water" goes. One that stands when the robot starts asks
    for none (R6).
  - The button starts a fill at once, or says why it cannot. It waits for
    nothing.
  - The gateway keeps the valve closed unless the float says "not full",
    whatever wants it open (RAT-032). A want it cannot follow for the
    float, a valve it closed at the longest fill, and a valve whose device
    was away open again only when wanted anew.
  - A float switch over MQTT needs an availability topic: a value kept on
    the broker would outlive a sender that stopped, and the float is the
    valve's limit.
  - A valve that has not opened a minute after the start ends the fill
    without a latch; the watchdog says why. A valve that stops answering
    while it is open latches the refill (RAT-031).
  - A person's stop, and a change of the valve's or the float's binding,
    end a fill without a latch. During a fill a backup is not restored,
    and the longest fill does not change.
  - The refill's latch may report only (SD-133).
- **The circulation pump's details** (9.3, decided here by the minimal
  principle).
  - "While mixing" (the default) runs it during each dose and its mixing
    wait, and during a fill. It is off while the check measures, so that
    every reading is taken alike and a pump that disturbs the probes
    cannot bias one (RAT-044).
  - "Always" runs it all the time. After a restart it is first switched
    off, and on again once its settings were read back (RAT-019). Found
    off at the device (a short power cut, a hand at the plug), it is
    checked again and switched on again.
  - The follower of a coupled pair never runs it: the leader runs the
    check.
  - The robot asks for it on every tick; the gateway switches (R1).
  - Only a session or an app token sets its mode.
- **The circulation pump** (PD-421).
  - A customer's pump in the tank, on a Shelly Gen2+ output.
  - By default it runs from the start of a dosing round and during a
    fill until the mixing wait has ended. A setting makes it run always.
  - It is off during Stop, maintenance and a hardware hold, and after a
    restart until the robot switches it on again (R6).
  - It does not shorten the mixing wait; stability decides (#1).
- **The rules of the grow controller's refill by level** (RAT-001,
  RAT-030, RAT-038) need a level sensor and stay for later. RAT-031,
  RAT-032 and RAT-079 apply to the float as above.


## SD-133: A lock a person releases may be set to report only

2026-10-29. Decided by the owner in this session, for operations that
watch their tanks in their own way.

- **Which locks.** Every latch a person releases, except leak, high level
  and the cable check:
  - no flow after the tries, and a dry run (SD-109, SD-127);
  - a tank volume that does not fit the test dose (SD-105);
  - EC or pH without effect (SD-105, RAT-041, RAT-055);
  - an outlet without drops, and drops without a pump (SD-111);
  - the return pump running too long (SD-109);
  - the refill (SD-132), once it is built.
- **The setting.** Each of them is set to "lock" (the default) or
  "report only".
  - With report only, the event and the alarm come as before, with the
    same level and repetition (SD-123), once while the report stands.
    The robot goes on as if the lock had been released at once. Status
    shows the report until a person marks it seen.
  - A latch set while its lock locked holds until it is released. Once a
    lock locks again, its next latch holds, even while a report of it
    still stands.
- **The frame never changes** (PD-459), whatever is set:
  - leak, high level, Stop and a hung controller switch off;
  - the cable check locks the pump and dosing: a damaged cable in the
    water may endanger people (decided by the owner);
  - no dose without proven flow (SD-105);
  - one channel at a time, the run time per dose and the daily amount;
  - the Shelly's auto-off and "off after power loss" (SD-132), and the
    longest fill.
- **Who.** Only signed in in the app or the web app, or with an app
  token. Never through an account token, MQTT or the coupled device.
  Each change is an event, also one a backup brings.
- **Shown.** While any lock is set to report only, the watchdog names it
  in a note, not as a problem: no alarm, and the problem flag (also over
  MQTT) stays free for real faults (decided by the owner). The settings
  warn, and the diagnostics package lists it.
- **R7 holds.** The frame is the hard limit; a lock above it is a
  setting, and report only goes back to the frame, never below it.


## SD-134: Watering from the tank, in any number of circuits

2026-10-29. Step 9 of #10, after the refill valve and the circulation pump
(SD-132). The owner decided the order (watering before light and climate)
and the questions below in this session. The rest are control details
decided here by the minimal principle. Light and climate follow with
decisions of their own.

- **Circuits.** Any number of watering circuits. Each is a pump or a valve
  on a Shelly Gen2+ output (SD-132), with a name, a schedule, a pump rate
  for counting litres, and optionally a plant whose phase sets it.
  - All water from the same tank. One circuit runs at a time: a shared
    tank, the pressure, and an exact count of litres. A shot that is due
    while another runs waits for it.
  - A ceiling in code keeps the configuration bounded (64 circuits).
- **When a circuit waters** (decided by the owner: as the user prefers).
  The schedule is one of:
  - shots in a daily window: a window (from, to), shots per day and the
    length of a shot, spread evenly over the window (RAT-010);
  - fixed times, each with its own length;
  - an interval: every N minutes, for a length, around the clock;
  - by hand only.
  - In every mode a person can water now, one shot of a given length.
  - A circuit linked to a plant takes shots per day and the length of a
    shot from the plant's current phase where the phase sets them
    (RAT-076: parameters, never names).
- **Water fit for watering** (decided by the owner: a wide band, with an
  override; RAT-086, RAT-084).
  - A circuit does not start, and a running one stops, while the tank's
    pH is below 4.0 or above 8.5, or its EC is more than 1.0 mS/cm above
    the EC target (the higher of this phase's and the last phase's), and
    always above 5.0 mS/cm. There is no lower EC bound.
  - A pH or EC probe without a usable reading, or in a calibration,
    blocks as well (R5).
  - After a dose, a fill or a start, watering waits until the water has
    been mixed in: until the check has measured again after its mixing
    wait (SD-112). A dose that starts stops a running shot.
  - pH not corrected does not block watering. It shows as a warning with
    its reason (RAT-084).
  - **Override.** A person can water anyway: one shot by hand, after a
    confirmation that names why the water is not fit. The shot is a
    warning event. A schedule never overrides, and neither can an account
    token, MQTT or the coupled device.
- **An empty tank** (decided by the owner: counting litres, a float as an
  option, and a level probe provided for; RAT-067, RAT-068).
  - The robot counts the litres given: each circuit's pump rate, which
    the user enters or measures into a jug, times its run time. The count
    starts again when the float says full, or a person announces a new
    fill.
  - It waters only while the tank keeps a reserve: 20 % of its volume by
    default, never below what keeps the feed pump in the water. A shot
    that would go below the reserve does not start, and "top up water" is
    raised.
  - A float switch "tank empty" may be assigned as a role. While it says
    empty, or has no value, no circuit waters.
  - A level probe may be assigned as a role "tank level", in litres, from
    a Shelly, ESPHome or MQTT. With one, the reserve is checked against
    the level and the count only shows. A level without a value blocks.
  - No flow at the feed pump (the latch of SD-109) blocks every circuit.
  - Whether the device itself gets a connector for a level probe is a
    hardware question for the product side.
- **Limits** (decided by the owner: set freely by the user).
  - The user sets the length of a shot and, optionally, a daily run time
    per circuit. The code has no product limit, only a technical ceiling
    the device accepts: 24 h per shot.
  - The robot writes the auto-off into the Shelly at 1.11 × the longest
    shot the circuit's schedule can give, rounded up to a whole minute
    (RAT-019), so that a hung robot does not keep watering.
  - Every switching goes through the gateway (R1), with the read-back of
    RAT-019.
- **Off.** Stop, maintenance and a hold of the hardware stop every
  circuit at once. After a restart every circuit is off (R6). A shot cut
  by the restart is reported and not repeated; the schedule goes on with
  its next shot.
- **The tank stays mixed.** A shot does not start during a fill, a dose
  or a mixing wait. The circulation pump (SD-132) is not tied to
  watering.
- **Who.** Circuits, schedules, the override and "water now" only from a
  session or an app token. An account token may only pause and resume a
  circuit's schedule.
- **Shown.** Each circuit with its next shot, its last shot and why it
  did not water, the litres given since the last fill, and the reserve.
  Each shot is an event, with its length, its litres and how it ended.
- **Rules from practice** that come into force with it, in their Doseph
  form: RAT-010 (shots), RAT-067 and RAT-068 (minimum level), RAT-084
  (pH not corrected), RAT-086 (water fit for watering). RAT-014 (drain
  percentage) waits for a drain measurement.
- **Details settled when it was built** (#80, control details by the
  minimal principle).
  - Schedules go by each minute of the clock that passes, in the local
    time of that minute. A clock set back by up to 90 minutes gives no
    shot twice. A clock set or lost after a start, or one that leaps
    further either way, starts the schedules from the new time and
    makes nothing up (R6): a long wait or a burst of shots would do
    more harm than one shot again. On the night the clocks go back, the
    repeated hour's shots come twice; on the night they go forward, the
    skipped hour's do not come.
  - A shot due while it cannot start waits at most an hour, then lapses;
    one still waiting when its circuit's next comes is skipped. Each
    reason is logged once. A paused circuit forgets what waited.
  - The count of litres has no start until a new tank or the float
    "full"; before that, a counted circuit does not water (R5). Water of
    a circuit whose flow is not known leaves the count without a value
    until it starts again. A shot counts from its switch-on, as the
    circuit was when it began. One cut by a restart counts as run until
    the device's own auto-off, the most it can have run.
  - A shot holds only once its output says "on", and ends at its length
    from the switch-on; switched off at the device, it ends. A circuit
    changed or removed ends its shot; a change keeps a pause.
  - The reserve or a float "empty" sets "water low", which a refill
    valve answers with a fill of its own. It clears with a new tank, the
    float "full", or a level probe 10 % of the tank above the reserve.
  - Only a circuit with a time window follows a plant (#10 step 9.4c). A
    phase may set shots a day and the length of a shot; what it leaves
    empty, and everything once the plant no longer runs, is the
    circuit's own. A phase's shot is never longer than the circuit's
    longest shot. A plant deleted lets its circuits go. A phase change
    counts from the next shot that comes due once the robot has moved
    the plant on (within seconds of midnight); a shot that waits or runs
    keeps its length. A plant copies its phases when it starts (SD-128),
    so shots set in a plant recipe reach plants started afterwards.

## SD-135: Light, on a schedule and dimmed, set by the plant's phase

2026-10-29. Step 9 of #10, after watering (SD-134). The owner decided the
questions below in this session. The rest are control details decided here
by the minimal principle. Climate follows with decisions of its own.

- **What** (decided by the owner: on and off, and dimming over 0–10 V).
  - Any number of light circuits, as with watering; a ceiling in code
    keeps the configuration bounded (16). Each is a Shelly Gen2+ output
    (SD-132): a switch for on and off, or a dimmer with a 0–10 V line for
    the brightness in %.
  - A circuit's day starts at its own local time and lasts its hours of
    light. The time it starts stays with the circuit, so the day does not
    wander when the hours change.
  - Brightness below the lamp driver's switch-on threshold is raised to
    the threshold; 0 % switches the light's relay off as well (RAT-013,
    RAT-029). The threshold is a setting of the circuit.
  - Light runs for hours: its output gets no auto-off in the device
    (RAT-019). Every switching goes through the gateway with its
    read-back (R1); for a dimmer the brightness is read back too.
- **Sunrise and sunset** (decided by the owner: set per circuit). A
  circuit has a duration for each, 0 for hard on and off. The ramps lie
  within the day: the light rises from the start of the day and falls
  until its end.
- **The plant's phase** (decided by the owner: hours and brightness).
  A light circuit may follow a plant; while the plant runs, its current
  phase sets the hours of light and the brightness where it has them,
  otherwise the circuit's own apply (RAT-076: parameters, never names).
  This is the pattern of watering (SD-134). A plant deleted lets its
  circuits go.
- **After a restart** (decided by the owner: on by the plan). The robot
  switches each light as its plan wants it now: on by day, off by night,
  and reports the restart. Light does no harm by being on or off at its
  planned time, so R6 holds: what can do harm (pumps, valves) stays off.
  A Shelly is off after a power loss (RAT-019) until the robot switches
  it.
- **STOP, a hold and maintenance** (decided by the owner: light always
  by the plan). None of them switches the light. The STOP dialog names
  pumps, valves and watering circuits only.
- **By hand.** A person may switch a light on or off; it holds until the
  plan's next change (the next start or end of the day). A light switched
  on by hand in the dark period is shown as such.
- **Heat** (decided by the owner: with the climate step). Light gets no
  protection against heat now; it is built with the climate, where the
  room temperature gets a role.
- **Who.** Light circuits, their plans and switching by hand only from a
  session or an app token.
- **Rules from practice** that come into force with it, in their Doseph
  form: RAT-013 (the driver's threshold) and RAT-029 (0 % switches the
  relay off). Both were dropped for the mixing robot alone (SD-105) and
  are taken up again.
