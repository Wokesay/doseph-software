# Rationale register

Each domain rule of this software that was learned from operating
practice has an entry here. The entry says what the rule is, why it
exists, what evidence backs it, where it is implemented and which tests
cover it. Code, tests and other docs cite only the ID (RAT-001 …).

- **IDs are stable**: a rule keeps the ID it had in the grow controller
  this software started from (`docs/PROVENANCE.md`), and an ID is never
  reused. A rule of the grow controller that the mixing robot does not
  need stays listed as dropped, with the area it belonged to, the rule as
  the grow controller had it, and under **Later** the step of the order
  of work (#10) that would bring it back.
- **Reference installation** means the private installation on which the
  rules were first developed and measured. Evidence names the year only.
- **Evidence** says whether a number was measured, observed, calculated
  or found in a review. "—" means there is no number behind it. The
  evidence was gathered for the grow controller; it applies to the
  mixing robot where the rule does.
- **Changed** says what changed from the grow controller's rule.
- New rules get the next free ID (now RAT-090), an entry here and a test.
- What later steps rebuild that is no rule of its own – grow phases and
  harvest, the setup assistant, the scenario harness, the dry-run latch –
  is in `docs/LATER.md`.

### RAT-001 – Fill volume is computed; the level sensor only checks and cuts off

- **Dropped:** the grow controller only (fill volume and level sensor); not part of the mixing robot (SD-105).
- **Rule:** A refill opens the inlet for a time worked out from the litres missing (target minus present level) and the inlet's set flow rate. The level reading can only end a fill early, once it is 1 L past the target, and serves as the cut-off. Without a set flow rate there is no refill, and the target stays at least 1 L below the tank's capacity.
- **Why:** A fill that stops on the level sensor alone inherits all of its faults, from slow drift to a value that no longer moves, and can overflow the tank. A computed amount bounds the fill independently, while the sensor remains as plausibility check and cut-off. Logging both the computed and the measured litres of every fill shows drift as it starts.
- **Evidence:** Observed on the reference installation (2026): the fill target was a setting (100 L by default in a 150 L tank), with a fixed cut-off at 140 L whatever the target. After a move to a 60 L tank that overflowed at 55 L, the cut-off went to 45 L and the target was capped at 40 L.
- **Later:** step 9 (refill valve and level sensor). Until then the robot only asks for water (SD-115).

### RAT-002 – Harvest is recorded only by the user

- **Rule:** The robot never declares a harvest itself: a harvest is an event with a time that the user records, and closing the grow run is a step of its own (RAT-077). Each phase says whether it moves on by itself after its planned days or only once the user confirms; the harvest always waits for the user.
- **Why:** Whether a plant is ripe is judged by looking at it, not by counting days. A harvest set off when the planned flowering days run out acts on a guess and can contradict what the user sees.
- **Evidence:** Observed on the reference installation (2026): a harvest the plan triggered was replaced by the user's confirmation, with a daily reminder that also counted the days past the plan.
- **Implemented in:** `core/include/doseph/plants.hpp`, `core/src/plants.cpp`, `core/src/robot.cpp`
- **Tests:** `tests/core/test_plants.cpp` "Plants: a phase that waits for its confirmation stays after its days and reminds daily; the last phase reminds to record the harvest; a phase begun today and a plant with other targets are said (SD-128, RAT-002)"

### RAT-003 – One pump job at a time; bad calibration results are rejected

- **Rule:** Only one pump job, a calibration or priming, runs at a time, and none while the cycle doses; a second start is refused with the reason. A calibration result that is missing, zero, negative or above 1000 ml is rejected and the previous value stays.
- **Why:** A dosing sequence that can start twice overdoses. One that runs on a missing or zero rate either has no exit or underdoses, and either way it looks like success.
- **Evidence:** Observed on the reference installation (2026): a mixing sequence that started without valid inputs could neither finish nor stop, and a flow rate of zero would have passed as valid. Limits on attempts and run time, a refusal of zero rates and a lock against a second start fixed it.
- **Changed:** The grow controller's mix jobs and their confirmation are gone; the cycle (SD-105) is the only thing that doses into the tank.
- **Implemented in:** `core/src/config.cpp`, `core/src/robot.cpp`
- **Tests:** `tests/core/test_cycle.cpp` "Pumps: a calibration runs into the cup and is worked out from the actual run time; a senseless result is refused (RAT-003, RAT-070)"

### RAT-004 – pH correction always comes last and is never part of a recipe

- **Rule:** Recipes hold nutrients only. pH− in a recipe is refused when the configuration is saved. The cycle corrects pH after EC, and pH waits while a nutrient round is open.
- **Why:** Nutrients shift pH, and the base pair adds most of the EC and pulls pH down by itself, so a pH step taken before it aims at a solution that does not exist yet. The tank then reaches irrigation at an unknown, usually too low pH, and without a pH-up channel nothing can bring it back.
- **Evidence:** Observed on the reference installation (2026): with acid added before the base pair and no measurement afterwards, the final pH was unknown. With acid last, it adds only a few hundredths of a mS/cm (about 20 ml per 100 L), less than the 0.05 EC tolerance. Precipitation, the usual argument against a late pH step, did not apply: the water was reverse-osmosis water with almost no carbonate hardness, and a buffer went in before any nutrient.
- **Implemented in:** `core/include/doseph/plan.hpp`, `core/src/config.cpp`, `core/src/cycle.cpp`
- **Tests:** `tests/core/test_config.cpp` "Configuration: pH correction is never part of a recipe (RAT-004)"; `tests/core/test_cycle.cpp` "Cycle: feed pump on, flow proven, flushed, measured; a test dose first, EC before pH, mixed in with flow, measured again, then it holds (SD-105, RAT-004, RAT-052)"

### RAT-005 – The start-up grace period applies to the watchdog only

- **Rule:** For 120 s after a start the watchdog reports target checks as neutral ("starting up") instead of as problems. The grace period applies to the watchdog only; the cycle works towards its targets at once.
- **Why:** Values need time to settle after a restart, and judging them during that time only produces false alarms. Controllers are different: after a phase change they must start working toward the new targets at once, so a grace period would only delay them.
- **Evidence:** — (design rule; no number behind it)
- **Implemented in:** `core/include/doseph/watchdog.hpp`
- **Tests:** `tests/core/test_watchdog.cpp` "Watchdog: neutral with a reason during the start, while stopped and without a recipe (RAT-005, RAT-073)"

### RAT-006 – A missing value is never 0

- **Rule:** A missing value is shown as "–" and treated as missing everywhere: in planning, in the cycle, in the history and in the web app. It never becomes 0, and a calibration point is only ever taken from a fresh raw reading.
- **Why:** Most controllers read 0 as the strongest possible demand: coldest, driest, empty, no CO2. A dead sensor that reports 0 therefore drives its outputs at full power, and since 0 is a valid value, no warning appears. A calibration point taken from a missing reading is worse than none, because later readings look plausible while being wrong.
- **Evidence:** Found on the reference installation (2026) in a review of every place that read a sensor: a missing reading passed on as 0 would have kept a water heater, a humidifier and CO2 dosing on and booked a full tank as consumed; one missing raw reading was stored as a 0 V calibration point.
- **Implemented in:** `core/include/doseph/common.hpp`, `core/include/doseph/plan.hpp`, `core/include/doseph/truth.hpp`, `core/src/robot.cpp`
- **Tests:** `tests/core/test_config.cpp` "Configuration: there and back through JSON; a missing number stays missing (R5, RAT-006)"; `tests/core/test_truth.cpp` "Sensor truth: no reading is never 0, and a probe without calibration gives no value for control (RAT-006, RAT-025)"; `tools/arch_check.sh`

### RAT-007 – After a restart everything is off; timed actuators have an independent cut-off

- **Rule:** After a crash or restart every dosing channel is off and the feed pump is off; nothing resumes on its own (R6). A dosing run is limited to 60 s and the device driver cuts a pump off on its own after 90 s.
- **Why:** Timers held in volatile memory are lost on a restart or update, and the "off" they were waiting for never comes. Each timed actuator therefore also has a cut-off that works on its own, set above its longest valid run so it never ends a valid one.
- **Evidence:** Observed on the reference installation (2026): many timed sequences kept their off command only in volatile memory, so a restart could leave a pump, valve, heater or dimmer in its last state. Independent cut-offs, such as 90 s for dosing pumps, closed that gap.
- **Implemented in:** `core/include/doseph/journal.hpp`, `core/src/robot.cpp`, `sim/world.cpp`
- **Tests:** `tests/core/test_cycle.cpp` "Cycle: a restart while a channel doses is reported; the next check mixes first and learns nothing (SD-106, R6, RAT-007)"

### RAT-008 – A tolerance is never 0

- **Rule:** A tolerance is never 0: the EC and pH tolerances are at least 0.05 and are refused below.
- **Why:** A band of zero width is never met by a real reading, so the check fails all the time. An alarm that never clears hides the alarms that matter. A tolerance of 0 is therefore treated as a configuration error and refused.
- **Evidence:** Observed on the reference installation (2026): tolerances of 0 kept the watchdog permanently red. Working values were about 1 K for air temperature, 5 % for humidity, 100 ppm for CO2 (above the sensor accuracy of about ±50 ppm) and 0.2 for pH and EC alarms, while the mixing loop stopped at the tighter 0.1 pH and 0.05 EC.
- **Implemented in:** `core/include/doseph/config.hpp`, `core/src/config.cpp`, `core/src/cycle.cpp`
- **Tests:** `tests/core/test_config.cpp` "Configuration: a tolerance of 0 is refused (RAT-008); settings only within their bounds (R7)"

### RAT-009 – Climate targets keep VPD in a corridor

- **Dropped:** the grow controller only (room climate); not part of the mixing robot (SD-105).
- **Rule:** The default climate targets are a day temperature of 26 °C in the vegetative phase and 27 °C in flowering, with 70 % relative humidity (about 1.0 kPa VPD) and 62 % (about 1.35 kPa). When the climate is led by VPD, the humidity setpoint follows from the VPD target and the target temperature, not the measured one.
- **Why:** Extra light or CO2 pays off only when temperature and humidity keep up: with more CO2 the best leaf temperature rises, and at a high VPD the leaves close their stomata and take up no more CO2. Keeping VPD in a corridor of about 1.0–1.4 kPa avoids water stress as well as wasted enrichment.
- **Evidence:** Observed on the reference installation (2026): day temperatures of 26–27.5 °C in the phases with CO2 enrichment, humidity set for a VPD of 1.0–1.4 kPa. In the last weeks of flowering the corridor was left on purpose (50 % RH, about 1.63 kPa) to lower the risk of mould, accepting a slightly smaller yield. CO2 enrichment there also followed light intensity; without CO2 dosing only the temperature and humidity targets carry over.
- **Later:** step 9 (climate).

### RAT-010 – Irrigation is controlled by the number of shots, not the shot size

- **Rule:** Each watering shot stays small; the daily amount comes from the number of shots. A circuit's schedule of shots in a daily window spreads its shots evenly from the window's start to its end, each of the same length.
- **Why:** Rock wool cannot take up a large shot at once: the water finds a channel and leaves at the bottom, so the drain looks right while parts of the block stay dry. Few large shots also leave long gaps in which the solution in the block concentrates. The count therefore shapes moisture over the day, and the duration sets the amount.
- **Evidence:** Observed on the reference installation (2026): the first settings gave shots of 120–200 ml on a 650 ml block, 18–31 % of its volume at once. Shots were then fixed at about 130 ml (about 20 %, 120 s), with 6–9 a day in flowering. Freshly rooted plants got the opposite on purpose: 4 short shots of 36 s, so the block dried in between and the roots grew downwards, since a block that stays wet there risks root rot.
- **Changed:** A schedule can also be fixed times, an interval or by hand (SD-134). The first-shot factor is dropped. The drain percentage, which set the shot length, waits for a drain measurement (RAT-014). A circuit with a time window may follow a plant, whose current phase then sets the number of shots and their length (SD-134).
- **Implemented in:** `core/src/watering.cpp`
- **Tests:** `tests/core/test_watering.cpp` "Watering: shots in a window are spread from its start to its end; fixed times and an interval give their own (RAT-010, SD-134)"; `tests/core/test_watering.cpp` "Watering: a schedule waters at its times, one circuit after the other; a paused one rests (SD-134, RAT-010)"

### RAT-011 – Top-up mixing doses on the fresh water only

- **Dropped:** the grow controller only (mixing into fresh water in a bucket or watering can); not part of the mixing robot (SD-105).
- **Rule:** When fresh water is added to solution left in a vessel, the recipe's amounts in ml/L are applied to the fresh litres only, never to the whole volume. The whole volume only checks that it fits; without a known amount of fresh water nothing is planned.
- **Why:** What is left already holds its share of nutrients, so dosing on the full volume overdoses at every top-up. A step driven by EC can also take its target as reached, since the residue lifts the EC it starts from.
- **Evidence:** Observed on the reference installation (2026): with 20–25 L left in the tank, fixed-rate additives dosed on the whole volume came out 25–50 % too high each day. A calcium/magnesium step driven to a base EC of 0.4 mS/cm started at about 1.3 mS/cm (20 L at 3.0 plus 27 L of fresh water), counted its target as met and skipped itself, so the fresh water got none. The installation fell back to the whole volume when the fresh litres were unknown; refusing to plan is the stricter way (RAT-006).
- **Later:** not planned (mixing in a bucket or watering can is no part of the robot, which doses towards the measured tank). A refill valve at step 9 needs only the count of fresh litres, for fixed additions (RAT-085).

### RAT-012 – EC can only be raised

- **Rule:** The robot can only add nutrients. An EC above its target is left as it is and named with the reason ("fresh water lowers it"); the cycle does not report it as reached.
- **Why:** Dosing only adds ions, so a target below the present EC cannot be reached. Unless the app says so, mixing reports success while the tank stays too strong, right in the phase that called for less.
- **Evidence:** Calculated on the reference installation (2026): when a phase change lowered the EC target from 3.0 to 2.0 mS/cm, the solution left in the tank was already above the new target, yet the mixing step would report the target as reached. A later lesson there: a drain must be followed directly by a refill, or the day's irrigations run without water.
- **Implemented in:** `core/src/cycle.cpp`
- **Tests:** `tests/core/test_cycle.cpp` "Cycle: EC above its target and pH below it are left as they are, with the reason (RAT-012, RAT-037, RAT-041)"

### RAT-013 – Dimming never goes below the lamp's switch-on threshold

- **Changed:** Dropped for the mixing robot alone (SD-105), taken up again with light (SD-135). The threshold is a setting of each light circuit, and the ramps of sunrise and sunset never go below it.
- **Rule:** A dimming value below the lamp driver's switch-on threshold is raised to the threshold; 0 means off and switches the light relay off as well (RAT-029). The threshold is a setting of the light schedule.
- **Why:** An LED driver lights only above a minimum dimming level and goes dark again a little below it. Between the two, relay and dimmer both look right while the lamps stay dark, and no check notices. The threshold belongs to the driver and its dimming line, so it is measured again whenever either changes.
- **Evidence:** Measured on the reference installation (2026): a first threshold of about 10 % turned out to come from a wiring fault that held the 0–10 V dimming line at about 7 V. Correctly wired, the lamps came on at 9 % and went out at 6 %.
- **Implemented in:** `core/src/lighting.cpp`
- **Tests:** `tests/core/test_lighting.cpp` "Light: a day from its start for its hours; a dimmer rises over its sunrise and falls over its sunset, never below the driver's threshold, and is off at night (SD-135, RAT-013, RAT-029)"; `tests/core/test_lighting.cpp` "Light: the sunrise raises the brightness from the driver's threshold, and night switches the lamps off (SD-135, RAT-013, RAT-029)"

### RAT-014 – Drain percentage leads irrigation; substrate readings inform only

- **Dropped:** the grow controller only (drain-to-waste irrigation); not part of the mixing robot (SD-105).
- **Rule:** The drain percentage – run-off as a share of what was given – leads watering. Substrate readings do not replace it: until there is data behind them they are recorded and shown, with no thresholds and no alarms.
- **Why:** The drain percentage measures watering directly and is an established measure. One substrate probe sees one spot; controlling on it needs a data basis first, and until then it should neither act nor alarm.
- **Evidence:** Observed on the reference installation (2026): the drain had no EC measurement, and a weighed drain container stayed the control input. A single substrate probe (moisture, temperature, bulk and pore EC) was only recorded and shown. It sat on purpose in the block that dries fastest, so a control based on it would err towards more water.
- **Later:** step 9 (watering).

### RAT-015 – No substitute values for quantities; invalid data is a problem

- **Rule:** No stand-in values for quantities: an uncalibrated pump doses nothing, a tank volume that is missing stops dosing, a reading that is not usable is a problem in the watchdog, not a neutral state. Guards test for a finite number explicitly.
- **Why:** When a run time or an amount is computed from a stand-in value, a fault turns into a dose that nobody notices. Missing data must therefore fail a watchdog check, not silence it. Comparisons do not catch NaN, since every comparison with NaN is false (NaN < 1 and NaN ≤ 0 alike), so guards test for a finite number explicitly. A counter quietly restarted at 0 later looks like a real one, so an unknown stock stays unbooked. A canister assumed full looks like a real count too: its low-stock and below-zero warnings then come late by whatever was missing, or not at all, and a mix check passes that the canister cannot cover.
- **Evidence:** Observed on the reference installation (2026): default values meant for missing readings did not take effect when a sensor was offline, and many of them were 0 anyway; a check for "volume ≤ 0" let a missing (NaN) volume pass.
- **Implemented in:** `core/include/doseph/config.hpp`, `core/include/doseph/plan.hpp`, `core/src/gateway.cpp`, `core/src/watchdog.cpp`
- **Tests:** `tests/core/test_plan.cpp` "EC round: a canister on no channel, or an uncalibrated channel, refuses the round (RAT-015)"; `tests/core/test_watchdog.cpp` "Watchdog: a reading that is not delivered is a problem, never neutral; not calibrated only while a recipe needs it (RAT-015, RAT-021)"; `tests/core/test_watchdog.cpp` "Watchdog: latches, an ended recipe and an uncalibrated channel are problems; the headline names the first and counts the rest (RAT-015, RAT-072)"

### RAT-016 – History leaves gaps empty

- **Rule:** The history stores gaps as gaps (null in the API); a series is never filled with 0 or the last value. pH, EC and temperature are recorded only while water flows through the measuring chamber.
- **Why:** The data of a finished run is the basis for comparing runs and costs. If gaps turned into zeros, averages and totals would be silently wrong and could not be told apart from real measurements.
- **Evidence:** Observed on the reference installation (2026): the detailed history was dropped after a retention period, so an export at the end of each run became the lasting record; over about a month the export had no unexplained gaps.
- **Changed:** Recording only with flow is new: a standing measuring chamber does not measure the tank.
- **Implemented in:** `core/src/robot.cpp`, `core/src/api.cpp` (CSV)
- **Tests:** `tests/core/test_api.cpp` "API: history of ph, ec, temp and flow only; values while flow is proven (RAT-016)"; `tests/core/test_api.cpp` "API: the history as CSV in the page's format; a gap stays an empty field (RAT-016)"; `web/e2e/robot.spec.ts` "history downloads as CSV in the page's format (RAT-016)"

### RAT-017 – VPD is air VPD, with no value when an input is missing

- **Dropped:** the grow controller only (room climate); not part of the mixing robot (SD-105).
- **Rule:** VPD is air VPD, from air temperature and relative humidity with the FAO-56 saturation vapour pressure: VPD = es(T) · (1 − RH/100), with no leaf offset. With an input missing or not valid, or two readings of the device's own sensors more than 60 s apart, there is no VPD (a gap), never 0. A sensor that reports only when its value changes has no age by its report time, so for it the 60 s pairing gives way to each input's own freshness. A leaf offset may come later, shown as "estimated".
- **Why:** How much cooler a leaf is than the air under LED light depends on light, air movement and leaf angle. Subtracting a fixed amount would pass a guess off as a measurement, the same mistake as a default standing in for a reading. The phase targets are air VPD anyway.
- **Evidence:** Observed on the reference installation (2026): a cross-check at 27 °C and 62 % RH gave 1.352 kPa against 1.36 kPa worked out by hand. A run through failure cases (an input missing or offline, implausible humidity, light off) gave no 0 in any of them. Leaf VPD under LED was put at roughly 0.1–0.2 kPa below air VPD, varying. The 60 s limit is an assumption.
- **Later:** step 9 (climate).

### RAT-018 – No parallel dosing sequence; time limits sit above the longest legitimate run

- **Rule:** One dosing run at a time: the gateway refuses a run while another is active. A run is at most 60 s; a run that does not report back in time is switched off and booked as failed.
- **Why:** A loop may only dose again once the previous dose is fully in the reading, which takes at least two sensor smoothing windows plus the mixing time, so a whole sequence can run long. A hung-job timeout shorter than that would release the lock during a valid run and let a second dosing sequence start alongside it, the worst possible result of a safety check.
- **Evidence:** Observed on the reference installation (2026): pH and EC readings were 5-sample moving averages at 15 s intervals, a 75 s window. Settle times of 90–120 s caused overshoot and were raised to 240–260 s. The worst-case sequence grew from about 35 to about 95 minutes, so the limit for a stale sequence had to rise from 90 to 150 minutes to keep the lock. The 60 s run cap and the 90 s device cut-off stayed unchanged.
- **Implemented in:** `core/src/gateway.cpp`
- **Tests:** `tests/core/test_journal.cpp` "Gateway: no dose without flow; one that loses its flow stops at once; one run at a time (SD-105, RAT-018)"

### RAT-019 – Network sockets fail safe on their own

- **Rule:** When the robot takes over the output of a smart plug or relay it sets "off after power loss" and "never on by itself", keeps the device's own input from switching it, and for a pulse role writes an auto-off timer into the device a little above the software's longest run (1.11 ×, rounded up to a whole minute). The output may switch only once these settings were read back, and they are read back again before every switch-on: once more written where they differ, and no switch-on where they still differ. Loads that run for hours (light, a circulation pump) get no auto-off.
- **Why:** If the controller fails while a valve or pump is on, nothing switches it off any more – only a timer inside the device still acts. A device's settings can also be lost or changed, by a factory reset or in the maker's own app, so the robot reads them back instead of trusting them.
- **Evidence:** Observed on the reference installation (2026): at first no smart relay had a fail-safe of its own. Device timers were then set, for example 90 s for the dosing pumps (above the 60 s software cap), 300 s for the CO2 valve and 1800 s for the inlet valve and the watering pump; loads running for hours got none, as a timer would cut the light in the middle of the day. A later read-back of every device found two that still restored their last state after a power loss and one planned auto-off that had never been set. The factor 1.11 for pulse roles is an assumption.
- **Implemented in:** `core/src/devices.cpp`, `core/src/gateway.cpp`
- **Tests:** `tests/core/test_devices.cpp` "Devices: a Shelly output gets off after power loss, never on by itself, not switched by its own input, and a pulse role an auto-off at 1.11 times its longest run (SD-132, RAT-019)"; `tests/core/test_devices.cpp` "Devices: a Shelly relay bound as the refill valve gets its settings written and read back; it switches on only after a check, off at once, and again while it says on (SD-132, RAT-019)"

### RAT-020 – pH dosing only on plausible readings, and only while it has an effect

- **Rule:** pH is dosed only on a usable reading (calibrated, fresh, plausible, not frozen, no jump lock), and only while it has an effect: two pH− doses in a row that lower pH by less than 0.03 lock pH dosing until a person releases it.
- **Why:** A finite number is not automatically a valid measurement. A damaged probe can report a stable but meaningless value, and a loop that trusts it keeps adding acid. A dose without a measurable effect points to an empty canister, a blocked hose, a failed pump or a faulty probe, and dosing more fixes none of these.
- **Evidence:** Observed on the reference installation (2026): a pH probe damaged by storage in distilled water read 10.08. As a finite number it passed the earlier check, and up to 160 ml of pH-down (8 attempts of 20 ml) could have gone into the tank on that reading.
- **Implemented in:** `core/include/doseph/cycle.hpp`, `core/src/truth.cpp`
- **Tests:** `tests/core/test_truth.cpp` "Sensor truth: stale and implausible; standstill of the raw value apart from freshness (RAT-020, RAT-023, RAT-027, RAT-059)"

### RAT-021 – Sensor failure and missing calibration are separate findings

- **Rule:** A sensor that fails and a sensor that is not calibrated are separate findings: "not calibrated" is a problem only while a recipe in force needs the value.
- **Why:** If both share one status, a known setup gap becomes a permanent alarm that buries real failures, and a real outage can hide behind a mere configuration hint.
- **Evidence:** Observed on the reference installation (2026): the working but never calibrated water-level sensor of the humidifier would have kept the sensor check red from the first switch-on, i.e. a constant push alarm; a scale with a zero calibration factor was dropped silently and appeared in neither list.
- **Implemented in:** `core/src/watchdog.cpp`
- **Tests:** `tests/core/test_watchdog.cpp` "Watchdog: a reading that is not delivered is a problem, never neutral; not calibrated only while a recipe needs it (RAT-015, RAT-021)"

### RAT-022 – Acknowledging an alarm only pauses its repetition

- **Rule:** Acknowledging an alarm only pauses the reminders about it; the status stays red. At night (22:00–07:00) only critical alarms are pushed, repeated after 5, 15 and 60 min; lesser findings wait for a morning report or stay in the app.
- **Why:** An acknowledgement that cleared the red status would quietly switch monitoring off. The pause is therefore bounded: it ends after a set time (12 h on the reference installation), when all checks are fine again, and at once when another problem appears. Lesser findings are not repeated, since hourly reminders about them would drown the alarms that need someone.
- **Evidence:** —
- **Implemented in:** `core/include/doseph/alarms.hpp`, `core/src/alarms.cpp`
- **Tests:** `tests/core/test_alarms.cpp` "Alarms: a critical alarm is pushed at night too and repeated 5, 15 and 60 min after, then no more; a warning by day once (RAT-022)"; `tests/core/test_alarms.cpp` "Alarms: at night a warning waits for the morning report when the quiet hours end; a note goes only there; the report comes once a day (RAT-022)"; `tests/core/test_alarms.cpp` "Alarms: acknowledging pauses the repetition, never the alarm; the pause ends after 12 h with a new round, at once when another problem appears, and with the problem (RAT-022)"; `tests/core/test_scenarios.cpp` "Scenario alarms: a leak is a critical alarm, pushed at once and repeated; acknowledging pauses the reminders and keeps the alarm; quiet hours are set and checked (SD-123, RAT-022)"

### RAT-023 – Sensor truth always runs; freshness and frozen raw values are checked separately

- **Rule:** Sensor truth runs all the time, also while nothing is dosed. Freshness and a standing raw value are checked separately.
- **Why:** A sensor that fails between runs is cheapest to fix right then; monitoring that waits for the next run finds it only on the start day. Standstill is judged on the raw signal, because calibrated, rounded values may rightly stay the same for hours, while the raw signal of a working probe always shows some noise.
- **Evidence:** Observed on the reference installation (2026): the history stored a new time only when the value changed, so the age of the last entry could not tell a sensor repeating the same value from one that had stopped.
- **Implemented in:** `core/include/doseph/readmodel.hpp`, `core/include/doseph/truth.hpp`, `core/src/truth.cpp`
- **Tests:** `tests/core/test_truth.cpp` "Sensor truth: stale and implausible; standstill of the raw value apart from freshness (RAT-020, RAT-023, RAT-027, RAT-059)"

### RAT-024 – "Not present" is a finding, not a resting state

- **Rule:** A measurement role with no device assigned has a state of its own, "not assigned": it never carries a value and is never used for control. Once a role is assigned, a device that disappears or stops answering is "offline", and the watchdog counts that as a problem.
- **Why:** A sensor can vanish without any loud failure – renamed, replaced or removed. If "missing" counts as harmless, monitoring stays green while an input is gone.
- **Evidence:** Observed on the reference installation (2026): sensors that disappeared were not counted as faults, so the overall health stayed green with a sensor missing.
- **Implemented in:** `core/src/devices.cpp`, `core/src/robot.cpp`, `core/src/watchdog.cpp`
- **Tests:** `tests/core/test_devices.cpp` "Devices: the float switch from a Shelly input, ESPHome or MQTT; offline, stale or unreadable it has no value (SD-132, RAT-024, R5)"; `tests/core/test_devices.cpp` "Devices: a device away is offline; back after a power loss its settings are checked again; a restart of the robot switches its outputs off (SD-132, RAT-024, R6)"

### RAT-025 – Control reads only validated values; no valid calibration, no control value

- **Rule:** The cycle reads only values that sensor truth marks usable. Without a valid calibration a value is shown but never used for a dose; a reading is never clamped into range.
- **Why:** Clamping turns a defect into a plausible-looking number, and a frozen or invalid value that is still numeric passes every simple guard. Control must stop on its existing safe path instead of acting on such values.
- **Evidence:** Observed on the reference installation (2026): clamping turned a 3.5 V raw signal, i.e. a defect, into a clean-looking pH 0, and four control loops (water heater, humidity, CO2, nutrients) had no check against frozen or stale readings.
- **Implemented in:** `core/include/doseph/common.hpp`, `core/include/doseph/config.hpp`, `core/include/doseph/readmodel.hpp`, `core/include/doseph/truth.hpp`, `core/src/truth.cpp`
- **Tests:** `tests/core/test_truth.cpp` "Sensor truth: no reading is never 0, and a probe without calibration gives no value for control (RAT-006, RAT-025)"; `tests/core/test_truth.cpp` "Sensor truth: pH from two buffers; a slope or zero point out of band is refused (RAT-025, RAT-026)"; `tests/core/test_api.cpp` "API: a backup restores the settings, checked like a change; never the password lock, and learning only where kept (RAT-025, RAT-085)"

### RAT-026 – Validity limits come from the calibration, not from a guess

- **Rule:** Validity limits come from the calibration: a pH calibration is refused when its slope lies outside 0.8–1.25 of the ideal or its zero point more than ±30 mV off; an EC calibration when its factor lies outside 0.5–2.
- **Why:** A limit set by feel is either too loose or too tight. Too loose is the dangerous side: a broken probe then reads like clean water and dosing goes on. Too tight only gives a false alarm that stops dosing, so that side is accepted; the answer to such an alarm is to check the probe and its zero point, and the limit moves only when a new calibration gives a new number.
- **Evidence:** Measured on the reference installation (2026): with a three-point EC calibration, the calibrated value at the probe's zero point in distilled water was −0.0159 mS/cm, so the lower limit became −0.02 mS/cm instead of an estimated −0.05 mS/cm; the remaining margin was about 0.6 mV of raw signal.
- **Implemented in:** `core/src/truth.cpp`
- **Tests:** `tests/core/test_truth.cpp` "Sensor truth: pH from two buffers; a slope or zero point out of band is refused (RAT-025, RAT-026)"

### RAT-027 – A sensor counts as failed only after several missed readings, judged by its last measurement

- **Rule:** A signal is stale only after it missed its updates for longer than its limit (pH and EC 60 s, temperature 120 s, flow 15 s), judged by its last measurement, never by a change of value.
- **Why:** A single missed reading is usually a short communication glitch; alarming on it produces false alarms. Requiring several misses still detects a real defect quickly. Judging by a change of value instead hides a signal that falls silent while its device keeps running, and makes a calm, healthy sensor look old.
- **Evidence:** Observed on the reference installation (2026): first, with freshness judged only per device and sensors reporting every 30 s, a limit of 5 min separated real defects from wireless dropouts, and six missed reports were too close to ordinary dropouts. Later the installation sent a sign of life per signal every 15 s: in a 14-day replay with real data, a water temperature read every 30 s was never stale at 60 s, and the other channels stayed between 0 and 20 s old.
- **Implemented in:** `core/src/truth.cpp`
- **Tests:** `tests/core/test_truth.cpp` "Sensor truth: stale and implausible; standstill of the raw value apart from freshness (RAT-020, RAT-023, RAT-027, RAT-059)"

### RAT-028 – After a restart, outputs fall back to off but recorded facts survive

- **Changed:** Light (SD-135) follows its plan after a restart: on by day, off at night. A hand switch is not resumed.
- **Rule:** After a restart the outputs fall back to off, while recorded facts survive: latches, jump locks, learned effects, the Stop, a new tank and an open dose in the dosing journal (SD-106).
- **Why:** Outputs and recorded facts need opposite handling after an unexplained reset. An output is safest off, and switching it back on by itself is a hazard. A recorded fact (a lock is set, a calibration is valid, a run is active) has to come back, because losing it quietly releases a lock or ends a run.
- **Evidence:** Observed on the reference installation (2026): stored on/off facts such as "calibration valid" and "run active" could be lost without any error message, while numeric settings were kept.
- **Implemented in:** `core/src/robot.cpp`
- **Tests:** `tests/core/test_config.cpp` "Run-time state: latches, jump locks and learned effects survive; a damaged one throws (RAT-028, RAT-063)"

### RAT-029 – Dimming to zero also switches the light relay off

- **Changed:** Dropped for the mixing robot alone (SD-105), taken up again with light (SD-135): a dimmer's "on" and its brightness are one Shelly component, so 0 % switches it off.
- **Rule:** Dimming to 0 means off, and the light relay then switches off too, so relay state and light agree; a value between 0 and the switch-on threshold is raised to the threshold (RAT-013).
- **Why:** With the relay left on and the dimmer at zero, "relay on" and "lamp lit" disagree: a check based on the relay raises a false critical alarm for light in the dark period, and only power measurement shows whether a lamp really shines.
- **Evidence:** Measured on the reference installation (2026): with the relays on and the dimmer at 0 V the lamps drew 0–1.3 W and gave no light. A check on power (alarm above 10 W) rightly stayed quiet, while one on the relay state raised a false "light in the dark period" alarm all night.
- **Implemented in:** `core/src/lighting.cpp`, `core/src/gateway.cpp`
- **Tests:** `tests/core/test_lighting.cpp` "Light: a day from its start for its hours; a dimmer rises over its sunrise and falls over its sunset, never below the driver's threshold, and is off at night (SD-135, RAT-013, RAT-029)"; `tests/core/test_lighting.cpp` "Light: the sunrise raises the brightness from the driver's threshold, and night switches the lamps off (SD-135, RAT-013, RAT-029)"

### RAT-030 – Refill supervision respects the probe's blind zone and judges the rate over a window

- **Dropped:** the grow controller only (tank refill); not part of the mixing robot (SD-105).
- **Rule:** Near the bottom of the tank the level probe has a blind zone in which its reading is no measurement: the rate and standstill checks of a refill are not armed there, and a time limit catches a dead valve instead. Above it, the fill rate is judged over a window of at least 5 min against a tolerance, never one reading against the next.
- **Why:** A smoothed and rounded level does not climb evenly during a fill; judged reading by reading, its normal dips look like a stalled inlet and end good fills. In the blind zone the probe shows no change at all, so only a time limit can catch a dead valve there.
- **Evidence:** Observed on the reference installation (2026): the level still read 0.0 L when about 4 L had already flowed in, and a rate check on readings 60 s apart cut a good fill short at 2 L. The blind zone's time limit was twice the time worked out to cross it plus 2 min – about 9 min for 5 L at 1.44 L/min; a later 60 L drum had a blind zone of 1 L.
- **Later:** step 9 (refill valve and level sensor).

### RAT-031 – No automatic restart after a refill fault

- **Rule:** When the refill valve is closed for a fault – the float switch did not say full within the longest fill, the float lost its value during a fill, or the valve stopped answering while it was open – the refill latches, with its reason and time. No fill opens until a person has looked and released it, and the valve stays closed meanwhile. Where the refill is set to report only (SD-133), the fault alarms once and holds nothing.
- **Why:** An automatic restart comes exactly when the cause is still unknown – a known cause would have been fixed. Letting water run unattended then risks a flood; the safe way is a person who starts the refill and watches it.
- **Evidence:** —
- **Changed:** A float switch takes the place of the level reading (SD-132), so the faults are the ones a float can show, plus a valve that stops answering. A start whose saved state cannot be read already stops the whole robot (RAT-063) and sets no latch of its own here. A fill after a release does not wait for a reading to settle: a float has none.
- **Implemented in:** `core/src/refill.cpp`, `core/src/robot.cpp`
- **Tests:** `tests/core/test_refill.cpp` "Refill: a fill that reaches its longest time without "full" closes the valve and latches the refill; no fill opens until a person releases it (RAT-031, RAT-079)"; `tests/core/test_refill.cpp` "Refill: a float that loses its value during a fill closes the valve and latches the refill (RAT-031, R5)"; `tests/core/test_refill.cpp` "Refill: a valve that stops answering while it is open latches the refill (RAT-031)"; `tests/core/test_refill.cpp` "Refill: set to report only, a fill that ends without "full" is reported and holds nothing (SD-133, RAT-031)"

### RAT-032 – The inlet's emergency limit follows the valve, not the job

- **Rule:** Whenever the refill valve is to be open – by the refill, by hand or any other way – the gateway keeps it closed unless the float switch says "not full", closes it when the float says full or loses its value, and closes it once it has been open for the longest fill. A valve closed so opens again only when it is wanted anew.
- **Why:** An overflow guard that lives only in the refill routine leaves a valve opened any other way without supervision, and the switching device's own auto-off may act too late.
- **Evidence:** Calculated on the reference installation (2026): a valve opened by hand had no level supervision, only the device's 30 min auto-off. The inlet delivered about 43 L in 30 min, so a fill starting at 120 L would have overflowed the 150 L tank before the auto-off acted.
- **Changed:** The float switch takes the place of the level reading and the tank's capacity (SD-132).
- **Implemented in:** `core/src/gateway.cpp`
- **Tests:** `tests/core/test_refill.cpp` "Refill: the gateway opens the valve only while the float says "not full", closes it at full or past the longest fill, whatever wants it open; closed so, it opens only when wanted anew (RAT-032)"

### RAT-033 – Narrow control band, wider alarm band

- **Rule:** The alarm band is wider than the control band: the watchdog reports EC or pH as a problem only beyond the control tolerance plus 0.2.
- **Why:** With one shared limit, every normal control step also shows up as an alarm, and people learn to ignore alarms. The alarm is meant for a controller that cannot hold its target, so it sits outside the control band.
- **Evidence:** Observed on the reference installation (2026): with control and alarm both at ±1 K for air temperature, each control step in a test night also appeared as an alarm. The bands were then split: ±1.0 K for control and ±2.0 K for the alarm on air temperature, ±3 % and ±7 % on relative humidity.
- **Implemented in:** `core/include/doseph/watchdog.hpp`, `core/src/watchdog.cpp`
- **Tests:** `tests/core/test_watchdog.cpp` "Watchdog: the alarm band is wider than the control band (RAT-033)"

### RAT-034 – Humidifier and dehumidifier never together; compressor protection

- **Dropped:** the grow controller only (room climate); not part of the mixing robot (SD-105).
- **Rule:** The gateway never runs humidifier and dehumidifier at the same time; when the other device's state is unknown (its plug does not answer), the device stays off. A compressor dehumidifier switches on again only 5 min after it was switched off, also after a Stop or a restart; switching off is always allowed.
- **Why:** The two work against each other: both at once waste energy, and switching between them too fast makes them chase each other. A compressor needs a few minutes to equalise pressure before it restarts; only a quick restart harms it, so a switch-off for safety is never delayed.
- **Evidence:** Observed on the reference installation (2026): in one test night humidity swung four times between 62 % and 85 % around a target of 70 ± 5 %, because a lock meant for switching on also delayed switching off and so forced a minimum run. The power trace showed a compressor unit: about 3 min of fan only at 32 W, then the compressor starting and rising from 315 W to 420 W. The installation also locked the opposite device for 10 min after each switch-off, ran the dehumidifier at least 10 min and pulsed the humidifier in 30 s steps; those timings are open until the climate function is built.
- **Later:** step 9 (climate).

### RAT-035 – Light ramps of 15 minutes inside the light period

- **Dropped:** the grow controller only (light); not part of the mixing robot (SD-105).
- **Rule:** Sunrise and sunset ramps last 15 min by default and lie inside the light period. A ramp is worked out from the time of day, so it keeps its set length and goes on correctly after a restart.
- **Why:** Light before or after the light period lengthens the day the plants see, since they respond to dim light too, which matters in flowering. Without ramps the climate control meets the lamps' full heat at once. Short ramps inside the period cost little light. Stepping the dimmer minute by minute did not keep the set length.
- **Evidence:** Observed on the reference installation (2026): a sunrise set to 30 min was over in 9 min, because per-minute steps were rounded to whole percent. Calculated there: 15-min ramps inside the window lose at most 1.7 % of the daily light integral in flowering (about 0.7 mol/m²/d), against about 5 % (about 2.1 mol/m²/d) with 30- and 60-min ramps; the lamps' heat load was about 500 W.
- **Later:** step 9 (light).

### RAT-036 – Stop is an active, idempotent shutdown

- **Rule:** Stop is active and idempotent: it switches every dosing channel and the feed pump off, books what ran and holds until Resume. While stopped nothing runs, not even a calibration into a cup.
- **Why:** If control loops merely stop issuing new commands, a stop freezes the last state and devices keep running unnoticed.
- **Evidence:** Observed on the reference installation (2026): a stop that only halted new commands left light, fans and the circulation pump running for hours.
- **Implemented in:** `core/include/doseph/config.hpp`, `core/include/doseph/cycle.hpp`, `core/include/doseph/gateway.hpp`, `core/src/robot.cpp`
- **Tests:** `tests/core/test_cycle.cpp` "Cycle: a Stop switches everything off at once, holds across a restart, and a resume mixes first what went in (RAT-036)"

### RAT-037 – Display follows the actual state; every block is a visible line with its reason

- **Rule:** What the robot does is shown as one line for the cycle, and everything it leaves as it is stands as a note with its reason; a note goes away with its cause.
- **Why:** A display based on requests misses devices switched some other way, and "requested but not running" is the state a user most needs to see. Blocks and control states need their own visible line, and that line has to go away with the block, or it turns into a stale message.
- **Evidence:** Observed on the reference installation (2026): the display showed the requested state, so a pump started some other way appeared as "off" while it ran, and blocks and control states had no visible line.
- **Implemented in:** `core/include/doseph/cycle.hpp`, `core/src/cycle.cpp` (`MixCycle::checks`), `web/src/pages/status.tsx`
- **Tests:** `tests/core/test_cycle.cpp` "Cycle: EC above its target and pH below it are left as they are, with the reason (RAT-012, RAT-037, RAT-041)"; `tests/core/test_cycle.cpp` "Cycle: the checklist says why the robot doses or not, and follows the state (RAT-037)"; `web/e2e/robot.spec.ts` "status says why the robot doses or not (RAT-037)"

### RAT-038 – Refill stops by time, not by the lagging level reading

- **Dropped:** the grow controller only (tank refill); not part of the mixing robot (SD-105).
- **Rule:** A refill (RAT-001) closes the valve when its computed run time is up; the level reading ends a fill early only once it is more than 1 L past the target. Without a valid inlet rate there is no refill and no stand-in rate. One fill keeps the valve open at most the allowed time minus 1 min (or minus half of it, if that is shorter), so a planned fill never ends as a fault (RAT-031); the rest follows in steps whose number is fixed at the first. Each fill waits until the level has been undisturbed for 3 min – after watering also until water draining back has stopped raising it, at most 30 min. At the end of a 10 min pause after each fill, on a level read after the valve closed, the level must have risen by at least half of what the set rate should have delivered; otherwise the inlet latches (RAT-031). A rise that cannot be judged (the watering pump on or its state unknown, no valid level, a fill cut by a restart) ends the steps, and two such fills in a row latch as well. Fills too small to judge alone (below 4 L) are added up until a rise is confirmed. A person at the tank (Stop, maintenance, the function switched off, a level calibration) ends a fill, and what flowed stays unconfirmed until the reading has moved by 1 L. While a fill waits and runs, EC and pH start no new round.
- **Why:** The level reading trails the real level because of smoothing and report intervals: stopping on it overfills, and stopping early to make up for it cuts fills short. The inflow itself stops dead when the valve closes.
- **Evidence:** Measured on the reference installation (2026): an inlet rate of 1.44 L/min (later set to 1.54 L/min) with no overrun after closing, only a lagging display; stopping on the level had overrun by 2–3 L per fill, and stopping early ended fills too soon. The installation checked the level again 120 s after closing and allowed at most two corrective runs. The 1 min margin, the 50 % threshold, the 4 L bound, the 3 min of settling and the calm level after watering are assumptions, as is a constant inlet rate during a fill; a real rate about 7 % off the set one passes the check easily.
- **Later:** step 9 (refill valve).

### RAT-039 – pH/EC safety: jump lock, hand-dose cap, no silent blocks

- **Rule:** A jump lock blocks dosing on a reading that jumped unexpectedly; it lifts after 15 min steady and survives a restart. A skipped dose is never silent: it stands as a note on the status page.
- **Why:** A disturbed probe can deliver wrong values that still lie inside the plausible range, and dosing on them is dangerous. An unbounded manual pump run can add a large amount of acid in one go. A skipped dosing that is only logged looks like a working system.
- **Evidence:** Observed on the reference installation (2026): without galvanic isolation the pH and EC probes disturbed each other; pH read 10.4–10.9 at a real value of about 2, and no check caught it. A dose skipped at pH 9.4 left its reason only in a log. Hand doses of pH-down added up to about 74 ml (93 s of pumping) in 23 L; the only limit was a 90 s device auto-off, about 70 ml in a single run.
- **Changed:** The hand dose and its cap are gone: the robot has no hand dose.
- **Implemented in:** `core/include/doseph/config.hpp`, `core/include/doseph/readmodel.hpp`, `core/src/truth.cpp`
- **Tests:** `tests/core/test_truth.cpp` "Sensor truth: a jump locks for 15 min of calm and survives a restart; an announced change explains it (RAT-039, RAT-042, RAT-044)"

### RAT-040 – Book consumption per run, hand doses included; test runs are not consumption

- **Dropped:** the grow controller only (consumption and stock of bottles (not built)); not part of the mixing robot (SD-105).
- **Rule:** A canister's stock goes down by what actually went into the tank, booked per pump run as it happens, runs that were cut included. A calibration into a cup is no consumption (RAT-070); filling a line drips into the tank and counts (SD-111).
- **Why:** If only automatic sequences book, every other run vanishes from consumption and cost figures. Test runs mostly go back into the canister and would overstate consumption.
- **Evidence:** Observed on the reference installation (2026): about 74 ml of pH− given by hand never showed up in the cost figures, because no pump booked its hand runs.
- **Later:** step 5 (maintenance reminders: a reminder to refill a canister needs its stock booked per run). The dosing journal (SD-106) already records every run into the tank.

### RAT-041 – Latch pH lowering that has no effect; no pH counter-correction

- **Rule:** pH dosing that has no effect latches (RAT-020); the robot never raises pH. A pH below its target is left as it is and named; raising it is up to a person.
- **Why:** A dose without effect points to an empty canister, a blocked line or a faulty probe; repeating it blindly risks a large overshoot once the fault clears. Alternating between lowering and raising ("ping-pong") wastes product, adds ions and can oscillate.
- **Evidence:** —
- **Implemented in:** `core/include/doseph/cycle.hpp`, `core/src/cycle.cpp`, `core/src/truth.cpp`
- **Tests:** `tests/core/test_cycle.cpp` "Cycle: EC above its target and pH below it are left as they are, with the reason (RAT-012, RAT-037, RAT-041)"; `tests/core/test_truth.cpp` "Sensor truth: a pH fall names the robot's own dose as one possible cause, and that it only lowers pH (RAT-041)"

### RAT-042 – Announced interventions explain sensor jumps

- **Rule:** An announced intervention explains jumps: a new tank explains jumps of pH, EC and temperature for 10 min, a dose the cycle made explains the change it causes while it is mixed in, and maintenance mode explains them while it lasts (at most 240 min, ended by a restart).
- **Why:** The jump lock exists to catch a misbehaving sensor. A step that follows a known action is not a fault; treating it as one blocks dosing and trains users to ignore alarms.
- **Evidence:** Observed on the reference installation (2026): taking the probes out for a buffer test made EC fall from 1.19 to 0.0 mS/cm and come back at 1.21 about 2.5 min later. The jump lock did what it should, but its alarm only reported a planned intervention.
- **Implemented in:** `core/include/doseph/robot.hpp`, `core/include/doseph/truth.hpp`, `core/src/cycle.cpp`, `core/src/robot.cpp`, `core/src/truth.cpp`
- **Tests:** `tests/core/test_cycle.cpp` "Cycle: a probe calibration holds the cycle; a dose running is cut and mixed first (RAT-042)"; `tests/core/test_truth.cpp` "Sensor truth: a jump locks for 15 min of calm and survives a restart; an announced change explains it (RAT-039, RAT-042, RAT-044)"; `tests/core/test_cycle.cpp` "Maintenance: the cycle holds and the feed pump is off, probe jumps count as the work, the targets are not judged; it ends by hand, by time or with a restart (RAT-042)"; `web/e2e/robot.spec.ts` "maintenance holds the robot until it is ended (RAT-042)"

### RAT-043 – No pH correction on an old reading

- **Rule:** No pH dose on an old reading: a reading older than its limit is not usable, and the cycle measures afresh after flushing the measuring chamber before every decision.
- **Why:** A pH value that stops updating keeps looking plausible while the tank changes. Acid dosed against it can drive the real pH far from target, so the age of the value has to show and has to stop dosing.
- **Evidence:** Observed on the reference installation (2026): for about five hours the pH reading still showed 7.01 from a buffer test, while the target check and the overall status stayed green. At that time only the pH-raising path checked the age of the reading; the lowering path would have dosed on the old value.
- **Implemented in:** `core/src/cycle.cpp`
- **Tests:** `tests/core/test_cycle.cpp` "Cycle: pH waits below the EC floor and after a nutrient dose, and is dosed only on a fresh reading (RAT-046, RAT-043)"

### RAT-044 – Jump locks survive a restart; no pH dosing on an unsecured measurement

- **Rule:** Jump locks survive a restart. After a restart the robot measures again before it doses; it never doses on a reading taken before the restart.
- **Why:** A safety lock held only in volatile memory vanishes exactly when the system restarts, possibly while the fault persists. The plausibility band alone is no protection: a disturbed reading can still look plausible.
- **Evidence:** Calculated on the reference installation (2026): after a restart had cleared the jump lock, the next automatic run would have dosed acid into a tank at about pH 4 on a disturbed reading of about 8.5. Interference from the running circulation pump put pH readings at about 7.9–12.1, and the part up to 9.0 lies inside the 3–9 plausibility band.
- **Implemented in:** `core/include/doseph/config.hpp`, `core/include/doseph/readmodel.hpp`, `core/include/doseph/truth.hpp`, `core/src/truth.cpp`
- **Tests:** `tests/core/test_truth.cpp` "Sensor truth: a jump locks for 15 min of calm and survives a restart; an announced change explains it (RAT-039, RAT-042, RAT-044)"

### RAT-045 – Short notifications

- **Rule:** A message that may become a notification later, such as a jump lock's reason, stays short: at most 200 characters with the longest values.
- **Why:** Lock screens cut long messages off, so the essential part was lost; a notification has to be readable at a glance.
- **Evidence:** Observed on the reference installation (2026): single notifications reached up to 355 characters and were cut off on the phone's lock screen. The daily report there was allowed up to 1,000 characters.
- **Changed:** Notifications are not built yet; the limit stays for when they are.
- **Implemented in:** `core/src/messages.cpp`
- **Tests:** `tools/i18n_keys.test.mjs` "a jump lock's reason fits a notification in both languages (RAT-045)"

### RAT-046 – EC gate before pH correction

- **Rule:** pH waits below an EC of 0.5 mS/cm (setting), and after a nutrient dose it waits 240 s more so the EC reading no longer holds samples from before it.
- **Why:** A glass pH electrode needs ions in the solution to read reliably; in almost pure water its value is arbitrary, and acid dosed on it is a guess. The gate uses only a valid EC, so a stale value left from the previous batch cannot open it. The rest after an EC dose makes sure the smoothed EC no longer includes samples from before that dose. With no EC dose on record there is nothing to wait for, so missing history does not block pH control.
- **Evidence:** Measured on the reference installation (2026): in reverse-osmosis water (EC about 0.04 mS/cm) two freshly calibrated pH probes differed by 2.2 pH (5.06 and 7.3); in tap water the same probes read 7.02 and 7.2. The 240 s rest was twice the 120 s smoothing window of the EC reading there.
- **Implemented in:** `core/include/doseph/config.hpp`, `core/include/doseph/cycle.hpp`, `core/src/cycle.cpp`
- **Tests:** `tests/core/test_cycle.cpp` "Cycle: pH waits below the EC floor and after a nutrient dose, and is dosed only on a fresh reading (RAT-046, RAT-043)"

### RAT-047 – Circulation dry-run protection with its own thresholds

- **Dropped:** the grow controller only (circulation dry-run protection by level; the flow proof (SD-105) takes its place); not part of the mixing robot (SD-105).
- **Rule:** The circulation pump's dry-run protection acts whoever switched the pump on, and it only ever switches the pump off. It cuts the pump below the tank's minimum level and allows it again only 0.5 L above, the same levels the pump's own control uses; it latches only when the pump was actually running (RAT-062). Dosing runs only while the circulation pump runs.
- **Why:** What the level probe can measure and when the pump may run are two separate properties; deriving one from the other either stops a pump that still draws water or claims a blind zone that measurement has ruled out. With the same off and release levels as the pump's control, protection and control never work against each other. A dose can only be judged in a mixed tank, so dosing needs circulation.
- **Evidence:** Measured on the reference installation (2026): the probe read correctly below 4 L, and the pump primed without trouble at 3 L. Raising the shared blind-zone setting to suit the pump would have slowed other checks: a failed inlet valve, for one, would have been noticed about 2.6 min later.
- **Later:** step 4 (the feed pump's dry-run latch, `docs/LATER.md`). SD-109 puts the flow proof in place of the level: three failed tries in a row latch "no flow", two with the feed pump in the tank (SD-127). Off and release levels come back only with a level sensor (step 9).

### RAT-048 – Water temperature is "not applicable" in an empty tank

- **Dropped:** the grow controller only (water heater and level); not part of the mixing robot (SD-105).
- **Rule:** With the tank level below its minimum, the watchdog rates the water temperature as neutral ("not applicable: tank empty") instead of checking its band; whether the sensor delivers still shows. The app keeps three states apart: not delivering, not calibrated, not applicable.
- **Why:** In an empty tank the probe measures air, and judging that against water targets gives findings without meaning, such as "too cold" whenever the room cools down.
- **Evidence:** Observed on the reference installation (2026): over 14 days, all five water-temperature jumps of 0.5 °C or more (out of 6,953 changes) came at 0 L, while the tank was filled or drained.
- **Later:** step 9 (water heater and level sensor). The robot's probe sits in the measuring chamber, which keeps its water without flow, and temperature is recorded only with flow (RAT-016).

### RAT-049 – Dry-run detection from pump power

- **Rule:** A pump that runs without pumping is told by what it draws: running dry it takes about half the current or power it takes moving water. The robot learns its feed pump's current while flow is proven; once the pump has moved water in a run, less than 60 % of the learned current for 2 s switches it off at once, as a try without flow (SD-127). At its start, while air clears, the window without flow protects it instead.
- **Why:** A level says whether there is water in the tank, not whether the pump moves it. Air in the pump head, a blocked intake or a valve in the wrong position leave the level unchanged. The current shows a dry run within seconds, before a missing flow would.
- **Evidence:** Measured on the reference installation (2026; a pump of about 3.4 W, three weeks of power readings): about 3.3–3.4 W pumping water and about 1.7 W running dry. 2.5 W told the two apart without false trips, also over past data, where it found a dry run nobody had noticed. Clearing air from the pump head took up to about 2 min, so the start-up grace has to be generous. Thresholds for other pumps are open; the robot's are assumptions until the bench test.
- **Implemented in:** `core/include/doseph/cycle.hpp`, `core/src/cycle.cpp`, `core/include/doseph/hardware.hpp`
- **Tests:** `tests/core/test_cycle.cpp` "Cycle: the feed pump's current is learned while it moves water; once it has, far less for 2 s is a dry run that switches it off at once as a try without flow (SD-127, RAT-049)"

### RAT-050 – Effect-based cap and minimum run time for pH-lowering doses

- **Rule:** A pH− dose is capped by its effect: at most 0.3 pH of change, at most 0.3 ml/L and at most the set maximum, planned with a start effect of 5.0 pH per ml/L; a run shorter than 1 s is not made.
- **Why:** Underestimating the acid means overdosing, and a pH overshoot cannot be undone; overestimating only costs extra rounds. Very short runs are too imprecise.
- **Evidence:** Measured on the reference installation (2026): 4.9 ml of pH− in 20.4 L of reverse-osmosis-based solution at EC 2.96 (0.24 ml/L) lowered pH by about 1.2, i.e. roughly 5.0 pH per ml/L – 7.5 times what the earlier dose formula assumed. A fixed 20 ml cap would have allowed a drop of about 4.9 pH for a 1.0 pH gap; the effect-based cap gives 1.22 ml (about 1.5 s of pumping) at 20.4 L.
- **Implemented in:** `core/include/doseph/config.hpp`, `core/include/doseph/plan.hpp`, `core/src/cycle.cpp`, `core/src/gateway.cpp`, `core/src/plan.cpp`
- **Tests:** `tests/core/test_plan.cpp` "Partial runs: equal in size, none under 1 s, no more than allowed (RAT-050, RAT-055)"; `tests/core/test_plan.cpp` "pH dose: its cap and the start effect (RAT-050)"

### RAT-051 – A dry-run lock is binding for every switching path

- **Dropped:** the grow controller only (dry-run locks); not part of the mixing robot (SD-105).
- **Rule:** A dry-run latch is enforced in one place, the gateway: a pump found running while its latch stands – whoever switched it on, by hand at the device included – is switched off again. Every other function reads the same lock and does not ask for the pump. Dosing runs only with the pump running, and a running dose stops when the pump is cut.
- **Why:** If only the protection knows the lock, every other path keeps starting the pump and the protection keeps cutting it, an endless on and off. Dosing without mixing doses against a reading that cannot see the dose.
- **Evidence:** Observed on the reference installation (2026): when the dry-run protection first went live, the circulation control and a routine running every minute kept restarting the pump, and the protection cut it within 10 s each time, because the lock was invisible to them. A lock caused only by a low level released itself there once the tank was refilled (`docs/LATER.md`).
- **Later:** step 4 (the feed pump's dry-run latch, SD-109).

### RAT-052 – Mixing and settling: pH needs much longer than EC

- **Rule:** After a dose the robot circulates before it measures again: at least one turnover, at least 2 minutes, after nutrients and after pH− alike, and the mixing clock runs only while water flows; then stability decides (SD-112). pH keeps its rest after a nutrient dose (RAT-046).
- **Why:** Dosing again before the previous dose is visible stacks doses and overshoots – irreversibly for pH. EC and pH respond very differently, so one shared waiting time does not fit.
- **Evidence:** Measured on the reference installation (2026; 20.4 L, circulation running): after nutrient doses EC reached tolerance after a median of 32 s (t63 median 26 s). After a pH-lowering dose the reading stayed flat for 99 s, about 80 s of that after the circulation pump had restarted, then fell steeply and reached tolerance after 194 s. Cutting the pH wait to about 45 s, in line with a faster sensor filter, would have let four doses in before the first one showed.
- **Changed:** The grow controller's fixed waits became the user's waits with a suggestion from volume and flow (SD-105); then one turnover as the robot's own wait, which the user may only lengthen, and pH waiting like EC (SD-112).
- **Implemented in:** `core/include/doseph/config.hpp`, `core/src/cycle.cpp`, `sim/world.cpp`
- **Tests:** `tests/core/test_cycle.cpp` "Cycle: feed pump on, flow proven, flushed, measured; a test dose first, EC before pH, mixed in with flow, measured again, then it holds (SD-105, RAT-004, RAT-052)"

### RAT-053 – EC keeps a reserve for the pH-lowering dose that follows

- **Rule:** An EC round aims below its target by a reserve for the pH− dose that follows: the expected EC rise of that dose (1.04 mS/cm per ml/L). Until the pH− effect is learned, the dose is planned at half the start effect; the reserve has no cap, but the aim never falls below the EC floor plus the EC tolerance. Once pH is within its range, the next round tops EC up to its target (SD-121).
- **Why:** Acid adds ions, and the pH step always follows the EC step, so dosing EC exactly to target and then lowering pH ends above the EC target. Ending slightly low is the safe side: a shortfall is made up by the next dose, while surplus nutrients can only be removed by diluting. Water with more alkalinity needs more acid than the start effect says, so a reserve planned at that effect is too small until the effect is learned.
- **Evidence:** Measured on the reference installation (2026): 4.9 ml of pH− in 20.4 L (0.24 ml/L) raised EC from 2.96 to 3.21 mS/cm, i.e. 1.04 mS/cm per ml/L – almost four times the effect of the nutrient pair itself (0.275). That run ended 7 % above the EC target. For a 1.0 pH gap the reserve is 1.0 ÷ 5.0 × 1.04 ≈ 0.21 mS/cm.
- **Implemented in:** `core/src/cycle.cpp`
- **Tests:** `tests/core/test_cycle.cpp` "Cycle: an EC round aims below its target by the reserve for the pH− dose that follows (RAT-053)"; `tests/core/test_scenarios.cpp` "Acceptance 50 L: tap water to the recipe and held there; pH within ± 0.2, EC within ± 0.1, an overshoot of at most 0.2 pH, every dose seen by its drop sensor (#10, step 4.9, SD-121)"; `tests/core/test_scenarios.cpp` "Acceptance 100 L: tap water to the recipe and held there; pH within ± 0.2, EC within ± 0.1, an overshoot of at most 0.2 pH, every dose seen by its drop sensor (#10, step 4.9, SD-121)"; `tests/core/test_scenarios.cpp` "Acceptance 100 L with the Pro example: a flow meter, a return pump and larger pump heads hold the recipe the same way (#10, step 4.9, SD-121)"

### RAT-054 – Paired nutrients keep their ratio at every limit

- **Rule:** The parts of a pair are scaled by one factor at every limit (round cap, run time, partial runs) and never capped or rounded per pump; a round that would need too many partial runs is made smaller for all its parts.
- **Why:** No two pumps deliver exactly the same flow. A time limit applied to each pump on its own turns the recipe ratio into the ratio of the flow rates. EC cannot show this, because it measures the total of all ions, not how they are split between the parts.
- **Evidence:** Measured on the reference installation (2026): the two pumps delivered about 52.8 and 39.6 ml/min. Capping each at 60 s gave A:B = 1.33:1 instead of 1:1 for larger doses in a 100–150 L tank; rounding A to whole millilitres turned a 9 ml dose into 5:4 (1.25:1).
- **Implemented in:** `core/include/doseph/config.hpp`, `core/include/doseph/plan.hpp`, `core/src/config.cpp`, `core/src/plan.cpp`, `sim/world.cpp`, `core/src/cycle.cpp`
- **Tests:** `tests/core/test_config.cpp` "Configuration: a pair keeps all its parts; none of them is fixed (RAT-054, RAT-085)"; `tests/core/test_plan.cpp` "EC round: 0.8 × gap, the pair scaled together, its ratio kept at every cap (RAT-054, RAT-055)"; `tests/core/test_plan.cpp` "EC round over the partial-run limit: made smaller by one factor for all parts, never refused or capped per channel (RAT-054)"; `tests/core/test_cycle.cpp` "Pairs: a round cut short with a part missing catches it up first, alone, and learns nothing from it (SD-114, RAT-054, RAT-057)"

### RAT-055 – Dose from the measured effect; large doses in equal partial runs

- **Rule:** Each round doses 0.8 × gap ÷ effect × volume. The effect starts at a fixed value (EC 0.275 mS/cm per ml/L of the recipe's scaled parts, pH 5.0 per ml/L) and is then learned from clean rounds, clamped to a quarter to four times the start. Large doses are split into equal partial runs of at most 60 s with 3 s pauses, at most 6 per canister. Two rounds in a row without effect latch.
- **Why:** Aiming at 80 % of the gap leaves room for incomplete mixing at measurement time, and an EC overshoot can only be undone by dilution. Both clamp directions yield the smaller dose. Silently shortened doses falsify the recipe. With pumps far too slow for the tank a single round rises by less than the noise, yet an empty canister or a pump that delivers nothing must still be caught: small rounds are judged together once they should have raised EC by 0.06.
- **Evidence:** Measured on the reference installation (2026): the nutrient pair raised EC by 0.275 mS/cm per ml/L in reverse-osmosis water (about 20 L). In model runs from EC 0.55 to 3.0 with 30 % of the last dose not yet mixed, aiming at the full gap ended at 3.30 and aiming at 80 % at 3.15. Before splitting, base doses cut to 60 s delivered only 64–80 % of the recipe amount in a 100 L tank.
- **Implemented in:** `core/include/doseph/config.hpp`, `core/include/doseph/cycle.hpp`, `core/include/doseph/plan.hpp`, `core/src/cycle.cpp`, `core/src/gateway.cpp`
- **Tests:** `tests/core/test_cycle.cpp` "Cycle: canisters without effect: two rounds without effect lock nutrient dosing until a person releases it (RAT-055, RAT-062)"; `tests/core/test_plan.cpp` "Partial runs: equal in size, none under 1 s, no more than allowed (RAT-050, RAT-055)"; `tests/core/test_plan.cpp` "EC round: 0.8 × gap, the pair scaled together, its ratio kept at every cap (RAT-054, RAT-055)"

### RAT-056 – A small learned effect never widens the EC cap

- **Rule:** The EC rise per round is capped (default 1.0 mS/cm), converted with the largest of the start effect, the recipe's learned effect and the effect its EC target implies; a learned value above four times the start counts as four times. A missing cap never means "no cap".
- **Why:** A learned effect that is too small – for example measured before a dose was fully mixed – would otherwise inflate both the dose and its limit. Using the larger effect keeps the cap at most where the start value puts it, and the cap is expressed in effect rather than in millilitres that ignore how strong the product is.
- **Evidence:** —
- **Implemented in:** `core/include/doseph/config.hpp`, `core/include/doseph/plan.hpp`, `core/src/config.cpp`, `core/src/plan.cpp`
- **Tests:** `tests/core/test_config.cpp` "Configuration: an EC target far from what the amounts give is refused, naming what fits (RAT-056)"; `tests/core/test_plan.cpp` "EC cap: a small learned effect never widens it, and it never goes without one (RAT-056, R5)"; `tests/core/test_plan.cpp` "EC effect of a recipe: its EC target only ever makes a round smaller; the best guess for the test dose (RAT-056, RAT-081, SD-105)"

### RAT-057 – Learn dose effects only from clean rounds

- **Rule:** A round's observed effect is learned only when nothing else changed the tank in between: no other dose, no calibration, no restart, no new tank. The test dose of a new tank learns only when it passed (SD-105). An EC round learns into the recipe it dosed, and nothing when that recipe's scaled parts change under it.
- **Why:** A second dose, a restart or a new fill falsifies the before/after comparison, and a wrong learned effect mis-sizes every later dose.
- **Evidence:** —
- **Implemented in:** `core/include/doseph/config.hpp`, `core/include/doseph/cycle.hpp`, `core/include/doseph/plan.hpp`, `core/src/cycle.cpp`
- **Tests:** `tests/core/test_cycle.cpp` "Cycle: a dose that loses its flow stops at once; without flow for 6 s the pump goes off, and what went in mixes with flow before it is measured, learning nothing (SD-105, SD-127, RAT-057)"

### RAT-058 – Waiting times cover the time to tolerance

- **Rule:** The wait after a dose has to cover the time until the reading stays within the control tolerance, with margin. The robot waits one turnover, then measures until the readings hold within their band over 60 s or a fifth of a turnover, at most 10 minutes; the user may lengthen the wait, never shorten it (SD-112).
- **Why:** The next dosing decision only changes while the remaining error is outside the control tolerance (EC 0.05 mS/cm, pH 0.1), so that is the time to measure. A t95 value mostly measures display resolution, because the slow end of the response disappears in the last digit.
- **Evidence:** Measured on the reference installation (2026): one EC response was within 0.03 mS/cm after 51 s, yet its t95 came out at 127 s. At about 20 L, EC reached tolerance after at most 47 s; with the factor 2.2 this gives about 100 s.
- **Changed:** by SD-105, the waits are the user's per tank; by SD-112 (answering #1), one turnover is the lower bound and stability over a window that grows with the turnover decides.
- **Implemented in:** `core/src/cycle.cpp`
- **Tests:** `tests/core/test_cycle.cpp` "Cycle: the mixing clock runs only with flow; the wait is one turnover, at least 2 min, which the user may only lengthen; then stable over a fifth of a turnover (SD-105, SD-112, RAT-052, RAT-058)"

### RAT-059 – Freshness and standstill are separate checks

- **Rule:** A raw value that stands still is a fault only for a signal that should be noisy (pH, EC); a rounded or quiet signal such as the water temperature is checked for freshness only.
- **Why:** A rounded or very quiet signal that does not change is not a fault; flagging it produces false alarms that teach users to ignore warnings. A sensor that stops reporting is still caught by the freshness check.
- **Evidence:** Observed on the reference installation (2026): a water temperature reported in 0.1 °C steps stayed on one value for more than 60 min twenty times in 14 days (up to 211 min) with a full tank and a working sensor; an air temperature sensor with 0.1 °C resolution stood still for up to 182 min. None of them had a gap in delivery.
- **Implemented in:** `core/include/doseph/readmodel.hpp`, `core/include/doseph/truth.hpp`, `core/src/truth.cpp`
- **Tests:** `tests/core/test_truth.cpp` "Sensor truth: stale and implausible; standstill of the raw value apart from freshness (RAT-020, RAT-023, RAT-027, RAT-059)"

### RAT-060 – Mains outlets protect themselves; no heater without an emergency shutdown

- **Dropped:** the grow controller only (mains outlets and heater); not part of the mixing robot (SD-105).
- **Rule:** When the robot takes over a smart plug it sets "off after power loss" and, for pulse and heating roles, an auto-off timer in the plug at about 1.11 × the role's maximum run time, rounded up to a whole minute; the role is bound only once both were read back. The robot itself switches off at the maximum run time, and the plug's timer is the fallback if the robot fails. A heater role is offered only once a latching emergency shutdown exists, on level, water temperature, overtemperature, missing heating effect (RAT-061), a failed sensor evaluation and run time.
- **Why:** A heater without water or with a failed sensor is a fire and scalding risk, and its protection must not depend on the controller, the network or a running grow. A timer inside the device still acts when the controller is gone.
- **Evidence:** Calculated for the reference installation (2026): a 295 W immersion heater raises 8 L of water by about 47 K in 90 min. The plug's timer there was set to 6,000 s against a 90 min software limit (factor 1.11); using that factor for pulse roles (humidifier, watering pump, inlet) is an assumption.
- **Later:** step 9 (smart plugs and a water heater).

### RAT-061 – Heater: judge the heating effect from power and water volume

- **Dropped:** the grow controller only (water heater); not part of the mixing robot (SD-105).
- **Rule:** Heating runs only through an external switched plug with a latching emergency shutdown (RAT-060). Its "no heating effect" check compares the measured temperature rise with the rise expected from heater power and water volume, minus passive losses, instead of waiting a fixed time. A second check, switched on but below 50 W after 60 s, catches the most common hardware faults within a minute.
- **Why:** At realistic volumes a fixed minimum rise within a fixed time asks for almost nothing: a heater with little output left still passes, and even a dead one shows only after the full wait. With the expected rise worked out from power and volume, a rise that is too slow is itself the finding.
- **Evidence:** Observed on the reference installation (2026): with a 295 W heater in 20 L, the old rule (0.125 K within 20 min) asked for only about 3 % of the expected rise of about 4 K. Passive loss measured there: 4.73 W/K × (water − ambient) plus 5.2 W of evaporation from the open tank (R² 0.95 over 56 hourly windows); the heating rate itself was not validated. In simulated tests the rule caught a total failure and a heater at 10 % output in 20 L after 5 min, while 70 % output did not trip it.
- **Later:** step 9 (water heater).

### RAT-062 – Switch-on lock vs. latch; latch only after an intervention, reason frozen

- **Rule:** A latch is set only after a dose had no effect or a test dose did not fit, never on its own; its reason is frozen when it is set.
- **Why:** Latching while the device is already off (e.g. an empty tank between runs) produces permanent red states and acknowledgements that seem to do nothing; a reason rewritten every cycle no longer tells what triggered the shutdown.
- **Evidence:** Observed on the reference installation (2026): when the tank was emptied with pump and heater already off, both protections latched anyway and stayed red; acknowledging had no effect while the tank was still empty, and the stored reason kept changing as the level fell. With latching limited to real shutdowns, switch-on attempts at 0 L were cut within 84–138 ms and latched, and acknowledging took effect within about 50 ms.
- **Implemented in:** `core/src/watchdog.cpp`
- **Tests:** `tests/core/test_cycle.cpp` "Cycle: canisters without effect: two rounds without effect lock nutrient dosing until a person releases it (RAT-055, RAT-062)"

### RAT-063 – Latches survive a restart

- **Rule:** Latches survive a restart. A saved state that cannot be read counts as "a latch may stand": the robot starts stopped and a person looks.
- **Why:** A restart (power failure, update) must not silently clear a protective shutdown nobody has looked at; otherwise, e.g., a pump that ran dry could be started again unattended, without a valid level showing the tank refilled and without saying so.
- **Evidence:** Observed on the reference installation (2026): one protective latch was lost whenever the control software restarted, while another was restored from stored state. A restored latch whose kind could not be determined was treated as the stricter kind, released only by acknowledgement. Restoration was verified in simulated tests, not live.
- **Implemented in:** `core/include/doseph/config.hpp`, `core/src/config.cpp`, `core/src/robot.cpp`, `core/src/watchdog.cpp`
- **Tests:** `tests/core/test_config.cpp` "Run-time state: latches, jump locks and learned effects survive; a damaged one throws (RAT-028, RAT-063)"

### RAT-064 – End-of-run summary is stored with the run

- **Dropped:** the grow controller only (grow runs); not part of the mixing robot (SD-105).
- **Rule:** At the end of a grow run a summary – water, nutrient amounts and their cost, the phases' durations, a yield entered by hand – is kept with that run's record; the yield can be added later.
- **Why:** The figures must show the state at the run's end, and what is added later must not be lost when the next run starts.
- **Evidence:** Observed on the reference installation (2026): end-of-run figures were read as the newest value overall rather than the value at the run's end, and a yield entered by hand was lost when the next run started.
- **Later:** step 6 (grow runs and harvest).

### RAT-065 – Drain on EC step-down only when needed; always on product change

- **Dropped:** the grow controller only (drain); not part of the mixing robot (SD-105).
- **Rule:** When a phase change lowers the EC target by more than 0.2 mS/cm, the app suggests draining the tank completely – unless the solution left plus fresh water ends below the new target and can be dosed up. On a change of nutrient product the drain stays. No partial draining: the robot cannot dilute (RAT-012).
- **Why:** Draining every time throws away usable solution and nutrients; mixing a new base product onto the old one is not wanted.
- **Evidence:** Calculated from the reference installation's figures (2026): stepping from EC 3.0 to 2.0, 13.4 L of residue plus 13.7 L of fresh water give about 1.55, below the new target, so no drain is needed; the drain into late flowering stays, since the diluted residue (0.84) is above the 0.8 target. One needless drain would have thrown away 34.8 L of mixed solution.
- **Later:** step 9 (drain); the advice at a phase change can come with step 6.

### RAT-066 – Phase defaults and recipe templates follow the manufacturer chart

- **Rule:** Recipe templates and their EC targets follow the manufacturer's chart; the pH target is the middle of the recipe's pH range.
- **Why:** Self-chosen EC targets drift far from what the product is designed for; pH targets on the edge of the band end up outside it because control stops about ±0.1 around the target.
- **Evidence:** Measured on the reference installation (2026): the chart implies 0.615 mS/cm per ml/L per part, equal to the mixing-tank measurement (0.62) and 12 % above a soak-water measurement (0.55). Before alignment, EC targets up to 3.0 exceeded the chart maximum (2.6 with PK, 2.3 without), so from the start of flowering 1.3–3 times the manufacturer amount was dosed. Flowering week 1 starts with the switch to 12/12 light.
- **Implemented in:** `core/data/templates.json`, `core/include/doseph/config.hpp`, `web/src/store.ts`
- **Tests:** —

### RAT-067 – Irrigation needs a minimum tank level

- **Rule:** Watering keeps a reserve in the tank: 20 % of its volume by default, never less than 10 %. A shot that would take the tank below it does not start, and water is to be topped up, with one notice until it is.
- **Why:** Watering the tank down past the point where pumps, probes and intake are covered means dry running and false readings.
- **Evidence:** Observed on the reference installation (2026): the limit equalled the circulation pump's dry-run threshold, 5 L in the first tank and 2.5 L in a 60 L drum in which every submerged part is covered from 2.5 L.
- **Changed:** The robot has no level probe of its own. It counts the litres each circuit gives (its flow times its run time) since the tank was last full, or reads a level probe where one is assigned (SD-134). The reserve is a share of the tank, not a fixed number of litres, and "top up water" opens a refill valve where there is one (SD-132).
- **Implemented in:** `core/src/watering.cpp`, `core/src/robot.cpp`
- **Tests:** `tests/core/test_watering.cpp` "Watering: the count keeps the reserve: a shot that would go below it is not made, water is to be topped up, and a new tank starts the count anew (RAT-067, SD-134)"

### RAT-068 – Irrigation pump only with a valid level above the minimum

- **Rule:** A circuit switches on only while the tank is known to hold water above its reserve: a float switch "tank empty" that says not empty, a level probe above the reserve, or the count of litres given. A float or a level without a value blocks, and a running shot stops when one of them says the tank is at its reserve. Without a float, a level or a known flow, a circuit does not water. The water's pH and EC are a second check beside it (RAT-086).
- **Why:** Otherwise the pump can run dry, and a level that cannot be read cannot show that water is there.
- **Evidence:** Observed on the reference installation (2026): watering was blocked when the level minus the planned cycle's volume fell below the minimum; with an unreadable level it watered anyway and reported it, judging a silent stop of watering the greater risk to the plants. Blocking is the stricter choice; a limited emergency watering when plants are too dry, watering without a level sensor and dry-run detection from the current draw (RAT-049) were asked for and not built.
- **Changed:** A float switch or the count can take the place of a level reading (SD-134). An unreadable level blocks, as decided there; no watering without a level.
- **Implemented in:** `core/src/watering.cpp`
- **Tests:** `tests/core/test_watering.cpp` "Watering: a float "tank empty" or a level probe keeps it from an empty tank; without a value it blocks (RAT-068, R5)"

### RAT-069 – "Supply" profile for devices with their own thermostat

- **Dropped:** the grow controller only (room climate devices); not part of the mixing robot (SD-105).
- **Rule:** A plug profile "supply" for a device that regulates itself, such as a heater with its own thermostat: off after a power loss, no auto-off in the plug, but a power limit in the plug as a net for a thermostat that sticks; the watchdog assesses it.
- **Why:** Such a device has to stay powered to do its job, so a timer in the plug would end its work a fixed time after every switch-on. A stuck thermostat does not show in how long the relay is on, but in the power the device draws, so the net is a power limit.
- **Evidence:** Observed on the reference installation (2026): such a heater's relay stayed on for weeks while its thermostat cycled – 19 heating phases above 50 W in about four weeks, the longest 6.8 min, the median 0.3 min, at most 369 W. A limit of about 450 W was proposed instead of the plug's far higher factory limit.
- **Later:** step 9 (smart plugs).

### RAT-070 – Calibration is not consumption; book actual run time per run

- **Rule:** A calibration or priming run goes into a cup and never counts as a dose into the tank; the flow rate is worked out from the run time the driver reported, not the requested one.
- **Why:** A calibration run handled like a hand dose is cut short by the hand-dose limit and counted as consumption. Dividing the cup volume by the requested time instead of the actual one then gives too high a flow rate, and all later doses come out too small. Booking only when a job ends loses whatever was pumped before an abort.
- **Evidence:** Observed on the reference installation (2026): a nominal 10-s calibration run was cut by the 5-ml manual limit after 5.7–7.8 s depending on the pump and booked as consumption; dividing the cup volume by 10 s would have overstated the rate by 1.3–1.8×. Consumption was booked only at the end of a mixing sequence, so amounts pumped before an external stop would have stayed unbooked (a gap found in review, not observed).
- **Implemented in:** `core/include/doseph/config.hpp`, `core/include/doseph/gateway.hpp`, `core/include/doseph/hardware.hpp`, `core/include/doseph/journal.hpp`, `core/include/doseph/robot.hpp`, `core/src/gateway.cpp`, `core/src/robot.cpp`
- **Tests:** `tests/core/test_cycle.cpp` "Pumps: a calibration runs into the cup and is worked out from the actual run time; a senseless result is refused (RAT-003, RAT-070)"; `tests/core/test_journal.cpp` "Dosing journal: a calibration or priming run goes into a cup, not into the tank (RAT-070)"

### RAT-071 – Minimum stock per canister: 150 ml, pH− 20 ml

- **Rule:** The watchdog reports a canister as a problem ("refill soon") when its counted level falls below 150 ml for a nutrient and 20 ml for pH−; a level below zero by the count is a problem of its own; an unknown level is not judged. A dose the counted level cannot cover is not started.
- **Why:** Refilling should not fall into a mixing run: a canister that runs dry part way through a dose quietly doses less than planned, and the round then teaches the robot a wrong effect. pH− is used in very small amounts, so it has its own, lower threshold.
- **Evidence:** Calculated from the reference installation's figures (2026): a whole run needed about 30 ml of pH− (from 2 to 114 ml) against roughly 0.7 to 3.3 L of each nutrient product.
- **Changed:** brought back with the canister level (SD-119), which counts every run of a canister's pump; a dose the count cannot cover now waits for a refill.
- **Implemented in:** `core/include/doseph/config.hpp`, `core/src/config.cpp`, `core/src/cycle.cpp`, `core/src/gateway.cpp`, `core/src/robot.cpp`, `core/src/watchdog.cpp`
- **Tests:** `tests/core/test_cycle.cpp` "Canisters: the level is counted run by run; low and used up are problems; a dose the count cannot cover waits for a refill (SD-119, RAT-071)"

### RAT-072 – Normal states are counted, only deviations are listed

- **Rule:** The watchdog counts normal states and names only deviations; the headline names the first problem and counts the rest.
- **Why:** Expected states shown as permanent individual lines bury the real problems and train users to ignore the overview.
- **Evidence:** Observed on the reference installation (2026): expected states such as "pump off – tank empty" stood as permanent single lines, and one waiting state was listed once per sensor, which filled the overview; folding normal states into one green count left only the deviations as single lines.
- **Implemented in:** `core/src/watchdog.cpp`
- **Tests:** `tests/core/test_watchdog.cpp` "Watchdog: latches, an ended recipe and an uncalibrated channel are problems; the headline names the first and counts the rest (RAT-015, RAT-072)"

### RAT-073 – Stale assessment is red; neutral when nothing can be judged

- **Rule:** An assessment older than 3 min is red ("monitoring without an assessment"); a check that cannot judge anything is neutral with its reason.
- **Why:** If the assessment itself stops, its last result would keep saying "all OK", so an old result counts as no result.
- **Evidence:** Measured on the reference installation (2026): over about 5,570 h of switched-off time on 10 outlets, no minute was above 0.5 W, and in 179 switch-offs under load the power fell to zero within one second, so a 2 W threshold separates "off" from "current flowing" with a wide margin. Run in pulses, the humidifier never stayed on longer than 1.5 min in 89 runs.
- **Implemented in:** `core/include/doseph/watchdog.hpp`, `core/src/robot.cpp`
- **Tests:** `tests/core/test_watchdog.cpp` "Watchdog: neutral with a reason during the start, while stopped and without a recipe (RAT-005, RAT-073)"

### RAT-074 – The watchdog only assesses; no path to actuators

- **Rule:** The watchdog only assesses: it reads the read model and the configuration and has no path to the pumps (R2, checked by tools/arch_check.sh).
- **Why:** Protection lives in the actuator gateway and reads sensor truth; keeping assessment separate means a monitoring bug or a disabled check can neither switch nor unblock an output.
- **Evidence:** — (design principle; the reference installation already separated assessment, which only wrote status and sent notifications, from switching)
- **Implemented in:** `core/include/doseph/watchdog.hpp`
- **Tests:** `tools/arch_check.sh`

### RAT-075 – Sensor truth always runs

- **Rule:** Sensor truth runs all the time, also while stopped, without a recipe and while a probe is calibrated.
- **Why:** Dosing interlocks read sensor truth; if validation paused, they would act on unchecked or stale values right when the tank is being worked on.
- **Evidence:** Observed on the reference installation (2026): the tank typically stands empty between runs — precisely when dry-run and heater interlocks matter.
- **Implemented in:** `core/include/doseph/truth.hpp`
- **Tests:** —

### RAT-076 – Phases provide parameters, never names

- **Rule:** A phase is a freely named set of parameters. Control, planning and assessment read only the parameters in force, never the phase's name; a check in CI refuses phase names written into the core.
- **Why:** Logic that keys on names breaks silently when users rename, add or translate phases; phases that only provide parameters keep behaviour explicit and adjustable.
- **Evidence:** Observed on the reference installation (2026): several control and monitoring paths branched on particular phase names, so behaviour hung on naming, and changing it took a rebuild.
- **Implemented in:** `core/include/doseph/plants.hpp`, `core/src/plants.cpp`, `core/src/watering.cpp`, `tools/arch_check.sh`
- **Tests:** `tests/core/test_plants.cpp` "Plants: renaming a phase changes nothing the robot does; logic reads a phase's parameters, never its name (RAT-076)"; `tests/core/test_watering.cpp` "Watering: a scheduled shot of a circuit that follows a plant runs as long as the phase says; the next phase without shots of its own gives the circuit's back (SD-134, RAT-076)"

### RAT-077 – Harvest is an event; "completed" is its own state

- **Rule:** A harvest is an event – a time and a log entry – of a running grow run. Completing the run is a separate, explicit step to the state "completed".
- **Why:** A harvest held as a state over several days mixes "plants cut" with "run still going" and leaves reports and the next run without a clear end.
- **Evidence:** Observed on the reference installation (2026): with harvest a state of several days and no state for a finished run, the end of a run could only be read from a timestamp.
- **Implemented in:** `core/include/doseph/config.hpp`, `core/src/robot.cpp`
- **Tests:** `tests/core/test_plants.cpp` "Plants over the API: a plant started from a plant recipe copies its phases, leads and ends the hand choice; the next phase, the harvest and the end are the user's; once none runs, the robot holds (SD-128, RAT-077)"

### RAT-078 – Tank level: piecewise-linear curve and switch-on hysteresis

- **Dropped:** the grow controller only (level sensor); not part of the mixing robot (SD-105).
- **Rule:** The level comes from the raw value through a piecewise-linear curve: valid from 2 points, raw values rising by at least 3 mV from point to point, volumes never falling; below the first point the first volume, above the last the last slope carried on. Outputs protected by the minimum level switch on again only 0.5 L above their switch-off level.
- **Why:** A level sensor can be non-linear near the bottom of a vessel, and a straight line through two points misreads the volume exactly where the dry-run thresholds sit. Points too close together give a slope that is mostly noise.
- **Evidence:** Measured on the reference installation (60 L drum, 2026): 1.9607 V = 0 L, 1.9871 V = 1.0 L, 2.0114 V = 2.0 L, 2.0198 V = 2.5 L, 2.1291 V = 10 L, segment slopes 37.9, 41.2, 59.5 and 68.6 L/V. A two-point line read 3.5 L at a real 2.5 L and 32.6 L at a real 36 L. The thresholds were set to 2.5 L off and 3 L on, since every submerged part is covered from 2.5 L. A check at a real 10 L still read 0.4–0.5 L high (cause unknown), so points should be checked again.
- **Later:** step 9 (level sensor).

### RAT-079 – Inlet valve: 25 min maximum, the float as the emergency limit

- **Rule:** The inlet valve stays open at most 25 min; the refill limits more tightly (15 min by default, 1 to 25 min). The device's own auto-off sits a little above the software limit (28 min: 1.11 ×, rounded up to whole minutes), so the device acts only when the controller fails. The float switch is the emergency limit all the same (RAT-032).
- **Why:** If the controller fails, the device must still close the valve, yet within the auto-off time the valve can deliver more water than the tank holds.
- **Evidence:** Observed on the reference installation (2026): after a switch to a 60 L drum (overflow at 55 L) the old emergency limit of 140 L could never be reached, and only timers and a 30 min device auto-off bounded the inlet – about 46 L per run. The limits then became 45 L, a 25 min software cap and a 25 min device auto-off.
- **Changed:** The float switch takes the place of the level-based emergency limit (SD-132).
- **Implemented in:** `core/include/doseph/config.hpp`, `core/src/config.cpp`, `core/src/devices.cpp`, `core/src/refill.cpp`, `core/src/gateway.cpp`
- **Tests:** `tests/core/test_refill.cpp` "Refill: a fill that reaches its longest time without "full" closes the valve and latches the refill; no fill opens until a person releases it (RAT-031, RAT-079)"; `tests/core/test_devices.cpp` "Devices: a Shelly output gets off after power loss, never on by itself, not switched by its own input, and a pulse role an auto-off at 1.11 times its longest run (SD-132, RAT-019)"

### RAT-080 – Mixing order is recipe data; CalMag EC effect

- **Rule:** The order of a recipe's parts is the dosing order, as the manufacturer gives it; CalMag counts with its own EC effect.
- **Why:** Manufacturers specify their own order (here part B before A, CalMag after the base, cleaner last); a fixed order in code cannot follow different products.
- **Evidence:** A manufacturer's feed program lists the order: buffer → base B → base A → PK → CalMag → cleanser. Measured on the reference installation (2026): CalMag added 0.217 mS/cm per ml/L; the chart implies about 0.22.
- **Implemented in:** `core/include/doseph/config.hpp`
- **Tests:** —

### RAT-081 – Start effects in nutrient solution are still unmeasured

- **Rule:** The start effects in nutrient solution are carried over from measurements in soak water and are still unmeasured in the tank; they err on the side of the smaller dose.
- **Why:** If the assumed effect is too large, the dose comes out too small and only costs extra rounds; if it is too small, the tank is overdosed. The buffered solution in the mixing tank should react more weakly than the soak water in which the effects were measured, so using those values there is the safe side.
- **Evidence:** Measured on the reference installation (2026): in soak water, pH− raised EC by 1.04 mS/cm per ml/L; for the nutrient solution in the mixing tank this and the other start effects were carried over unmeasured.
- **Implemented in:** `core/include/doseph/plan.hpp`
- **Tests:** `tests/core/test_plan.cpp` "EC effect of a recipe: its EC target only ever makes a round smaller; the best guess for the test dose (RAT-056, RAT-081, SD-105)"

### RAT-082 – Simulator sensor noise from measured noise

- **Rule:** The simulator's sensor noise comes from measured noise; numbers that are not measured are marked as assumptions.
- **Why:** Simulator numbers must be measured or marked as assumptions; realistic noise shows that filters, jump locks and frozen-value detection do not trip on normal fluctuation.
- **Evidence:** Measured on the reference installation (2026; 7 days, 10-min windows, medians): pH high-frequency σ ≈ 0.006 (≈ 0.014 over 10 min); EC σ ≈ 0.0003–0.0004 mS/cm; water temperature ≈ 0.02 °C; tank level ≈ 0.1 L in a 150-L tank. Noise was computed on a fixed time grid; a log that only stores changes had overstated it by a factor of 1.6–4.
- **Implemented in:** `sim/world.cpp`
- **Tests:** —

### RAT-083 – Light hours are a phase parameter, and phase guidance shows live values

- **Dropped:** the grow controller only (light); not part of the mixing robot (SD-105).
- **Rule:** The light hours per day are a parameter of each phase (typically 18 h vegetative, 12 h flowering). Guidance that explains a phase shows its duration and targets from these parameters, never as numbers fixed in the text. A phase's planned values are kept apart from the targets in force of the running phase.
- **Why:** Numbers written into help texts go stale as soon as the user changes a phase, and the guidance then contradicts what the controller does.
- **Evidence:** Observed on the reference installation (2026): the phase guide read duration and targets live from the phase parameters, so it showed even temporary test values as they were.
- **Later:** step 6 (phase parameters and their guidance); the light hours act once light comes with step 9.

### RAT-084 – A skipped pH correction does not stop irrigation, but stays visible

- **Rule:** pH outside its range, not corrected, does not block watering. The watering state shows it as a warning, with the measured pH and its target. The band of RAT-086 is wide enough that uncorrected tap water does not usually block (above pH 8.5 it does).
- **Why:** In substrate a batch with uncorrected pH does less harm than plants drying out; stopping watering does damage of its own, usually when nobody is there to step in. A skipped correction that is not shown makes the run look clean.
- **Evidence:** Found in a review on the reference installation (2026): a mix that ended without pH correction could still report a normal finish, and most ways of ending without correction gave no reason.
- **Changed:** The reason for every skipped dose is shown by the check already (RAT-037, RAT-041); watering adds its own warning.
- **Implemented in:** `core/src/watering.cpp`
- **Tests:** `tests/core/test_watering.cpp` "Watering: water is fit within a wide band: pH 4.0 to 8.5, EC at most 1.0 above its target and never above 5.0, with a recent reading and no calibration; pH not corrected only warns (RAT-086, RAT-084, R5)"

### RAT-085 – Mixing to the target EC: the recipe gives ratio and order; a fixed addition goes in per litre of fresh water

- **Rule:** A recipe gives the ratio and the order of its scaled parts; EC control scales them together to the target. A fixed part goes in per litre of fresh water and is not dosed by EC rounds.
- **Why:** Manufacturer charts scale the base nutrients to a target EC in a fixed ratio, but give additives such as a cleanser a fixed rate per volume. Scaled with an EC correction, an additive is overdosed in every round; driven by EC, its step can take a tank that is already at EC as done and skip itself.
- **Evidence:** Found in a review of pair dosing (the fixed addition is one of its safeguards); the scaling to a target EC in a fixed ratio follows the manufacturers' charts. Observed on the reference installation (2026, see RAT-011): a Ca/Mg step controlled to EC took its target as met and skipped itself, so the fresh water got none.
- **Implemented in:** `core/include/doseph/config.hpp`, `core/include/doseph/plan.hpp`, `core/src/config.cpp`, `core/src/plan.cpp`, `core/src/robot.cpp`, `web/src/api.ts`
- **Tests:** `tests/core/test_config.cpp` "Configuration: a pair keeps all its parts; none of them is fixed (RAT-054, RAT-085)"; `tests/core/test_plan.cpp` "EC round: a fixed addition is neither dosed nor scaled; the round goes to the other parts in their ratio (RAT-085)"; `tests/core/test_plan.cpp` "EC effect after an edit: scaled up by parts no longer scaled, kept only at or above the new start (RAT-085)"; `tests/core/test_api.cpp` "API: a backup is not restored while a channel runs; what the robot learned stays only where the file keeps it (RAT-085)"

### RAT-086 – Watering stops only on water clearly not fit for it

- **Rule:** No circuit starts, and a running shot stops, while the tank's pH is below 4.0 or above 8.5, or its EC is more than 1.0 mS/cm above the EC target (the higher of this phase's and the last phase's), and always above 5.0 mS/cm. There is no lower EC bound. Water is judged by the check's last measurement: without one, with one older than three holds, or during a probe calibration, it blocks. After a dose or a fill a shot waits until the check has mixed it in and measured again, and a dose that starts stops a running shot. A person may water anyway: one shot by hand, after a confirmation, logged as a warning; a schedule never does.
- **Why:** The worst case of a fertigation controller is a pump that ran too long or dosed too much. The probes see it, and watering would carry the overdose to the roots. The band is wide on purpose: it catches a gross overdose, not a missed recipe, and a narrow band would block uncorrected tap water and leave plants dry (RAT-084). A reading right after a dose does not show it yet (RAT-052), and a missing reading is never fit (RAT-006).
- **Evidence:** Calculated from measured rates: one 60 s run of parts A and B together (52.8 and 39.6 ml/min, RAT-054) adds about 1.3 mS/cm in 20 L at 0.275 per ml/L (RAT-055), caught when the tank was within about 0.3 of its target; in 100 L it adds about 0.25 and passes. One 60 s run of pH− at 52.8 ml/min and 5.0 pH per ml/L (RAT-050, measured once) takes 200 L from pH 5.8 to about 4.5, which passes; in 20 L the same run, extended tenfold from that single point, reads below 3 and blocks as implausible (RAT-020) or as a jump (RAT-039). The limits 4.0, 8.5, 1.0 and 5.0 mS/cm and the mixing time's scaling with volume are assumptions.
- **Changed:** The mixing wait is the check's own (SD-112), and the water is judged by its measurement after it, since the probes see the tank only while it measures. The rule allowed no override; the owner decided for one shot by hand (SD-134).
- **Implemented in:** `core/src/watering.cpp`, `core/src/robot.cpp`
- **Tests:** `tests/core/test_watering.cpp` "Watering: water is fit within a wide band: pH 4.0 to 8.5, EC at most 1.0 above its target and never above 5.0, with a recent reading and no calibration; pH not corrected only warns (RAT-086, RAT-084, R5)"; `tests/core/test_watering.cpp` "Watering: water not fit keeps a circuit from watering and stops a shot; a person may water anyway, which is a warning (SD-134, RAT-086)"; `tests/core/test_watering.cpp` "Watering: Stop, maintenance and a dose stop a running shot; a restart reports a cut shot and does not repeat it (SD-134, R6, RAT-086)"

### RAT-087 – A fixed pattern to a target EC never doses more than the recipe's amounts

- **Dropped:** the grow controller only (mixing a fixed pattern in a bucket); not part of the mixing robot (SD-105).
- **Rule:** Mixing to a target EC without measuring scales every scaled part of the recipe by one factor of at most 1, worked out from the target, the water's EC and the effect (the recipe's chart EC over all its parts, or a larger learned effect); it never doses more than the recipe's own amounts per litre, and a fixed part goes in per litre of fresh water. It is refused without a chart EC inside the band (a quarter to four times the start effect), a target within the EC target's range or the water's EC; the water's EC is never taken as 0, and one above the target's range is refused as a likely µS/cm. The plan says it does not measure and names the EC to expect.
- **Why:** A fixed pattern gets no feedback, often with RO water entered as 0 and no meter at hand. A chart EC typed too low still passes the band (1.2 instead of 2.1, RAT-056), so scaled up from it the pattern could dose up to four times the chart's amounts, which only draining and diluting undo (RAT-055). Bounded by the recipe's amounts, a slip doses at most what the recipe says; a target the recipe cannot reach is named, not chased.
- **Evidence:** — (a safeguard from a review; manufacturer charts scale their base parts to a target EC in a fixed ratio)
- **Later:** not planned (mixing without a probe, in a bucket, is no part of the robot, which measures before every dose).

### RAT-088 – A mix to the target measures the mixed water first, and its first step must raise EC

- **Dropped:** the grow controller only (mixing in a bucket; the test dose of a new tank (SD-105) takes the place of judging the first step); not part of the mixing robot (SD-105).
- **Rule:** Mixing to the target with a probe first circulates the water – 100 s per 20 L and at least the EC settle time, at least 100 s and at most 60 min, counted from when the pump runs – then reads its EC and works the amounts out again. Nothing is dosed while it measures. A pump that does not run, the inlet opening, or a probe not usable or in a calibration end it with nothing in and the reason. The first step is 0.8 × the gap, at most the step limit, converted with the largest known effect, and no more than the recipe's own amounts while the recipe has neither a chart EC nor a learned effect. Once mixed in, the step is judged before anything is learned: a rise below the larger of 0.02 mS/cm and a quarter of the expected one locks EC dosing with its own text, and one above four times the expected is named. Only a clean step is learned (RAT-057).
- **Why:** Fresh or topped-up water is often not mixed at the probe yet, and the first step is worked out from that reading. A dry or misplaced probe reads about 0 and still counts as usable: without the check, up to two more rounds at the full step limit would follow before the no-effect lock (RAT-055), into a tank the probe cannot see, and a wrong rise once learned sizes every later round wrong. A product of unknown strength could make the first step several times the step limit, so it stays within the recipe until its strength is known.
- **Evidence:** — (a safeguard from a review; the mixing time per volume as for RAT-052 and RAT-058, an assumption)
- **Later:** not planned (mixing in a bucket). The robot measures after flushing before every decision, and the test dose of a new tank judges its first dose (SD-105).

### RAT-089 – EC is compensated to 25 °C; without a water temperature it is no value for control

- **Rule:** The EC reading is compensated linearly to 25 °C at 2 % per °C with the water temperature measured in the same measuring chamber. Without a usable water temperature the EC is shown and marked "not temperature compensated", and the cycle doses no nutrients on it. An EC calibration needs the water temperature too, and the reference is taken at its value at 25 °C as printed on the bottle.
- **Why:** Conductivity rises by about 2 % per °C. A tank at 18 °C reads about 14 % lower than at 25 °C; dosing towards a target on that reading overdoses by the same share, and an overshoot can only be undone by dilution. Reference solutions are printed at 25 °C.
- **Evidence:** — (the 2 %/°C coefficient is the usual value for nutrient solutions; not measured on the reference installation)
- **Implemented in:** `core/include/doseph/readmodel.hpp`, `core/include/doseph/truth.hpp`, `core/src/robot.cpp`, `core/src/truth.cpp`
- **Tests:** `tests/core/test_truth.cpp` "Sensor truth: EC is compensated to 25 °C at 2 % per °C; without a water temperature it is no value for control (RAT-089)"
