# Rationale register

Each domain rule of this software that was learned from operating
practice has an entry here. The entry says what the rule is, why it
exists, what evidence backs it, where it is implemented and which tests
cover it. Code, tests and other docs cite only the ID (RAT-001 …).

- **IDs are stable** and the same as in the predecessor
  growcontroller-software (`docs/PROVENANCE.md`). An ID is never reused. A
  rule of the grow controller that the mixing robot does not need stays
  listed as dropped, with the area it belonged to.
- **Reference installation** means the private installation on which the
  rules were first developed and measured. Evidence names the year only.
- **Evidence** says whether a number was measured, observed, calculated
  or found in a review. "—" means there is no number behind it. The
  evidence was gathered for the grow controller; it applies to the
  mixing robot where the rule does.
- **Changed** says what changed from the predecessor's rule.
- References written `gc PD-xxx` or `gc#n` are product decisions and
  issues of the predecessor; `SD-001` to `SD-104` are its software
  decisions (`docs/DECISIONS.md`).
- New rules get the next free ID (now RAT-090), an entry here and a test.

### RAT-001 – Fill volume is computed; the level sensor only checks and cuts off

- **Dropped:** the grow controller only (fill volume and level sensor); not part of the mixing robot (SD-105).

### RAT-002 – Harvest is recorded only by the user

- **Dropped:** the grow controller only (grow runs and harvest); not part of the mixing robot (SD-105).

### RAT-003 – One pump job at a time; bad calibration results are rejected

- **Rule:** Only one pump job, a calibration or priming, runs at a time, and none while the cycle doses; a second start is refused with the reason. A calibration result that is missing, zero, negative or above 1000 ml is rejected and the previous value stays.
- **Why:** A dosing sequence that can start twice overdoses. One that runs on a missing or zero rate either has no exit or underdoses, and either way it looks like success.
- **Evidence:** Observed on the reference installation (2026): a mixing sequence that started without valid inputs could neither finish nor stop, and a flow rate of zero would have passed as valid. Limits on attempts and run time, a refusal of zero rates and a lock against a second start fixed it.
- **Changed:** The mix jobs of the predecessor and their confirmation are gone; the cycle (SD-105) is the only thing that doses into the tank.
- **Implemented in:** `core/src/config.cpp`, `core/src/robot.cpp`
- **Tests:** `tests/core/test_cycle.cpp` "Pumps: a calibration runs into the cup and is worked out from the actual run time; a senseless result is refused (RAT-003, RAT-070)"

### RAT-004 – pH correction always comes last and is never part of a recipe

- **Rule:** Recipes hold nutrients only. pH− in a recipe is refused when the configuration is saved. The cycle corrects pH after EC, and pH waits while a nutrient round is open.
- **Why:** Nutrients shift pH, and the base pair adds most of the EC and pulls pH down by itself, so a pH step taken before it aims at a solution that does not exist yet. The tank then reaches irrigation at an unknown, usually too low pH, and without a pH-up channel nothing can bring it back.
- **Evidence:** Observed on the reference installation (2026): with acid added before the base pair and no measurement afterwards, the final pH was unknown. With acid last, it adds only a few hundredths of a mS/cm (about 20 ml per 100 L), less than the 0.05 EC tolerance. Precipitation, the usual argument against a late pH step, did not apply: the water was reverse-osmosis water with almost no carbonate hardness, and a buffer went in before any nutrient.
- **Implemented in:** `core/include/doseph/plan.hpp`, `core/src/config.cpp`, `core/src/cycle.cpp`
- **Tests:** `tests/core/test_config.cpp` "Configuration: pH correction is never part of a recipe (RAT-004)"; `tests/core/test_cycle.cpp` "Cycle: pump on, flow proven, flushed, measured; a test dose first, EC before pH, mixed in with flow, measured again, then it holds (SD-105, RAT-004, RAT-052)"

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

- **Rule:** After a crash or restart every pump is off and the circulation pump is off; nothing resumes on its own (R6). A dosing run is limited to 60 s and the device driver cuts a pump off on its own after 90 s.
- **Why:** Timers held in volatile memory are lost on a restart or update, and the "off" they were waiting for never comes. Each timed actuator therefore also has a cut-off that works on its own, set above its longest valid run so it never ends a valid one.
- **Evidence:** Observed on the reference installation (2026): many timed sequences kept their off command only in volatile memory, so a restart could leave a pump, valve, heater or dimmer in its last state. Independent cut-offs, such as 90 s for dosing pumps, closed that gap.
- **Implemented in:** `core/include/doseph/journal.hpp`, `core/src/robot.cpp`, `sim/world.cpp`
- **Tests:** `tests/core/test_cycle.cpp` "Cycle: a restart while a pump doses is reported; the next check mixes first and learns nothing (SD-106, R6, RAT-007)"

### RAT-008 – A tolerance is never 0

- **Rule:** A tolerance is never 0: the EC and pH tolerances are at least 0.05 and are refused below.
- **Why:** A band of zero width is never met by a real reading, so the check fails all the time. An alarm that never clears hides the alarms that matter. A tolerance of 0 is therefore treated as a configuration error and refused.
- **Evidence:** Observed on the reference installation (2026): tolerances of 0 kept the watchdog permanently red. Working values were about 1 K for air temperature, 5 % for humidity, 100 ppm for CO2 (above the sensor accuracy of about ±50 ppm) and 0.2 for pH and EC alarms, while the mixing loop stopped at the tighter 0.1 pH and 0.05 EC.
- **Implemented in:** `core/include/doseph/config.hpp`, `core/src/config.cpp`, `core/src/cycle.cpp`
- **Tests:** `tests/core/test_config.cpp` "Configuration: a tolerance of 0 is refused (RAT-008); settings only within their bounds (R7)"

### RAT-009 – Climate targets keep VPD in a corridor

- **Dropped:** the grow controller only (room climate); not part of the mixing robot (SD-105).

### RAT-010 – Irrigation is controlled by the number of shots, not the shot size

- **Dropped:** the grow controller only (irrigation); not part of the mixing robot (SD-105).

### RAT-011 – Top-up mixing doses on the fresh water only

- **Dropped:** the grow controller only (mixing into fresh water in a bucket or watering can); not part of the mixing robot (SD-105).

### RAT-012 – EC can only be raised

- **Rule:** The robot can only add nutrients. An EC above its target is left as it is and named with the reason ("fresh water lowers it"); the cycle does not report it as reached.
- **Why:** Dosing only adds ions, so a target below the present EC cannot be reached. Unless the app says so, mixing reports success while the tank stays too strong, right in the phase that called for less.
- **Evidence:** Calculated on the reference installation (2026): when a phase change lowered the EC target from 3.0 to 2.0 mS/cm, the solution left in the tank was already above the new target, yet the mixing step would report the target as reached. A later lesson there: a drain must be followed directly by a refill, or the day's irrigations run without water.
- **Implemented in:** `core/src/cycle.cpp`
- **Tests:** `tests/core/test_cycle.cpp` "Cycle: EC above its target and pH below it are left as they are, with the reason (RAT-012, RAT-037, RAT-041)"

### RAT-013 – Dimming never goes below the lamp's switch-on threshold

- **Dropped:** the grow controller only (light); not part of the mixing robot (SD-105).

### RAT-014 – Drain percentage leads irrigation; substrate readings inform only

- **Dropped:** the grow controller only (drain-to-waste irrigation); not part of the mixing robot (SD-105).

### RAT-015 – No substitute values for quantities; invalid data is a problem

- **Rule:** No stand-in values for quantities: an uncalibrated pump doses nothing, a tank volume that is missing stops dosing, a reading that is not usable is a problem in the watchdog, not a neutral state. Guards test for a finite number explicitly.
- **Why:** When a run time or an amount is computed from a stand-in value, a fault turns into a dose that nobody notices. Missing data must therefore fail a watchdog check, not silence it. Comparisons do not catch NaN, since every comparison with NaN is false (NaN < 1 and NaN ≤ 0 alike), so guards test for a finite number explicitly. A counter quietly restarted at 0 later looks like a real one, so an unknown stock stays unbooked. A bottle assumed full looks like a real count too: its low-stock and below-zero warnings then come late by whatever was missing, or not at all, and a mix check passes that the bottle cannot cover.
- **Evidence:** Observed on the reference installation (2026): default values meant for missing readings did not take effect when a sensor was offline, and many of them were 0 anyway; a check for "volume ≤ 0" let a missing (NaN) volume pass.
- **Implemented in:** `core/include/doseph/config.hpp`, `core/include/doseph/plan.hpp`, `core/src/gateway.cpp`, `core/src/watchdog.cpp`
- **Tests:** `tests/core/test_plan.cpp` "EC round: a bottle on no pump, or an uncalibrated pump, refuses the round (RAT-015)"; `tests/core/test_watchdog.cpp` "Watchdog: a reading that is not delivered is a problem, never neutral; not calibrated only while a recipe needs it (RAT-015, RAT-021)"; `tests/core/test_watchdog.cpp` "Watchdog: latches, an ended recipe and an uncalibrated pump are problems; the headline names the first and counts the rest (RAT-015, RAT-072)"

### RAT-016 – History leaves gaps empty

- **Rule:** The history stores gaps as gaps (null in the API); a series is never filled with 0 or the last value. pH, EC and temperature are recorded only while water flows through the measuring cell.
- **Why:** The data of a finished run is the basis for comparing runs and costs. If gaps turned into zeros, averages and totals would be silently wrong and could not be told apart from real measurements.
- **Evidence:** Observed on the reference installation (2026): the detailed history was dropped after a retention period, so an export at the end of each run became the lasting record; over about a month the export had no unexplained gaps.
- **Changed:** Recording only with flow is new: a standing cell does not measure the tank.
- **Implemented in:** `core/src/robot.cpp`
- **Tests:** `tests/core/test_api.cpp` "API: history of ph, ec, temp and flow only; values while flow is proven (RAT-016)"

### RAT-017 – VPD is air VPD, with no value when an input is missing

- **Dropped:** the grow controller only (room climate); not part of the mixing robot (SD-105).

### RAT-018 – No parallel dosing sequence; time limits sit above the longest legitimate run

- **Rule:** One dosing run at a time: the gateway refuses a run while another is active. A run is at most 60 s; a run that does not report back in time is switched off and booked as failed.
- **Why:** A loop may only dose again once the previous dose is fully in the reading, which takes at least two sensor smoothing windows plus the mixing time, so a whole sequence can run long. A hung-job timeout shorter than that would release the lock during a valid run and let a second dosing sequence start alongside it, the worst possible result of a safety check.
- **Evidence:** Observed on the reference installation (2026): pH and EC readings were 5-sample moving averages at 15 s intervals, a 75 s window. Settle times of 90–120 s caused overshoot and were raised to 240–260 s. The worst-case sequence grew from about 35 to about 95 minutes, so the limit for a stale sequence had to rise from 90 to 150 minutes to keep the lock. The 60 s run cap and the 90 s device cut-off stayed unchanged.
- **Implemented in:** `core/src/gateway.cpp`
- **Tests:** `tests/core/test_journal.cpp` "Gateway: no dose without flow; one that loses its flow stops at once; one run at a time (SD-105, RAT-018)"

### RAT-019 – Network sockets fail safe on their own

- **Dropped:** the grow controller only (mains outlets on the network); not part of the mixing robot (SD-105).

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

- **Dropped:** the grow controller only (alarm notifications (not built yet)); not part of the mixing robot (SD-105).

### RAT-023 – Sensor truth always runs; freshness and frozen raw values are checked separately

- **Rule:** Sensor truth runs all the time, also while nothing is dosed. Freshness and a standing raw value are checked separately.
- **Why:** A sensor that fails between runs is cheapest to fix right then; monitoring that waits for the next run finds it only on the start day. Standstill is judged on the raw signal, because calibrated, rounded values may rightly stay the same for hours, while the raw signal of a working probe always shows some noise.
- **Evidence:** Observed on the reference installation (2026): the history stored a new time only when the value changed, so the age of the last entry could not tell a sensor repeating the same value from one that had stopped.
- **Implemented in:** `core/include/doseph/readmodel.hpp`, `core/include/doseph/truth.hpp`, `core/src/truth.cpp`
- **Tests:** `tests/core/test_truth.cpp` "Sensor truth: stale and implausible; standstill of the raw value apart from freshness (RAT-020, RAT-023, RAT-027, RAT-059)"

### RAT-024 – "Not present" is a finding, not a resting state

- **Dropped:** the grow controller only (sensor slots on the bus); not part of the mixing robot (SD-105).

### RAT-025 – Control reads only validated values; no valid calibration, no control value

- **Rule:** The cycle reads only values that sensor truth marks usable. Without a valid calibration a value is shown but never used for a dose; a reading is never clamped into range.
- **Why:** Clamping turns a defect into a plausible-looking number, and a frozen or invalid value that is still numeric passes every simple guard. Control must stop on its existing safe path instead of acting on such values.
- **Evidence:** Observed on the reference installation (2026): clamping turned a 3.5 V raw signal, i.e. a defect, into a clean-looking pH 0, and four control loops (water heater, humidity, CO2, nutrients) had no check against frozen or stale readings.
- **Implemented in:** `core/include/doseph/common.hpp`, `core/include/doseph/config.hpp`, `core/include/doseph/readmodel.hpp`, `core/include/doseph/truth.hpp`, `core/src/truth.cpp`
- **Tests:** `tests/core/test_truth.cpp` "Sensor truth: no reading is never 0, and a probe without calibration gives no value for control (RAT-006, RAT-025)"; `tests/core/test_truth.cpp` "Sensor truth: pH from two buffers; a slope or zero point out of band is refused (RAT-025, RAT-026)"

### RAT-026 – Validity limits come from the calibration, not from a guess

- **Rule:** Validity limits come from the calibration: a pH calibration is refused when its slope lies outside 0.8–1.25 of the ideal or its zero point more than ±30 mV off; an EC calibration when its factor lies outside 0.5–2.
- **Why:** A limit set by feel is either too loose or too tight. Too loose is the dangerous side: a broken probe then reads like clean water and dosing goes on. Too tight only gives a false alarm that stops dosing, so that side is accepted; the answer to such an alarm is to check the probe and its zero point, and the limit moves only when a new calibration gives a new number.
- **Evidence:** Measured on the reference installation (2026): with a three-point EC calibration, the calibrated value at the probe's zero point in distilled water was −0.0159 mS/cm, so the lower limit became −0.02 mS/cm instead of an estimated −0.05 mS/cm; the remaining margin was about 0.6 mV of raw signal.
- **Implemented in:** `core/src/truth.cpp`
- **Tests:** `tests/core/test_truth.cpp` "Sensor truth: pH from two buffers; a slope or zero point out of band is refused (RAT-025, RAT-026)"

### RAT-027 – A sensor counts as failed only after several missed readings, judged by its last measurement

- **Rule:** A channel is stale only after it missed its updates for longer than its limit (pH and EC 60 s, temperature 120 s, flow 15 s), judged by its last measurement, never by a change of value.
- **Why:** A single missed reading is usually a short communication glitch; alarming on it produces false alarms. Requiring several misses still detects a real defect quickly. Judging by a change of value instead hides a channel that falls silent while its device keeps running, and makes a calm, healthy sensor look old.
- **Evidence:** Observed on the reference installation (2026): first, with freshness judged only per device and sensors reporting every 30 s, a limit of 5 min separated real defects from wireless dropouts, and six missed reports were too close to ordinary dropouts. Later the installation sent a sign of life per channel every 15 s: in a 14-day replay with real data, a water temperature read every 30 s was never stale at 60 s, and the other channels stayed between 0 and 20 s old.
- **Implemented in:** `core/src/truth.cpp`
- **Tests:** `tests/core/test_truth.cpp` "Sensor truth: stale and implausible; standstill of the raw value apart from freshness (RAT-020, RAT-023, RAT-027, RAT-059)"

### RAT-028 – After a restart, outputs fall back to off but recorded facts survive

- **Rule:** After a restart the outputs fall back to off, while recorded facts survive: latches, jump locks, learned effects, the Stop, a new tank and an open dose in the dosing journal (SD-106).
- **Why:** Outputs and recorded facts need opposite handling after an unexplained reset. An output is safest off, and switching it back on by itself is a hazard. A recorded fact (a lock is set, a calibration is valid, a run is active) has to come back, because losing it quietly releases a lock or ends a run.
- **Evidence:** Observed on the reference installation (2026): stored on/off facts such as "calibration valid" and "run active" could be lost without any error message, while numeric settings were kept.
- **Implemented in:** `core/src/robot.cpp`
- **Tests:** `tests/core/test_config.cpp` "Run-time state: latches, jump locks and learned effects survive; a damaged one throws (RAT-028, RAT-063)"

### RAT-029 – Dimming to zero also switches the light relay off

- **Dropped:** the grow controller only (light); not part of the mixing robot (SD-105).

### RAT-030 – Refill supervision respects the probe's blind zone and judges the rate over a window

- **Dropped:** the grow controller only (tank refill); not part of the mixing robot (SD-105).

### RAT-031 – No automatic restart after a refill fault

- **Dropped:** the grow controller only (tank refill); not part of the mixing robot (SD-105).

### RAT-032 – The inlet's emergency limit follows the valve, not the job

- **Dropped:** the grow controller only (tank refill (inlet valve)); not part of the mixing robot (SD-105).

### RAT-033 – Narrow control band, wider alarm band

- **Rule:** The alarm band is wider than the control band: the watchdog reports EC or pH as a problem only beyond the control tolerance plus 0.2.
- **Why:** With one shared limit, every normal control step also shows up as an alarm, and people learn to ignore alarms. The alarm is meant for a controller that cannot hold its target, so it sits outside the control band.
- **Evidence:** Observed on the reference installation (2026): with control and alarm both at ±1 K for air temperature, each control step in a test night also appeared as an alarm. The bands were then split: ±1.0 K for control and ±2.0 K for the alarm on air temperature, ±3 % and ±7 % on relative humidity.
- **Implemented in:** `core/include/doseph/watchdog.hpp`, `core/src/watchdog.cpp`
- **Tests:** `tests/core/test_watchdog.cpp` "Watchdog: the alarm band is wider than the control band (RAT-033)"

### RAT-034 – Humidifier and dehumidifier never together; compressor protection

- **Dropped:** the grow controller only (room climate); not part of the mixing robot (SD-105).

### RAT-035 – Light ramps of 15 minutes inside the light period

- **Dropped:** the grow controller only (light); not part of the mixing robot (SD-105).

### RAT-036 – Stop is an active, idempotent shutdown

- **Rule:** Stop is active and idempotent: it switches every dosing pump and the circulation pump off, books what ran and holds until Resume. While stopped nothing runs, not even a calibration into a cup.
- **Why:** If control loops merely stop issuing new commands, a stop freezes the last state and devices keep running unnoticed.
- **Evidence:** Observed on the reference installation (2026): a stop that only halted new commands left light, fans and the circulation pump running for hours.
- **Implemented in:** `core/include/doseph/config.hpp`, `core/include/doseph/cycle.hpp`, `core/include/doseph/gateway.hpp`, `core/src/robot.cpp`
- **Tests:** `tests/core/test_cycle.cpp` "Cycle: a Stop switches everything off at once, holds across a restart, and a resume mixes first what went in (RAT-036)"

### RAT-037 – Display follows the actual state; every block is a visible line with its reason

- **Rule:** What the robot does is shown as one line for the cycle, and everything it leaves as it is stands as a note with its reason; a note goes away with its cause.
- **Why:** A display based on requests misses devices switched some other way, and "requested but not running" is the state a user most needs to see. Blocks and control states need their own visible line, and that line has to go away with the block, or it turns into a stale message.
- **Evidence:** Observed on the reference installation (2026): the display showed the requested state, so a pump started some other way appeared as "off" while it ran, and blocks and control states had no visible line.
- **Implemented in:** `core/include/doseph/cycle.hpp`
- **Tests:** `tests/core/test_cycle.cpp` "Cycle: EC above its target and pH below it are left as they are, with the reason (RAT-012, RAT-037, RAT-041)"

### RAT-038 – Refill stops by time, not by the lagging level reading

- **Dropped:** the grow controller only (tank refill); not part of the mixing robot (SD-105).

### RAT-039 – pH/EC safety: jump lock, hand-dose cap, no silent blocks

- **Rule:** A jump lock blocks dosing on a reading that jumped unexpectedly; it lifts after 15 min steady and survives a restart. A skipped dose is never silent: it stands as a note on the status page.
- **Why:** A disturbed probe can deliver wrong values that still lie inside the plausible range, and dosing on them is dangerous. An unbounded manual pump run can add a large amount of acid in one go. A skipped dosing that is only logged looks like a working system.
- **Evidence:** Observed on the reference installation (2026): without galvanic isolation the pH and EC probes disturbed each other; pH read 10.4–10.9 at a real value of about 2, and no check caught it. A dose skipped at pH 9.4 left its reason only in a log. Hand doses of pH-down added up to about 74 ml (93 s of pumping) in 23 L; the only limit was a 90 s device auto-off, about 70 ml in a single run.
- **Changed:** The hand dose and its cap are gone: the robot has no hand dose.
- **Implemented in:** `core/include/doseph/config.hpp`, `core/include/doseph/readmodel.hpp`, `core/src/truth.cpp`
- **Tests:** `tests/core/test_truth.cpp` "Sensor truth: a jump locks for 15 min of calm and survives a restart; an announced change explains it (RAT-039, RAT-042, RAT-044)"

### RAT-040 – Book consumption per run, hand doses included; test runs are not consumption

- **Dropped:** the grow controller only (consumption and stock of bottles (not built)); not part of the mixing robot (SD-105).

### RAT-041 – Latch pH lowering that has no effect; no pH counter-correction

- **Rule:** pH dosing that has no effect latches (RAT-020); the robot never raises pH. A pH below its target is left as it is and named; raising it is up to a person.
- **Why:** A dose without effect points to an empty canister, a blocked line or a faulty probe; repeating it blindly risks a large overshoot once the fault clears. Alternating between lowering and raising ("ping-pong") wastes product, adds ions and can oscillate.
- **Evidence:** —
- **Implemented in:** `core/include/doseph/cycle.hpp`, `core/src/cycle.cpp`, `core/src/truth.cpp`
- **Tests:** `tests/core/test_cycle.cpp` "Cycle: EC above its target and pH below it are left as they are, with the reason (RAT-012, RAT-037, RAT-041)"; `tests/core/test_truth.cpp` "Sensor truth: a pH fall names the robot's own dose as one possible cause, and that it only lowers pH (RAT-041)"

### RAT-042 – Announced interventions explain sensor jumps

- **Rule:** An announced intervention explains jumps: a new tank explains jumps of pH, EC and temperature for 10 min, a dose the cycle made explains the change it causes while it is mixed in.
- **Why:** The jump lock exists to catch a misbehaving sensor. A step that follows a known action is not a fault; treating it as one blocks dosing and trains users to ignore alarms.
- **Evidence:** Observed on the reference installation (2026): taking the probes out for a buffer test made EC fall from 1.19 to 0.0 mS/cm and come back at 1.21 about 2.5 min later. The jump lock did what it should, but its alarm only reported a planned intervention.
- **Implemented in:** `core/include/doseph/robot.hpp`, `core/include/doseph/truth.hpp`, `core/src/cycle.cpp`, `core/src/robot.cpp`, `core/src/truth.cpp`
- **Tests:** `tests/core/test_cycle.cpp` "Cycle: a probe calibration holds the cycle; a dose running is cut and mixed first (RAT-042)"; `tests/core/test_truth.cpp` "Sensor truth: a jump locks for 15 min of calm and survives a restart; an announced change explains it (RAT-039, RAT-042, RAT-044)"

### RAT-043 – No pH correction on an old reading

- **Rule:** No pH dose on an old reading: a reading older than its limit is not usable, and the cycle measures afresh after flushing the cell before every decision.
- **Why:** A pH value that stops updating keeps looking plausible while the tank changes. Acid dosed against it can drive the real pH far from target, so the age of the value has to show and has to stop dosing.
- **Evidence:** Observed on the reference installation (2026): for about five hours the pH reading still showed 7.01 from a buffer test, while the target check and the overall status stayed green. At that time only the pH-raising path checked the age of the reading; the lowering path would have dosed on the old value.
- **Implemented in:** `core/src/cycle.cpp`
- **Tests:** —

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
- **Tests:** —

### RAT-047 – Circulation dry-run protection with its own thresholds

- **Dropped:** the grow controller only (circulation dry-run protection by level; the flow proof (SD-105) takes its place); not part of the mixing robot (SD-105).

### RAT-048 – Water temperature is "not applicable" in an empty tank

- **Dropped:** the grow controller only (water heater and level); not part of the mixing robot (SD-105).

### RAT-049 – Dry-run detection from pump power (planned)

- **Dropped:** the grow controller only (dry-run detection from pump power); not part of the mixing robot (SD-105).

### RAT-050 – Effect-based cap and minimum run time for pH-lowering doses

- **Rule:** A pH− dose is capped by its effect: at most 0.3 pH of change, at most 0.3 ml/L and at most the set maximum, planned with a start effect of 5.0 pH per ml/L; a run shorter than 1 s is not made.
- **Why:** Underestimating the acid means overdosing, and a pH overshoot cannot be undone; overestimating only costs extra rounds. Very short runs are too imprecise.
- **Evidence:** Measured on the reference installation (2026): 4.9 ml of pH− in 20.4 L of reverse-osmosis-based solution at EC 2.96 (0.24 ml/L) lowered pH by about 1.2, i.e. roughly 5.0 pH per ml/L – 7.5 times what the earlier dose formula assumed. A fixed 20 ml cap would have allowed a drop of about 4.9 pH for a 1.0 pH gap; the effect-based cap gives 1.22 ml (about 1.5 s of pumping) at 20.4 L.
- **Implemented in:** `core/include/doseph/config.hpp`, `core/include/doseph/plan.hpp`, `core/src/cycle.cpp`, `core/src/gateway.cpp`, `core/src/plan.cpp`
- **Tests:** `tests/core/test_plan.cpp` "Partial runs: equal in size, none under 1 s, no more than allowed (RAT-050, RAT-055)"; `tests/core/test_plan.cpp` "pH dose: its cap and the start effect (RAT-050)"

### RAT-051 – A dry-run lock is binding for every switching path

- **Dropped:** the grow controller only (dry-run locks); not part of the mixing robot (SD-105).

### RAT-052 – Mixing and settling: pH needs much longer than EC

- **Rule:** After a dose the robot circulates before it measures again, and pH waits longer than EC. The waits are set by the user per tank; the suggestion is 2 turnovers after nutrients and 3 after pH− (SD-105), and the mixing clock runs only while water flows.
- **Why:** Dosing again before the previous dose is visible stacks doses and overshoots – irreversibly for pH. EC and pH respond very differently, so one shared waiting time does not fit.
- **Evidence:** Measured on the reference installation (2026; 20.4 L, circulation running): after nutrient doses EC reached tolerance after a median of 32 s (t63 median 26 s). After a pH-lowering dose the reading stayed flat for 99 s, about 80 s of that after the circulation pump had restarted, then fell steeply and reached tolerance after 194 s. Cutting the pH wait to about 45 s, in line with a faster sensor filter, would have let four doses in before the first one showed.
- **Changed:** The fixed waits of the predecessor became the user's waits with a suggestion from volume and flow (SD-105).
- **Implemented in:** `core/include/doseph/config.hpp`, `core/src/cycle.cpp`, `sim/world.cpp`
- **Tests:** `tests/core/test_cycle.cpp` "Cycle: pump on, flow proven, flushed, measured; a test dose first, EC before pH, mixed in with flow, measured again, then it holds (SD-105, RAT-004, RAT-052)"

### RAT-053 – EC keeps a reserve for the pH-lowering dose that follows

- **Rule:** An EC round aims below its target by a reserve for the pH− dose that follows: the expected EC rise of that dose (1.04 mS/cm per ml/L), at most 0.3 mS/cm.
- **Why:** Acid adds ions, and the pH step always follows the EC step, so dosing EC exactly to target and then lowering pH ends above the EC target. Ending slightly low is the safe side: a shortfall is made up by the next dose, while surplus nutrients can only be removed by diluting.
- **Evidence:** Measured on the reference installation (2026): 4.9 ml of pH− in 20.4 L (0.24 ml/L) raised EC from 2.96 to 3.21 mS/cm, i.e. 1.04 mS/cm per ml/L – almost four times the effect of the nutrient pair itself (0.275). That run ended 7 % above the EC target. For a 1.0 pH gap the reserve is 1.0 ÷ 5.0 × 1.04 ≈ 0.21 mS/cm.
- **Implemented in:** `core/src/cycle.cpp`
- **Tests:** —

### RAT-054 – Paired nutrients keep their ratio at every limit

- **Rule:** The parts of a pair are scaled by one factor at every limit (round cap, run time, partial runs) and never capped or rounded per pump; a round that would need too many partial runs is made smaller for all its parts.
- **Why:** No two pumps deliver exactly the same flow. A time limit applied to each pump on its own turns the recipe ratio into the ratio of the flow rates. EC cannot show this, because it measures the total of all ions, not how they are split between the parts.
- **Evidence:** Measured on the reference installation (2026): the two pumps delivered about 52.8 and 39.6 ml/min. Capping each at 60 s gave A:B = 1.33:1 instead of 1:1 for larger doses in a 100–150 L tank; rounding A to whole millilitres turned a 9 ml dose into 5:4 (1.25:1).
- **Implemented in:** `core/include/doseph/config.hpp`, `core/include/doseph/plan.hpp`, `core/src/config.cpp`, `core/src/plan.cpp`, `sim/world.cpp`
- **Tests:** `tests/core/test_config.cpp` "Configuration: a pair keeps all its parts; none of them is fixed (RAT-054, RAT-085)"; `tests/core/test_plan.cpp` "EC round: 0.8 × gap, the pair scaled together, its ratio kept at every cap (RAT-054, RAT-055)"; `tests/core/test_plan.cpp` "EC round over the partial-run limit: made smaller by one factor for all parts, never refused or capped per pump (RAT-054)"

### RAT-055 – Dose from the measured effect; large doses in equal partial runs

- **Rule:** Each round doses 0.8 × gap ÷ effect × volume. The effect starts at a fixed value (EC 0.275 mS/cm per ml/L of the recipe's scaled parts, pH 5.0 per ml/L) and is then learned from clean rounds, clamped to a quarter to four times the start. Large doses are split into equal partial runs of at most 60 s with 3 s pauses, at most 6 per bottle. Two rounds in a row without effect latch.
- **Why:** Aiming at 80 % of the gap leaves room for incomplete mixing at measurement time, and an EC overshoot can only be undone by dilution. Both clamp directions yield the smaller dose. Silently shortened doses falsify the recipe. With pumps far too slow for the tank a single round rises by less than the noise, yet an empty bottle or a pump that delivers nothing must still be caught: small rounds are judged together once they should have raised EC by 0.06.
- **Evidence:** Measured on the reference installation (2026): the nutrient pair raised EC by 0.275 mS/cm per ml/L in reverse-osmosis water (about 20 L). In model runs from EC 0.55 to 3.0 with 30 % of the last dose not yet mixed, aiming at the full gap ended at 3.30 and aiming at 80 % at 3.15. Before splitting, base doses cut to 60 s delivered only 64–80 % of the recipe amount in a 100 L tank.
- **Implemented in:** `core/include/doseph/config.hpp`, `core/include/doseph/cycle.hpp`, `core/include/doseph/plan.hpp`, `core/src/cycle.cpp`, `core/src/gateway.cpp`
- **Tests:** `tests/core/test_cycle.cpp` "Cycle: an empty bottle: two rounds without effect lock nutrient dosing until a person releases it (RAT-055, RAT-062)"; `tests/core/test_plan.cpp` "Partial runs: equal in size, none under 1 s, no more than allowed (RAT-050, RAT-055)"; `tests/core/test_plan.cpp` "EC round: 0.8 × gap, the pair scaled together, its ratio kept at every cap (RAT-054, RAT-055)"

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
- **Tests:** `tests/core/test_cycle.cpp` "Cycle: a dose that loses its flow stops at once; what went in mixes with flow before it is measured, and nothing is learned (SD-105, RAT-057)"

### RAT-058 – Waiting times cover the time to tolerance

- **Rule:** The wait after a dose has to cover the time until the reading stays within the control tolerance, with margin. The robot does not measure it on its own yet: the user sets the waits, and the suggestion from volume and flow is a starting point (SD-105).
- **Why:** The next dosing decision only changes while the remaining error is outside the control tolerance (EC 0.05 mS/cm, pH 0.1), so that is the time to measure. A t95 value mostly measures display resolution, because the slow end of the response disappears in the last digit.
- **Evidence:** Measured on the reference installation (2026): one EC response was within 0.03 mS/cm after 51 s, yet its t95 came out at 127 s. At about 20 L, EC reached tolerance after at most 47 s; with the factor 2.2 this gives about 100 s.
- **Changed:** by SD-105, the waits are the user's per tank; whether the suggestion is a lower bound is open (product question #1).
- **Implemented in:** `core/src/cycle.cpp`
- **Tests:** —

### RAT-059 – Freshness and standstill are separate checks

- **Rule:** A raw value that stands still is a fault only for a channel that should be noisy (pH, EC); a rounded or quiet signal such as the water temperature is checked for freshness only.
- **Why:** A rounded or very quiet signal that does not change is not a fault; flagging it produces false alarms that teach users to ignore warnings. A sensor that stops reporting is still caught by the freshness check.
- **Evidence:** Observed on the reference installation (2026): a water temperature reported in 0.1 °C steps stayed on one value for more than 60 min twenty times in 14 days (up to 211 min) with a full tank and a working sensor; an air temperature sensor with 0.1 °C resolution stood still for up to 182 min. None of them had a gap in delivery.
- **Implemented in:** `core/include/doseph/readmodel.hpp`, `core/include/doseph/truth.hpp`, `core/src/truth.cpp`
- **Tests:** `tests/core/test_truth.cpp` "Sensor truth: stale and implausible; standstill of the raw value apart from freshness (RAT-020, RAT-023, RAT-027, RAT-059)"

### RAT-060 – Mains outlets protect themselves; no heater without an emergency shutdown

- **Dropped:** the grow controller only (mains outlets and heater); not part of the mixing robot (SD-105).

### RAT-061 – Heater: judge the heating effect from power and water volume

- **Dropped:** the grow controller only (water heater); not part of the mixing robot (SD-105).

### RAT-062 – Switch-on lock vs. latch; latch only after an intervention, reason frozen

- **Rule:** A latch is set only after a dose had no effect or a test dose did not fit, never on its own; its reason is frozen when it is set.
- **Why:** Latching while the device is already off (e.g. an empty tank between runs) produces permanent red states and acknowledgements that seem to do nothing; a reason rewritten every cycle no longer tells what triggered the shutdown.
- **Evidence:** Observed on the reference installation (2026): when the tank was emptied with pump and heater already off, both protections latched anyway and stayed red; acknowledging had no effect while the tank was still empty, and the stored reason kept changing as the level fell. With latching limited to real shutdowns, switch-on attempts at 0 L were cut within 84–138 ms and latched, and acknowledging took effect within about 50 ms.
- **Implemented in:** `core/src/watchdog.cpp`
- **Tests:** `tests/core/test_cycle.cpp` "Cycle: an empty bottle: two rounds without effect lock nutrient dosing until a person releases it (RAT-055, RAT-062)"

### RAT-063 – Latches survive a restart

- **Rule:** Latches survive a restart. A saved state that cannot be read counts as "a latch may stand": the robot starts stopped and a person looks.
- **Why:** A restart (power failure, update) must not silently clear a protective shutdown nobody has looked at; otherwise, e.g., a pump that ran dry could be started again unattended, without a valid level showing the tank refilled and without saying so.
- **Evidence:** Observed on the reference installation (2026): one protective latch was lost whenever the control software restarted, while another was restored from stored state. A restored latch whose kind could not be determined was treated as the stricter kind, released only by acknowledgement. Restoration was verified in simulated tests, not live.
- **Implemented in:** `core/include/doseph/config.hpp`, `core/src/config.cpp`, `core/src/robot.cpp`, `core/src/watchdog.cpp`
- **Tests:** `tests/core/test_config.cpp` "Run-time state: latches, jump locks and learned effects survive; a damaged one throws (RAT-028, RAT-063)"

### RAT-064 – End-of-run summary is stored with the run

- **Dropped:** the grow controller only (grow runs); not part of the mixing robot (SD-105).

### RAT-065 – Drain on EC step-down only when needed; always on product change

- **Dropped:** the grow controller only (drain); not part of the mixing robot (SD-105).

### RAT-066 – Phase defaults and recipe templates follow the manufacturer chart

- **Rule:** Recipe templates and their EC targets follow the manufacturer's chart; the pH target is the middle of the recipe's pH range.
- **Why:** Self-chosen EC targets drift far from what the product is designed for; pH targets on the edge of the band end up outside it because control stops about ±0.1 around the target.
- **Evidence:** Measured on the reference installation (2026): the chart implies 0.615 mS/cm per ml/L per part, equal to the mixing-tank measurement (0.62) and 12 % above a soak-water measurement (0.55). Before alignment, EC targets up to 3.0 exceeded the chart maximum (2.6 with PK, 2.3 without), so from the start of flowering 1.3–3 times the manufacturer amount was dosed. Flowering week 1 starts with the switch to 12/12 light.
- **Implemented in:** `core/data/templates.json`, `core/include/doseph/config.hpp`, `web/src/store.ts`
- **Tests:** —

### RAT-067 – Irrigation needs a minimum tank level

- **Dropped:** the grow controller only (irrigation); not part of the mixing robot (SD-105).

### RAT-068 – Irrigation pump only with a valid level above the minimum

- **Dropped:** the grow controller only (irrigation); not part of the mixing robot (SD-105).

### RAT-069 – "Supply" profile for devices with their own thermostat

- **Dropped:** the grow controller only (room climate devices); not part of the mixing robot (SD-105).

### RAT-070 – Calibration is not consumption; book actual run time per run

- **Rule:** A calibration or priming run goes into a cup and never counts as a dose into the tank; the flow rate is worked out from the run time the driver reported, not the requested one.
- **Why:** A calibration run handled like a hand dose is cut short by the hand-dose limit and counted as consumption. Dividing the cup volume by the requested time instead of the actual one then gives too high a flow rate, and all later doses come out too small. Booking only when a job ends loses whatever was pumped before an abort.
- **Evidence:** Observed on the reference installation (2026): a nominal 10-s calibration run was cut by the 5-ml manual limit after 5.7–7.8 s depending on the pump and booked as consumption; dividing the cup volume by 10 s would have overstated the rate by 1.3–1.8×. Consumption was booked only at the end of a mixing sequence, so amounts pumped before an external stop would have stayed unbooked (a gap found in review, not observed).
- **Implemented in:** `core/include/doseph/config.hpp`, `core/include/doseph/gateway.hpp`, `core/include/doseph/hardware.hpp`, `core/include/doseph/journal.hpp`, `core/include/doseph/robot.hpp`, `core/src/gateway.cpp`, `core/src/robot.cpp`
- **Tests:** `tests/core/test_cycle.cpp` "Pumps: a calibration runs into the cup and is worked out from the actual run time; a senseless result is refused (RAT-003, RAT-070)"; `tests/core/test_journal.cpp` "Dosing journal: a calibration or priming run goes into a cup, not into the tank (RAT-070)"

### RAT-071 – Minimum stock per canister: 150 ml, pH− 20 ml

- **Dropped:** the grow controller only (stock of bottles (not built)); not part of the mixing robot (SD-105).

### RAT-072 – Normal states are counted, only deviations are listed

- **Rule:** The watchdog counts normal states and names only deviations; the headline names the first problem and counts the rest.
- **Why:** Expected states shown as permanent individual lines bury the real problems and train users to ignore the overview.
- **Evidence:** Observed on the reference installation (2026): expected states such as "pump off – tank empty" stood as permanent single lines, and one waiting state was listed once per sensor, which filled the overview; folding normal states into one green count left only the deviations as single lines.
- **Implemented in:** `core/src/watchdog.cpp`
- **Tests:** `tests/core/test_watchdog.cpp` "Watchdog: latches, an ended recipe and an uncalibrated pump are problems; the headline names the first and counts the rest (RAT-015, RAT-072)"

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

- **Dropped:** the grow controller only (grow phases); not part of the mixing robot (SD-105).

### RAT-077 – Harvest is an event; "completed" is its own state

- **Dropped:** the grow controller only (harvest); not part of the mixing robot (SD-105).

### RAT-078 – Tank level: piecewise-linear curve and switch-on hysteresis

- **Dropped:** the grow controller only (level sensor); not part of the mixing robot (SD-105).

### RAT-079 – Inlet valve: 25 min maximum, emergency limit by level

- **Dropped:** the grow controller only (inlet valve); not part of the mixing robot (SD-105).

### RAT-080 – Mixing order is recipe data; CalMag EC effect

- **Rule:** The order of a recipe's parts is the dosing order, as the manufacturer gives it; CalMag counts with its own EC effect.
- **Why:** Manufacturers specify their own order (here part B before A, CalMag after the base, cleaner last); a fixed order in code cannot follow different products.
- **Evidence:** The Athena Blended Feed Program, Metric, A01.004 lists the order: Balance → base B → base A → PK → CaMg → Cleanse. Measured on the reference installation (2026): CalMag added 0.217 mS/cm per ml/L; the chart implies about 0.22.
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

### RAT-084 – A skipped pH correction does not stop irrigation, but stays visible

- **Dropped:** the grow controller only (irrigation); not part of the mixing robot (SD-105).

### RAT-085 – Mixing to the target EC: the recipe gives ratio and order; a fixed addition goes in per litre of fresh water

- **Rule:** A recipe gives the ratio and the order of its scaled parts; EC control scales them together to the target. A fixed part goes in per litre of fresh water and is not dosed by EC rounds.
- **Why:** Manufacturer charts scale the base nutrients to a target EC in a fixed ratio, but give additives such as a cleanser a fixed rate per volume. Scaled with an EC correction, an additive is overdosed in every round; driven by EC, its step can take a tank that is already at EC as done and skip itself.
- **Evidence:** Found in the review of SD-057 (the fixed addition is one of its review safeguards); the scaling to a target EC in a fixed ratio: SD-057 sources [1][2]. Observed on the reference installation (2026, see RAT-011): a Ca/Mg step controlled to EC took its target as met and skipped itself, so the fresh water got none.
- **Implemented in:** `core/include/doseph/config.hpp`, `core/include/doseph/plan.hpp`, `core/src/config.cpp`, `core/src/plan.cpp`, `core/src/robot.cpp`, `web/src/api.ts`
- **Tests:** `tests/core/test_config.cpp` "Configuration: a pair keeps all its parts; none of them is fixed (RAT-054, RAT-085)"; `tests/core/test_plan.cpp` "EC round: a fixed addition is neither dosed nor scaled; the round goes to the other parts in their ratio (RAT-085)"; `tests/core/test_plan.cpp` "EC effect after an edit: scaled up by parts no longer scaled, kept only at or above the new start (RAT-085)"

### RAT-086 – Watering stops only on water clearly not fit for it

- **Dropped:** the grow controller only (irrigation); not part of the mixing robot (SD-105).

### RAT-087 – A fixed pattern to a target EC never doses more than the recipe's amounts

- **Dropped:** the grow controller only (mixing a fixed pattern in a bucket); not part of the mixing robot (SD-105).

### RAT-088 – A mix to the target measures the mixed water first, and its first step must raise EC

- **Dropped:** the grow controller only (mixing in a bucket; the test dose of a new tank (SD-105) takes the place of judging the first step); not part of the mixing robot (SD-105).

### RAT-089 – EC is compensated to 25 °C; without a water temperature it is no value for control

- **Rule:** The EC reading is compensated linearly to 25 °C at 2 % per °C with the water temperature measured in the same cell. Without a usable water temperature the EC is shown and marked "not temperature compensated", and the cycle doses no nutrients on it. An EC calibration needs the water temperature too, and the reference is taken at its value at 25 °C as printed on the bottle.
- **Why:** Conductivity rises by about 2 % per °C. A tank at 18 °C reads about 14 % lower than at 25 °C; dosing towards a target on that reading overdoses by the same share, and an overshoot can only be undone by dilution. Reference solutions are printed at 25 °C.
- **Evidence:** — (the 2 %/°C coefficient is the usual value for nutrient solutions; not measured on the reference installation)
- **Implemented in:** `core/include/doseph/readmodel.hpp`, `core/include/doseph/truth.hpp`, `core/src/robot.cpp`, `core/src/truth.cpp`
- **Tests:** `tests/core/test_truth.cpp` "Sensor truth: EC is compensated to 25 °C at 2 % per °C; without a water temperature it is no value for control (RAT-089)"
