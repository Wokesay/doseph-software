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
| Logic reads capabilities, never a channel number or a line or variant name | SD-108 | ✓ | `tools/arch_check.sh` (no `.line`, `.variant` outside the profile's JSON) |
| No dose without proven flow | SD-105 | ✓ | `MixCycle`, `Actuators::startRun` · test_cycle.cpp |
| A dose is written to the dosing journal before its pump starts | SD-106 | ✓ | `Doser`, `DoseJournal` · test_journal.cpp, test_cycle.cpp |

## Cycle: the check (SD-105)

| Rule | Rationale | Status | Implementation · tests |
|---|---|---|---|
| Recipes hold nutrients only; pH is corrected after EC and waits while a nutrient round is open | RAT-004 | ✓ | `plan.hpp`, `config.cpp`, `cycle.cpp` · test_config.cpp, test_cycle.cpp |
| EC is only raised; an EC above target is left as it is, with the reason | RAT-012 | ✓ | `cycle.cpp` · test_cycle.cpp |
| EC above its band with no dose since a check found it within raises "top up water" once (event, watchdog, note); it goes when EC is back within the band; a new tank or recipe starts afresh | SD-115 | ✓ | `config.hpp`, `cycle.cpp`, `robot.cpp`, `watchdog.cpp` · test_scenarios.cpp |
| One line says what the robot does; everything it leaves as it is stands as a note with its reason, and the checklist says why it doses or not | RAT-037 | ✓ | `cycle.hpp`, `cycle.cpp`, `status.tsx` · test_cycle.cpp, robot.spec.ts |
| pH is only lowered; pH− without effect latches; a pH below target is left as it is, with the reason | RAT-041 | ✓ | `cycle.hpp`, `cycle.cpp`, `truth.cpp` · test_cycle.cpp, test_truth.cpp |
| No pH dose on an old reading: the cycle measures afresh after flushing before every decision | RAT-043 | ✓ | `cycle.cpp` · test_cycle.cpp |
| No pH dose below the EC floor, nor within 240 s after a nutrient dose | RAT-046 | ✓ | `config.hpp`, `cycle.hpp`, `cycle.cpp` · test_cycle.cpp |
| After a dose it mixes with flow at least one turnover, at least 2 min, before it measures again; pH waits like EC | RAT-052 | ✓ | `config.hpp`, `cycle.cpp`, `world.cpp` · test_cycle.cpp |
| An EC round keeps a reserve for the pH− dose that follows: at half the start effect until it is learned, no cap, never below the EC floor; EC is topped up once pH is in range (SD-121) | RAT-053 | ✓ | `cycle.cpp` · test_cycle.cpp, test_scenarios.cpp |
| The wait after a dose covers the time to tolerance: one turnover, then steady over 60 s or a fifth of a turnover (at most 10 min); the user may only lengthen it | RAT-058 | ✓ | `cycle.hpp`, `cycle.cpp` · test_cycle.cpp |
| Above 150 L the wait is the settling learned from clean doses (1.5 × the longest of the last three, 5 to 600 min), 60 min until one is learned | SD-112 | ✓ | `plan.cpp`, `cycle.cpp`, `robot.cpp` · test_plan.cpp, test_scenarios.cpp |
| A latch only after a dose without effect or a test dose that did not fit; its reason is frozen | RAT-062 | ✓ | `watchdog.cpp` · test_cycle.cpp |
| The device's feed pump runs for a check and is off in Hold, Wait and Off; no modes | SD-109 | ✓ | `cycle.cpp` · test_cycle.cpp |
| Flow by the device's method: an outlet level sensor proves it while it reports water and the feed pump runs, a flow meter at 0.5 L/min, each for 10 s | SD-109 | ✓ | `robot.cpp`, `cycle.cpp` · test_cycle.cpp |
| The feed pump in the tank goes off after 6 s without flow, at its start or later, before the hardware's own cut; a new try in 5 min, and the second try in a row locks it until a person releases it (or reports only, SD-133); in the device 2 min and three tries | SD-127 | ✓ | `cycle.hpp`, `cycle.cpp` · test_cycle.cpp |
| The hardware's cut for no flow stops the check and counts as a try; switching the pump off re-arms it | SD-127 | ✓ | `cycle.cpp`, `robot.cpp`, `hardware.hpp` · test_cycle.cpp |
| At the end of each check pH and EC with the feed pump running are compared with them standing; a shift beyond 0.1 pH or 0.05 mS/cm is compared once more at once, and confirmed it locks the pump and dosing as a critical alarm until a person releases it | SD-127 | ✓ | `cycle.hpp`, `cycle.cpp`, `alarms.cpp` · test_cycle.cpp |
| The feed pump's current is learned while it moves water; once it has, below 60 % of it for 2 s it goes off at once as a try without flow | RAT-049 | ✓ | `cycle.hpp`, `cycle.cpp` · test_cycle.cpp |
| A probe or channel calibration, or a line fill, stops the check and the feed pump first; the check starts anew once it ends | SD-109 | ✓ | `cycle.cpp`, `robot.cpp` · test_scenarios.cpp |
| A return pump that runs a minute without a pause is dry or blocked: it is held off and latched, everything holds, and a restart holds it off again until a person releases it (or reports only, SD-133) | SD-109 | ✓ | `hardware.hpp`, `gateway.hpp`, `robot.cpp` · test_scenarios.cpp |

## Dose planning and learning

| Rule | Rationale | Status | Implementation · tests |
|---|---|---|---|
| A pH− dose is capped by its effect (0.3 pH, 0.3 ml/L, the set maximum); no run under 1 s | RAT-050 | ✓ | `config.hpp`, `plan.hpp`, `cycle.cpp`, `gateway.cpp`, `plan.cpp` · test_plan.cpp |
| A test dose too small for a pump is made as large as the smallest round every part can run, never past the aim; any other round too small is not dosed, noted, and the check goes on to pH | SD-122 | ✓ | `plan.hpp`, `plan.cpp`, `cycle.cpp` · test_plan.cpp, test_cycle.cpp |
| A pair is scaled by one factor at every limit and keeps its ratio; a part missing from a round cut short is caught up first (SD-114) | RAT-054 | ✓ | `config.hpp`, `plan.hpp`, `config.cpp`, `plan.cpp`, `world.cpp`, `cycle.cpp` · test_config.cpp, test_plan.cpp, test_cycle.cpp |
| Each round doses 0.8 × gap ÷ effect × volume; large doses go in equal partial runs | RAT-055 | ✓ | `config.hpp`, `cycle.hpp`, `plan.hpp`, `cycle.cpp`, `gateway.cpp` · test_cycle.cpp, test_plan.cpp |
| A small learned effect never widens the EC cap | RAT-056 | ✓ | `config.hpp`, `plan.hpp`, `config.cpp`, `plan.cpp` · test_config.cpp, test_plan.cpp |
| An effect is learned only from clean rounds; a test dose only when it passed | RAT-057 | ✓ | `config.hpp`, `cycle.hpp`, `plan.hpp`, `cycle.cpp` · test_cycle.cpp |
| Recipe templates follow the chart; the pH target is the middle of the range | RAT-066 | ◐ | `templates.json`, `config.hpp`, `store.ts` · no test of its own |
| The order of a recipe's parts is the dosing order | RAT-080 | ◐ | `config.hpp` · no test of its own |
| Start effects err on the side of the smaller dose | RAT-081 | ✓ | `plan.hpp` · test_plan.cpp |
| A recipe gives ratio and order; a fixed part goes in per litre and is never scaled by EC | RAT-085 | ✓ | `config.hpp`, `plan.hpp`, `config.cpp`, `plan.cpp`, `robot.cpp`, `api.ts` · test_config.cpp, test_plan.cpp, test_api.cpp |

## Gateway, channels and canisters

| Rule | Rationale | Status | Implementation · tests |
|---|---|---|---|
| One pump job at a time and none while the cycle doses; a senseless calibration result is refused | RAT-003 | ✓ | `config.cpp`, `robot.cpp` · test_cycle.cpp |
| After a restart every pump is off and nothing resumes; a run is at most 60 s | RAT-007 | ✓ | `journal.hpp`, `robot.cpp`, `world.cpp` · test_cycle.cpp |
| One dosing run at a time; a run that does not report back in time is switched off and booked as failed | RAT-018 | ✓ | `gateway.cpp` · test_journal.cpp |
| Stop switches everything off at once, books what ran and holds across a restart until Resume | RAT-036 | ✓ | `config.hpp`, `cycle.hpp`, `gateway.hpp`, `robot.cpp` · test_cycle.cpp |
| A calibration or line fill is no dose into the tank; flows and amounts come from the actual run time | RAT-070 | ✓ | `config.hpp`, `gateway.hpp`, `hardware.hpp`, `journal.hpp`, `robot.hpp`, `gateway.cpp`, `robot.cpp` · test_cycle.cpp, test_journal.cpp |
| The hardware release holds every pump: a run or the feed pump is refused with the input that holds it, and the cycle waits with it | SD-110 | ✓ | `gateway.hpp`, `gateway.cpp`, `cycle.cpp`, `robot.cpp`, `watchdog.cpp` · test_scenarios.cpp |
| A leak or high level latches; a person releases the latch only once the device no longer reports the cause, and a restart never releases it | SD-110 | ✓ | `robot.cpp` · test_scenarios.cpp |
| The stop button on the device is the Stop of the app: it holds across a restart until Resume in the app | SD-110 | ✓ | `robot.cpp` · test_scenarios.cpp |
| A calibration belongs to the pump head it was measured with: another head, none fitted or none recorded voids it, and the channel doses nothing until it is calibrated again | SD-108 | ✓ | `config.hpp`, `gateway.cpp`, `robot.cpp` · test_journal.cpp, test_scenarios.cpp |
| A channel without a pump head runs nothing, not even into a cup | SD-108 | ✓ | `gateway.cpp` · test_journal.cpp, test_scenarios.cpp |
| One pH− canister at most; the rest are nutrients | SD-108 | ✓ | `config.cpp` · test_config.cpp |
| A channel's run time into the tank (doses and line fills) per rolling 24 h stays within its head's value and never passes 60 min; a run that would pass it is refused with the reason, the check waits for it, the user may set ml per day below it, and the count survives a restart | SD-113 | ✓ | `hardware.hpp`, `config.hpp`, `config.cpp`, `gateway.cpp`, `cycle.cpp` · test_journal.cpp, test_config.cpp, test_scenarios.cpp |
| A run into the tank needs drops: none within four times a drop's time at the calibrated flow, at least 5 s, from its start or the last drop cuts it and locks the channel; a shorter run is judged at its end plus that time; a Stop is not judged | SD-111 | ✓ | `gateway.hpp`, `gateway.cpp` · test_journal.cpp, test_scenarios.cpp |
| Drops while the pump stands, past 10 s after its run, lock the channel with an alarm, and the check mixes first and learns nothing | SD-111 | ✓ | `gateway.cpp`, `robot.cpp` · test_journal.cpp, test_scenarios.cpp |
| A locked channel doses nothing into the tank until a person releases it (or the lock reports only, SD-133); a calibration or line fill into a cup still runs | SD-111 | ✓ | `gateway.cpp`, `cycle.cpp` · test_journal.cpp, test_scenarios.cpp |
| After the last run of a dose the pump runs backwards about 0.2 ml, at most 1 s, where it can; the channel's next run first fills the line the same time forwards, unbooked, so the dose stays the planned amount; a run that would be refused is refused before the refill | SD-111 | ✓ | `config.hpp`, `gateway.hpp`, `gateway.cpp` · test_journal.cpp, test_scenarios.cpp |
| Filling a line, where the outlet has a drop sensor, drips into the tank: runs of at most 60 s until the first drops fall, at most 5 min in all; it is journaled and booked like a dose, the check mixes it in first, and a restart reports it cut; without a sensor the line goes into a cup for the time given | SD-111 | ✓ | `journal.hpp`, `gateway.cpp`, `robot.cpp` · test_scenarios.cpp, test_cycle.cpp |
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

## Alarms, notifications and maintenance (SD-123, SD-124, SD-125)

| Rule | Rationale | Status | Implementation · tests |
|---|---|---|---|
| A watchdog problem is an alarm: a latch at once, anything else after 5 min; a target only while the check holds or waits; critical, warning or info by its kind | SD-123 | ✓ | `alarms.hpp`, `alarms.cpp`, `robot.cpp` · test_alarms.cpp |
| Critical alarms are pushed at once, at night too, and repeated after 5, 15 and 60 min; warnings once by day, else in the morning report; notes only there; acknowledging pauses the reminders for 12 h at most and the alarm stays | RAT-022 | ✓ | `alarms.cpp` · test_alarms.cpp, test_scenarios.cpp |
| The alarms' state survives a restart, so nothing known is pushed again | SD-123 | ✓ | `config.hpp`, `config.cpp` · test_alarms.cpp |
| Local time from the rules of the IANA time zone database, for quiet hours and the report; an unknown zone is UTC | SD-123 | ✓ | `localtime.hpp`, `localtime.cpp` · test_localtime.cpp |
| Push channels: their settings and secrets in a file of their own, never in a backup, the diagnostics or the API; no control characters in any field; a damaged file is no channel | SD-124 | ✓ | `push.hpp`, `push.cpp`, `robot.cpp` · test_push.cpp |
| One delivery per enabled channel and notification, in the device's language; the core sends nothing, the platform reports back; a channel that fails is logged once | SD-124 | ✓ | `push.cpp`, `robot.cpp`, `messages.cpp` · test_push.cpp, test_messages.cpp |
| The simulator sends through the system's curl with the secret in a private config file that is deleted after the run | SD-124 | ✓ | `sim/push.cpp` · test_push.cpp |
| Maintenance items by what the device has, each due its interval after it was done, calibrated or first had; items due within a week of the earliest join its date | SD-125 | ✓ | `service.hpp`, `service.cpp`, `config.cpp` · test_service.cpp |
| A maintenance item on its date is a note in the morning report and the app until it is done; the watchdog stays as it is | SD-125 | ✓ | `robot.cpp` · test_service.cpp |

## Plants and phases (SD-128)

| Rule | Rationale | Status | Implementation · tests |
|---|---|---|---|
| The leading running plant's phase sets the water plan: its recipe, and its own EC target and pH range where it has them; without a running plant the recipe chosen by hand; once the last plant is completed the robot holds | SD-128 | ✓ | `plants.cpp`, `cycle.cpp`, `watchdog.cpp`, `robot.cpp` · test_plants.cpp |
| A plant copies its plant recipe's phases when it starts; day 1 is the day it started; a phase moves on by itself at the local midnight after its days, also after the device was off | SD-128 | ✓ | `plants.cpp`, `robot.cpp` · test_plants.cpp |
| A phase that waits for its confirmation stays after its days and reminds daily, as the last phase does until the harvest is recorded; only the user records the harvest | RAT-002 | ✓ | `plants.cpp`, `robot.cpp` · test_plants.cpp |
| Logic reads a phase's parameters, never its name; CI refuses phase names in the core | RAT-076 | ✓ | `plants.cpp`, `arch_check.sh` · test_plants.cpp |
| The harvest is an event of a running plant; completing it is a step of its own | RAT-077 | ✓ | `config.hpp`, `robot.cpp` · test_plants.cpp |
| The cultivation system is one of a fixed list per tank; it sets what the app offers, never the water plan | SD-128 | ✓ | `config.hpp`, `config.cpp`, `robot.cpp` · test_plants.cpp, robot.spec.ts |
| The calendar lists the next 8 weeks by local day: phases that begin, a phase that waits for confirmation, planned ends and the maintenance dates; what is past its plan stands at today | SD-128 | ✓ | `plants.cpp`, `robot.cpp` · test_plants.cpp, robot.spec.ts |

## Sign-in and access (SD-129)

| Rule | Rationale | Status | Implementation · tests |
|---|---|---|---|
| An access token is shown once and kept as a hash; it has one of three roles, never the owner's; a revoked token is refused at once; a password change ends the sessions, not the tokens | SD-129 | ✓ | `auth.cpp`, `robot.cpp` · test_auth.cpp, test_api.cpp |
| A call outside a token's role is refused: an app token never reaches the password or the tokens, an account token never limits, releases, calibration, single pumps or settings, a peer token only reads; a token never goes into a cookie | SD-129 | ✓ | `api.cpp`, `sim/main.cpp` · test_api.cpp, robot.spec.ts |
| The device ID is random, made once, kept across a restart and never in a backup | SD-129 | ✓ | `robot.cpp` · test_api.cpp, robot.spec.ts |

## Commissioning (SD-129)

| Rule | Rationale | Status | Implementation · tests |
|---|---|---|---|
| The steps of #10 are kept in order and resume after a restart; a step the robot sees is done while it sees it and is never confirmed by hand; the user confirms the placement and the test run, and skips only an optional step | SD-129 | ✓ | `robot.cpp`, `config.cpp` · test_api.cpp, robot.spec.ts |
| A run that went through a channel's line to its end counts as that line filled | SD-129 | ✓ | `robot.cpp`, `gateway.cpp` · test_api.cpp |
| Until the owner confirmed the placement, the cycle is off and nothing runs towards the tank (dose, line fill, refill); into a cup it may; a backup never brings the confirmation | SD-118 | ✓ | `cycle.cpp`, `gateway.cpp`, `robot.cpp` · test_journal.cpp, test_api.cpp |

## MQTT and Home Assistant (SD-130)

| Rule | Rationale | Status | Implementation · tests |
|---|---|---|---|
| MQTT is off by default; values go out also without TLS, commands only over TLS with a user; the password stays in the device's own file and is never read back | SD-130 | ✓ | `mqtt.cpp`, `robot.cpp` · test_mqtt.cpp, robot.spec.ts |
| A command over MQTT is a call of the account role with the same checks as one from the app; there is none for a limit, a release, a calibration or a single pump; each known command is logged | SD-130 | ✓ | `api.cpp` · test_mqtt.cpp |
| A value the robot cannot use goes to the broker as null, never 0 (R5) | SD-130 | ✓ | `mqtt.cpp` · test_mqtt.cpp |
| The simulator's connection has no TLS, so it takes no commands; it says it is online with "offline" as its last will, and a refusal or missing TLS shows in the settings | SD-130 | ✓ | `sim/mqtt_link.cpp` · test_mqtt_link.cpp |

## Coupling (SD-131)

| Rule | Rationale | Status | Implementation · tests |
|---|---|---|---|
| Two devices pair only with the code the second one shows: 10 minutes, voided by the fifth wrong try; each then holds a peer token for the other; never with itself, at most one peer | SD-131 | ✓ | `robot.cpp`, `api.cpp` · test_coupling.cpp, robot.spec.ts |
| The peer and its token live apart from the configuration and never in a backup or an answer of the API; ending the coupling revokes the tokens on both sides, and only the peer's own token may end it there | SD-131 | ✓ | `robot.cpp`, `coupling.cpp` · test_coupling.cpp |
| The follower plans nothing: its feed pump runs while the leader's does, with every guard of a check, and it doses only on the leader's request through its own gateway, one run at a time | SD-131 | ✓ | `cycle.cpp`, `robot.cpp` · test_coupling.cpp |
| 10 s without the leader's call cut a run it asked for, book it, and refuse new ones until the leader calls again; after a restart the follower waits for a call (R6) | SD-131 | ✓ | `robot.cpp` · test_coupling.cpp |
| The leader doses a canister on the follower's channel by the follower's calibration and books the result; a run there that does not end in time is booked as run; never more than one channel runs across both, and the follower's own channel jobs wait for the leader's maintenance | SD-131, PD-457 | ✓ | `gateway.cpp`, `robot.cpp`, `cycle.cpp` · test_coupling.cpp |
| Without the follower its canisters are not planned; the watchdog shows the link and a difference of more than 0.3 pH or 0.2 mS/cm for 10 min between the devices | SD-131 | ✓ | `robot.cpp`, `watchdog.cpp` · test_coupling.cpp |

## Network devices (SD-132)

| Rule | Rationale | Status | Implementation · tests |
|---|---|---|---|
| An output of a network device may switch only once "off after power loss", "never on by itself", its own input detached and, for a pulse role, an auto-off at 1.11 × its longest run read back as written; they are read back before every switch-on, written once more where they differ, and where they still differ it is not switched on | RAT-019 | ✓ | `devices.cpp`, `gateway.cpp` · test_devices.cpp |
| A role without a device has no value; an assigned device that does not answer, a stale or unreadable value give none either, and the watchdog counts each as a problem | RAT-024 | ✓ | `devices.cpp`, `robot.cpp`, `watchdog.cpp` · test_devices.cpp |
| Only the gateway switches a network output (R1, checked by `arch_check.sh`), and an answer counts only for the request it answers; off goes at once and again while the device says on; a Stop and a restart switch every output off; an output on without a check, bound anew, or whose device was away is switched off and checked before it switches on; an output let go is switched off until its device answers, with an alarm after a minute | SD-132, R1, R6 | ✓ | `gateway.cpp`, `robot.cpp` · test_devices.cpp |
| Outputs are switched only on a Shelly of the second generation or later, known by its own ID: another device at its address is offline; values come from a Shelly, ESPHome's web server or an MQTT topic, MQTT only while connected and its availability says online, and after an outage only once it comes again; passwords live apart, never in a backup or an answer of the API; an import or a removal forgets what moved or went | SD-132 | ✓ | `devices.cpp`, `config.cpp`, `robot.cpp` · test_devices.cpp |

## Refill valve (SD-132)

| Rule | Rationale | Status | Implementation · tests |
|---|---|---|---|
| A fill opens once each time "top up water" is raised, or by a person's "Fill now" (a session or an app token, never an account token or MQTT); not during Stop, maintenance or a hold, before the placement is confirmed, while the refill is latched, or without a float switch that says "not full"; a request waits while a dose or a job runs and lapses otherwise; while it fills, the check doses nothing and starts no round, and after it the check mixes the water in first | SD-132, SD-115 | ✓ | `refill.cpp`, `robot.cpp`, `cycle.cpp` · test_refill.cpp |
| A fill ends when the float says full; Stop, maintenance, a hold, a person's stop, a change of valve or float, or a valve that did not open end it without a latch, and the check goes on; during a fill no backup is restored; a restart switches the valve off, reports the cut fill, mixes first and never resumes it | SD-132, R6 | ✓ | `refill.cpp`, `robot.cpp` · test_refill.cpp |
| A fill that reaches its longest time without "full", whose float loses its value, or whose valve stops answering while open latches the refill until a person releases it | RAT-031 | ✓ | `refill.cpp`, `robot.cpp` · test_refill.cpp |
| The gateway keeps the valve closed unless the float says "not full", and closes it at full, when the float loses its value, or past the longest fill, whatever wants it open; a want it cannot follow, and one whose device was away, ends: it opens only when wanted anew. A float over MQTT needs an availability topic | RAT-032 | ✓ | `gateway.cpp`, `config.cpp` · test_refill.cpp, test_devices.cpp |
| The circulation pump runs while mixing (during a dose, its mixing wait and a fill, never while the check measures) or always; never during Stop, maintenance or a hold, nor on a follower, and after a restart only once switched on again after its check; the mixing wait stays as long | SD-132, R6, RAT-044 | ✓ | `robot.cpp`, `gateway.cpp` · test_circulation.cpp, robot.spec.ts |
| The longest fill is 1 to 25 min, 15 by default; the device's own auto-off is 28 min | RAT-079 | ✓ | `config.cpp`, `devices.cpp`, `refill.cpp`, `gateway.cpp` · test_refill.cpp, test_devices.cpp |

## Locks that report only (SD-133)

| Rule | Rationale | Status | Implementation · tests |
|---|---|---|---|
| A lock a person releases may be set to report only; leak, high level and the cable check never, whatever a configuration says. Only a session or an app token sets it, never an account token, MQTT or the coupled device, and each change is an event, also one a backup brings | SD-133, PD-459 | ✓ | `config.cpp`, `api.cpp`, `robot.cpp` · test_locks.cpp |
| Set to report only, the latch alarms with its own level and holds nothing: the feed pump tries again, an outlet without drops runs to its end and its channel doses on, a tank volume counts as confirmed; the frame (flow before a dose, one channel at a time, run time, daily amount) holds all the same | SD-133 | ✓ | `cycle.cpp`, `gateway.cpp`, `robot.cpp` · test_locks.cpp |
| While a lock reports only, the watchdog names it in a note, not as a problem; a report is cleared without restarting the check, a latch set while it locked holds until released, and once a lock locks again the next one holds | SD-133, RAT-062 | ✓ | `watchdog.cpp`, `robot.cpp` · test_locks.cpp, robot.spec.ts |

## History and messages

| Rule | Rationale | Status | Implementation · tests |
|---|---|---|---|
| The history keeps gaps as gaps, also as CSV; pH, EC and temperature only with flow | RAT-016 | ✓ | `robot.cpp`, `api.cpp` · test_api.cpp, robot.spec.ts |
| A message stays short enough to become a notification | RAT-045 | ✓ | `messages.cpp` · i18n_keys.test.mjs |
| Four levels: 10 s for a day, 1 min for a week, 15 min for a year, 1 h for five years; a snapshot from before the hourly level is read and that level worked out from its 15-min one | SD-126 | ✓ | `history.hpp`, `history.cpp` · test_history.cpp |

## Simulator

| Rule | Rationale | Status | Implementation · tests |
|---|---|---|---|
| The simulator's noise is measured; anything else is marked as an assumption | RAT-082 | ◐ | `world.cpp` · no test of its own |
