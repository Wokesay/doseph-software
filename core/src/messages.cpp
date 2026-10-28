// SPDX-License-Identifier: AGPL-3.0-or-later
// English templates of the texts the robot sends. One entry per line as
// {"key", "template"}: tools/i18n_keys.test.mjs reads this table and
// compares keys and placeholders with the German one in web/src/lang/msg.ts.
#include "doseph/messages.hpp"

#include <cmath>
#include <cstdio>
#include <unordered_map>

namespace doseph {

namespace {

struct Entry {
  const char* key;
  const char* text;
};

// clang-format off
constexpr Entry kMessages[] = {
    // The cycle (cycle.cpp)
    {"cycle.off.no_recipe", "No recipe in force – choose one under Recipe"},
    {"cycle.off.ended", "{name} has ended – choose the next recipe"},
    {"cycle.off.no_volume", "Enter the tank volume first"},
    {"cycle.stopped", "Stopped – everything is off. Tap Resume to continue"},
    {"cycle.suction_on", "Switching the suction pump on"},
    {"cycle.wait_flow", "Waiting for flow"},
    {"cycle.prove_flow", "Checking the flow: {flow:1} L/min"},
    {"cycle.flush", "Flushing the measuring chamber · {left} left"},
    {"cycle.measure", "Measuring until the readings are steady · {left}"},
    {"cycle.remeasure", "Measuring the result until steady · {left}"},
    {"cycle.measure_wait_ec", "Measuring: waiting for an EC reading"},
    {"cycle.ph_rest", "pH waits {left} after the nutrient dose"},
    {"cycle.dose_ec", "Dosing nutrients: EC {from:2} → {aim:2} · round {round} · {ml:1} ml"},
    {"cycle.dose_ec_test", "Test dose: {ml:1} ml of nutrients to check the tank volume (EC {from:2})"},
    {"cycle.dose_catchup", "Catching up a pair: {ml:1} ml of {names}, missing from a round cut short (EC {from:2})"},
    {"cycle.wait.catchup_part", "Waiting: {name} is missing from its pair but cannot be dosed – no other part of the pair goes in alone"},
    {"cycle.dose_ph", "Dosing pH−: pH {from:2} → {target:2} · dose {n} · {ml:1} ml"},
    {"cycle.dose_ph_test", "Test dose: {ml:2} ml pH− to check the tank volume (pH {from:2})"},
    {"cycle.mix", "Mixing in · {left} left – the clock runs only with flow"},
    {"cycle.mix_first", "Mixing in what went in before · {left} left"},
    {"cycle.mix_paused", "Mixing paused: no flow · {left} left"},
    {"cycle.hold", "On target: pH {ph:2}, EC {ec:2} mS/cm"},
    {"cycle.suction_failed", "The suction pump could not be switched on: {reason}"},
    {"cycle.wait.calibrating", "Waiting: a probe is being calibrated"},
    {"cycle.wait.ec", "Waiting: EC – {reason}"},
    {"cycle.wait.ec_plan", "Waiting: {reason}"},
    {"cycle.wait.stock", "Not enough in {name}: {need:1} ml needed, {have:0} ml left by the count – refill it and mark it refilled"},
    {"cycle.wait.channel_locked", "Waiting: channel {channel} of {name} is locked – release it under Status once its cause is fixed"},
    {"cycle.wait.stock_empty", "{name} is used up by the count – refill it and mark it refilled, or enter the amount left"},
    {"cycle.wait.ph_plan", "Waiting: {reason}"},
    {"cycle.wait.no_flow", "Waiting: the suction pump is on, but nothing flows – check the pump, hoses and filter"},
    {"cycle.wait.dry", "Waiting: no flow in three tries – release the dry-run lock under Status once fixed"},
    {"cycle.wait.nothing_dosed", "Waiting: nothing could be dosed"},
    {"cycle.wait.channel_job", "Waiting: a channel is being calibrated or primed"},
    {"cycle.wait.unsteady", "Waiting: the readings did not settle – the next check tries again"},
    {"cycle.wait.volume", "Dosing paused: the test dose suggests about {implied:0} L, not {entered:0} L – check the tank volume"},
    {"cycle.note.dose_failed", "A dose stopped: {reason}"},
    {"cycle.note.ec_above", "EC {ec:2} mS/cm is above the target {target:2} – the robot can't dilute; fresh water lowers it"},
    {"cycle.note.ec_latched", "EC dosing is locked: EC didn't rise after dosing – check canisters, pumps and probe, then release"},
    {"cycle.note.ec_max", "EC: {n} rounds without reaching the target – the next check tries again"},
    {"cycle.note.no_ec_target", "{name} has no EC target – no nutrients are dosed"},
    {"cycle.note.no_ph_down", "No pH− canister on a channel – pH is not corrected"},
    {"cycle.note.no_ph_range", "{name} has no pH range – pH is not corrected"},
    {"cycle.note.ph_below", "pH {ph:2} is below the target {target:2} – the robot only lowers pH; raise it by hand if needed"},
    {"cycle.note.ph_gate", "pH waits: EC {ec:2} mS/cm is below {floor:2} – a pH probe reads unreliably in such weak solution"},
    {"cycle.note.ph_invalid", "pH: {reason}"},
    {"cycle.note.ph_latched", "pH dosing is locked: pH didn't fall after dosing – check the pH− canister, pump and probe, then release"},
    {"cycle.note.ph_max", "pH: {n} doses without reaching the target – the next check tries again"},
    // Maintenance mode
    {"maint.minutes", "Maintenance lasts 1 to {max:0} minutes, or 0 to end it"},
    {"cycle.wait.maintenance", "Maintenance: the robot doses nothing and the suction pump is off until it ends"},
    {"ev.maintenance", "Maintenance started"},
    {"ev.maintenance.text", "For {min:0} min the robot doses nothing, the suction pump is off and jumps of the probes count as the work."},
    {"ev.maintenance_end", "Maintenance ended"},
    {"ev.maintenance_over", "Maintenance time is over"},
    {"watch.maintenance", "Maintenance: not judged"},
    {"cycle.note.ph_too_small", "The pH− dose ({ml:2} ml) is too small for the pump – nothing dosed"},
    // Why the robot doses or not (MixCycle::checks)
    {"check.stopped", "Stopped: nothing runs until Resume"},
    {"check.not_stopped", "Not stopped"},
    {"check.recipe", "Recipe in force: {name}"},
    {"check.volume", "Tank volume: {volume:0} L"},
    {"check.volume_fits", "The tank volume fits the test dose, or none was made yet"},
    {"check.volume_latched", "The test dose did not fit the tank volume – correct or confirm it"},
    {"check.flow", "Water flowed at the last check"},
    {"check.no_flow", "No flow at the last check – the robot tries again"},
    {"check.no_job", "No calibration running"},
    {"check.channel_job", "A channel is being calibrated or primed"},
    {"check.probe_job", "A probe is being calibrated"},
    {"check.ec_target", "EC target {target:2} mS/cm"},
    {"check.parts_ready", "Every part of the recipe is on a calibrated channel"},
    {"check.stock_ok", "No canister is used up by the count"},
    {"check.stock_empty", "{name} is used up by the count"},
    {"check.part_not_ready", "{name} is on no channel, or its channel is not calibrated"},
    {"check.ec_ok", "The EC reading is valid"},
    {"check.ec_bad", "EC: {reason}"},
    {"check.no_latch", "No lock to release"},
    {"check.ph_target", "pH target {target:2}"},
    {"check.ph_ok", "The pH reading is valid"},
    {"check.ph_bad", "pH: {reason}"},
    {"check.ph_down", "pH− ready: {name}"},
    {"check.no_ph_down", "No pH− canister on a calibrated channel"},
    {"check.gate_ok", "EC {ec:2} is above {floor:2}: pH can be read"},
    {"check.gate_low", "EC {ec:2} is below {floor:2}: pH can't be read like this, nutrients first"},
    {"check.rest_over", "The rest after a nutrient dose is over"},
    {"check.rest_left", "Rest after the nutrient dose: {left} left"},
    {"check.ec_first", "Nutrients are being dosed: pH comes after"},
    {"check.ec_done", "No nutrient round open"},

    // Planning (plan.cpp)
    {"dose.no_flow", "the channel is not calibrated"},
    {"dose.no_amount", "no amount"},
    {"dose.too_small", "{ml:2} ml would run under {s} s – too imprecise"},
    {"dose.too_large", "{ml:1} ml would need more than {runs} runs"},
    {"dose.no_channel", "{name} is on no channel"},
    {"dose.part", "{name}: {reason}"},
    {"ec.no_volume", "tank volume unknown"},
    {"ec.effect_unknown", "the recipe's effect is unknown"},
    {"ec.recipe", "the recipe has no amounts"},
    {"ec.recipe_no_scaled", "{name} has only fixed amounts – no EC can be raised with it"},
    {"ec.reached", "EC is not below the target"},
    {"ph.no_value", "pH value missing"},
    {"ph.no_volume", "tank volume unknown"},
    {"ph.reached", "pH is not above the target"},

    // Gateway and doser (gateway.cpp)
    {"act.stopped", "Stop active – tap Resume first"},
    {"act.busy", "another channel is running"},
    {"act.no_channel", "channel {channel} does not exist"},
    {"act.too_long", "a run may last at most {s} s"},
    {"act.too_short", "a run under {s} s is too imprecise"},
    {"act.uncalibrated", "the channel is not calibrated"},
    {"act.no_head", "channel {channel} has no pump head"},
    {"act.head_changed", "the pump head changed since the channel was calibrated"},
    {"act.channel_locked", "channel {channel} is locked: release it once its cause is fixed"},
    {"act.no_reverse", "the pump of channel {channel} cannot run backwards"},
    {"act.no_flow", "no flow through the device – no dose without flow"},
    {"act.purpose", "unknown kind of run"},
    {"act.hw", "the device refused: {error}"},
    {"purpose.ec", "Nutrient round"},
    {"purpose.ph", "pH correction"},
    {"purpose.calibration", "Channel calibration"},
    {"purpose.prime", "Priming"},
    {"purpose.fill", "Line fill"},
    {"purpose.other", "Run"},
    {"dose.busy", "Another dose is running"},
    {"dose.empty", "Nothing to dose"},
    {"dose.no_response", "{name}: the pump did not report back – switched off"},
    {"dose.disconnected", "the pump driver disconnected"},
    {"dose.lost", "{name}: run lost – {reason}"},
    {"dose.run_cancelled", "the run was cancelled"},
    {"dose.failed", "{name}: run failed – {reason}"},
    {"dose.fill_no_drops", "{name}: no drops after filling for {s:0}\u00a0s – check the canister, the line and the pump head"},
    {"dose.aborted", "{name}: stopped – {reason}"},
    {"abort.interrupted", "the check started anew"},
    {"abort.stop", "Stop"},
    {"abort.user", "cancelled"},
    {"ev.dose", "{name}: {ml:1} ml"},
    {"ev.dose_incomplete", "{name}: only {ml:1} ml"},
    {"ev.flow_cut", "Dose stopped: no flow"},
    {"ev.flow_cut.text", "The flow through the device stopped while a pump dosed. What went in is mixed in before the next measurement."},

    // Sensor truth (truth.cpp); a jump lock's reason stays short enough for a notification (RAT-045)
    {"truth.offline", "The measuring part does not answer"},
    {"truth.no_data", "No reading yet"},
    {"truth.stale", "No new reading for {ageS} s (limit {limitS} s)"},
    {"truth.uncalibrated", "Not calibrated"},
    {"truth.ec_no_temp", "Not temperature compensated: water temperature – {reason}"},
    {"truth.frozen", "The raw value has not moved for 15 min – check the probe"},
    {"truth.implausible", "{value}{unit} is outside {min}–{max}{unit} – check the probe"},
    {"truth.jump", "The value jumped from {from}{unit} to {to}{unit}. If nobody worked at the tank, check the probe. The lock lifts after 15 min steady."},
    {"truth.jump_ph_fall", "pH fell from {from} to {to}. Measure by hand; if it is too low, raise it yourself – the robot only lowers pH. The lock lifts after 15 min steady."},
    {"truth.ok", "OK"},
    {"calib.ph_slope", "The two buffers give an implausible slope – check the probe and the buffers"},
    {"calib.ph_zero", "The probe's zero point is {mv} mV off (±30 mV at most) – clean the probe and use fresh buffers; if that doesn't help, replace it"},
    {"calib.ec_factor", "The reading is too far from the reference – check the probe and the solution"},

    // Watchdog (watchdog.cpp)
    {"watch.ph", "pH probe"},
    {"watch.ec", "EC probe"},
    {"watch.temp", "Water temperature"},
    {"watch.flow", "Flow"},
    {"watch.reading_ok", "OK"},
    {"watch.flow_ok", "{flow:1} L/min"},
    {"watch.flow_none", "No flow – check the suction pump"},
    {"watch.flow_not_needed", "Not needed right now"},
    {"watch.target_ec", "EC"},
    {"watch.target_ph", "pH"},
    {"watch.target_ok", "{value:2} (target {target:2})"},
    {"watch.target_high", "{value:2} is above the target {target:2} by more than {band:2}"},
    {"watch.target_low", "{value:2} is below the target {target:2} by more than {band:2}"},
    {"watch.no_target", "No target"},
    {"watch.grace", "Starting up"},
    {"watch.no_measurement", "No recent measurement"},
    {"watch.stopped", "Stopped"},
    {"watch.stop", "Stop"},
    {"watch.latch", "Lock"},
    {"watch.recipe", "Recipe"},
    {"watch.no_recipe", "No recipe in force"},
    {"watch.recipe_ended", "{name} has ended – choose the next recipe"},
    {"watch.recipe_ok", "{name}"},
    {"watch.volume", "Tank volume"},
    {"watch.no_volume", "Tank volume missing"},
    {"watch.channels", "Channels"},
    {"watch.no_ph_down", "No pH− canister on a channel"},
    {"watch.no_channel", "{name} is on no channel"},
    {"watch.channel_uncalibrated", "{name}: channel {channel} is not calibrated"},
    {"watch.stock", "Level of {name}"},
    {"watch.stock_ok", "{ml:0} ml left by the count"},
    {"watch.stock_low", "{ml:0} ml left – refill soon"},
    {"watch.stock_empty", "{ml:0} ml by the count – canister empty? Refill it, or enter the amount left"},
    {"watch.headline_ok", "All good ({n} checks)"},
    {"watch.headline_more", "{first} (+{more} more)"},
    {"latch.ec.no_effect", "EC dosing locked: EC didn't rise after dosing"},
    {"latch.ph.no_effect", "pH dosing locked: pH didn't fall after dosing"},
    {"latch.tank.volume", "Check the tank volume: the test dose suggests about {impliedL:0} L, not {enteredL:0} L"},

    // Robot (robot.cpp)
    {"ev.boot", "Started"},
    {"ev.boot.stopped", "The Stop is still active – resume when ready"},
    {"ev.boot.state_unknown", "Saved state unreadable – stopped"},
    {"ev.boot.state_unknown.text", "The robot could not read its saved state or settings, so it does not know whether a lock stood. It stays stopped: check the settings and the tank, then resume."},
    {"ev.boot.cut_dose", "{name}: dose cut by a restart"},
    {"ev.boot.cut_dose.text", "Up to {ml:1} ml of {name} may be in the tank – how much is unknown. The next check mixes the tank first and then measures."},
    {"ev.boot.cut_cup", "Channel {channel}: calibration or priming cut by a restart"},
    {"ev.boot.cut_cup.text", "Nothing went into the tank. Run it again."},
    {"ev.boot.mix_first", "Mixing first"},
    {"ev.boot.mix_first.text", "Doses from before the restart are not mixed in and measured yet: the first check mixes {min:0} min with flow before it measures, and learns nothing from them."},
    {"ev.config.changed", "Settings changed: {what}"},
    {"ev.config.imported", "Settings restored from a backup"},
    {"cfg.what.system", "device"},
    {"cfg.what.tank", "tank"},
    {"cfg.what.control", "control"},
    {"cfg.what.canisters", "canisters"},
    {"cfg.what.recipes", "recipes"},
    {"ev.tank.new", "New tank"},
    {"ev.tank.new.text", "The next dose is a small test dose that checks the tank volume."},
    {"ev.active.set", "Recipe {name} in force"},
    {"ev.active.days", "for {days:1} days"},
    {"ev.active.open", "without an end"},
    {"ev.active.cleared", "No recipe in force"},
    {"ev.cycle.recipe_ended", "Recipe ended"},
    {"ev.cycle.ec_round", "Nutrient round {n}"},
    {"ev.cycle.ec_round.text", "EC {from:2} → {to:2} mS/cm with {ml:1} ml"},
    {"ev.cycle.ph_dose", "pH dose {n}"},
    {"ev.cycle.ph_dose.text", "pH {from:2} → {to:2} with {ml:2} ml pH−"},
    {"ev.cycle.held", "On target"},
    {"ev.cycle.held.text", "pH {ph:2}, EC {ec:2} mS/cm"},
    {"ev.cycle.no_flow", "No flow"},
    {"ev.cycle.unsteady", "Readings not steady"},
    {"ev.cycle.unsteady.text", "Within the last minute EC moved by {ec:3} mS/cm and pH by {ph:2} – the probes may be dirty or the tank is not mixed"},
    {"ev.cycle.test_ok", "Test dose: the tank volume fits"},
    {"ev.cycle.test_ok.text", "{entered:0} L entered; the test dose suggests about {implied:0} L"},
    {"ev.cycle.test_ok_ph", "Test dose: no sign of a smaller tank"},
    {"ev.cycle.test_ok_ph.text", "pH fell no more than expected for {entered:0} L"},
    {"ev.cycle.volume", "Test dose: check the tank volume"},
    {"ev.cycle.volume.text", "The test dose acted as if the tank held about {implied:0} L, not {entered:0} L. The robot doses nothing until you correct the volume or confirm it."},
    {"ev.ec.no_effect", "Nutrient dosing locked: no effect"},
    {"ev.ec.no_effect.text", "Two rounds in a row raised EC by less than 0.02 mS/cm. Check the canisters, the pumps and the EC probe, then release the lock."},
    {"ev.ph.no_effect", "pH dosing locked: no effect"},
    {"ev.ph.no_effect.text", "Two pH− doses in a row lowered pH by less than 0.03. Check the pH− canister, its pump and the pH probe, then release the lock."},
    {"ev.latch.released", "Lock released"},
    {"ev.channel.calibrated", "Channel {channel} calibrated"},
    {"ev.fill", "{name}: line filled"},
    {"ev.fill_incomplete", "{name}: line not filled"},
    {"ev.channel.head_changed", "Channel {channel}: pump head {head} – calibrate again"},
    {"ev.channel.head_removed", "Channel {channel}: no pump head"},
    {"ev.flow.dry", "Suction pump locked: dry run"},
    {"ev.channel.no_drops", "Channel {channel}: no drops"},
    {"ev.channel.siphon", "Channel {channel}: drips without its pump"},
    {"ev.stock.set", "{name}: level entered"},
    {"ev.stock.set.text", "{name} holds {ml:0} ml now."},
    {"ev.stock.below_zero", "{name}: used up by the count"},
    {"ev.stock.below_zero.text", "{name}: canister empty? Refill it; doses since it ran dry did not go in fully. If it is not empty, enter the amount left. If this keeps happening, calibrate the channel again."},
    {"ev.cycle.pair_short", "Pair incomplete: {names}"},
    {"ev.cycle.pair_short.text", "A nutrient round stopped part way: {names} missing {ml:1} ml in all. The next nutrient round doses only these first, mixes them in and measures."},
    {"ev.cycle.catchup_dropped", "Pair not caught up: {names}"},
    {"ev.cycle.catchup_dropped.text", "{ml:1} ml in all were missing from the pair; the robot no longer doses them on their own. Reason: {reason}"},
    {"catchup.recipe_changed", "the recipe in force changed its parts"},
    {"catchup.new_tank", "a new tank"},
    {"catchup.by_hand", "ended in the app"},
    {"ev.channel.calibrated.text", "{flow:1} ml/min ({ml:1} ml in {s:1} s)"},
    {"ev.fill.text", "Channel {channel} ran {s:0}\u00a0s until the first drops fell; they went into the tank, and the next check mixes them in first."},
    {"ev.channel.head_changed.text", "Its calibration was measured with another head and is void. The channel doses nothing until it is calibrated again."},
    {"ev.channel.head_removed.text", "Its calibration is void. The channel doses nothing until a head is fitted and the channel is calibrated again."},
    {"ev.flow.dry.text", "No flow in {tries} tries in a row: the suction pump may run dry. Check the suction line, the tank and the filter, then release the lock under Status."},
    {"ev.channel.no_drops.text", "Its pump ran, but nothing dripped at its outlet: the outlet or line is blocked, the canister empty, the tube kinked or the pump head failed. The run was stopped; the channel doses nothing until you release it."},
    {"ev.channel.siphon.text", "Drops fell at its outlet while its pump stood: the canister stands above the device and siphons. Put it no higher than the device. The channel doses nothing until you release it; the check mixes first."},
    {"ev.probe.start_ph", "pH calibration started"},
    {"ev.probe.start_ec", "EC calibration started"},
    {"ev.probe.saved_ph", "pH probe calibrated"},
    {"ev.probe.saved_ec", "EC probe calibrated"},
    {"ev.stop", "Stop"},
    {"ev.plain", "{text}"},
    {"ev.resume", "Resumed"},
    {"stop.app", "Stop in the app"},
    {"act.hold.leak", "a leak holds every pump"},
    {"act.hold.high_level", "high level in the measuring chamber holds every pump"},
    {"act.hold.return", "the return pump is locked: dry or blocked"},
    {"act.hold.stop_button", "the stop button on the device holds every pump"},
    {"act.hold.release", "the hardware release is off"},
    {"cycle.wait.hold.leak", "Waiting: a leak – every pump is off; fix it, then release the lock"},
    {"cycle.wait.hold.high_level", "Waiting: high level in the measuring chamber (return blocked or return pump failed) – fix it, then release the lock"},
    {"cycle.wait.hold.return", "Waiting: the return pump ran without pause (dry or blocked) – fix it, then release the lock"},
    {"cycle.wait.hold.stop_button", "Waiting: the stop button on the device is pressed"},
    {"cycle.wait.hold.release", "Waiting: the hardware release is off"},
    {"latch.hw.leak", "Leak: every pump stays off until you release the lock once the leak is fixed"},
    {"latch.hw.high_level", "High level in the measuring chamber: return blocked or return pump failed"},
    {"latch.hw.return", "Return pump held off: it ran without pause – dry or blocked"},
    {"latch.flow.dry", "Dry run: no flow in {tries} tries – suction line out of the water, tank empty or filter blocked"},
    {"latch.channel.no_drops", "Channel {channel} locked: no drops at its outlet – outlet or line blocked, canister empty, tube kinked or pump head failed"},
    {"latch.channel.siphon", "Channel {channel} locked: it dripped without its pump – a canister stands above the device"},
    {"ev.hw.leak", "Leak – every pump off"},
    {"ev.hw.leak.text", "The device reports a leak. Everything is off and stays off until the leak is fixed and you release the lock."},
    {"ev.hw.high_level", "High level in the measuring chamber"},
    {"ev.hw.high_level.text", "The return is blocked or the return pump failed. The suction pump and dosing stay off until the chamber has drained and you release the lock."},
    {"ev.hw.return", "Return pump dry or blocked"},
    {"ev.hw.return.text", "The return pump ran for a minute without a pause: it moves no water. It is held off, and the suction pump and dosing stay off until you fix it and release the lock."},
    {"stop.button", "The stop button on the device"},
    {"latch.cause_present", "Not released: the device still reports it"},
    {"watch.release", "Hardware release"},
    {"watch.hold.stop_button", "The stop button on the device is pressed"},
    {"watch.hold.release", "The hardware release is off"},
    {"store.failed", "Not saved: the device cannot write its storage"},
    {"config.not_object", "This file is not a settings backup – choose one saved with \"Back up settings\""},
    {"config.newer", "This backup comes from a newer version – update the robot first, then restore it"},
    {"config.import", "This file can't be restored – it is damaged or not a settings backup ({reason})"},
    {"config.busy", "Not restored: a dose, a pump run or a probe calibration is going on – try again when it is done"},
    {"canister.in_recipe", "The recipe {name} uses this canister – remove it there first"},
    {"canister.stock", "The level can't be below 0 ml – enter what is in the canister"},
    {"canister.no_capacity", "{name} has no size – enter its size first, or the amount in it"},
    {"recipe.in_force", "This recipe is in force – choose another one first"},
    {"active.days", "Between {min} and {max} days"},
    {"active.until", "The end must lie in the future, at most a year ahead"},
    {"channel.n", "Channel {channel}"},
    {"job.busy", "A calibration or priming is running – wait for it or cancel it"},
    {"job.cycle_dosing", "The robot is dosing right now – try again in a moment"},
    {"job.seconds", "Between 1 and {max} s"},
    {"job.measure_cup", "The pump ran {s:1} s – enter how many ml are in the cup"},
    {"job.primed", "Tube primed"},
    {"job.filled", "Channel {channel}: the line is full ({s:0}\u00a0s)"},
    {"job.no_result_wanted", "No calibration is waiting for a result"},
    {"job.ml", "Enter the amount in the cup: more than 0, at most {max} ml"},
    {"job.no_run", "The pump did not run – start the calibration again"},
    {"job.calibrated", "Channel {channel}: {flow:1} ml/min saved"},
    {"probe.kind", "Calibrate pH or EC"},
    {"probe.none", "No calibration is open"},
    {"probe.no_raw", "No fresh reading from the probe"},
    {"probe.ph_ref", "Use a buffer between pH 1 and 13"},
    {"probe.ph_same", "Use two buffers at least 1 pH apart"},
    {"probe.ph_two", "Measure two buffers first"},
    {"probe.ec_ref", "The reference is between 0.1 and 20 mS/cm – its value at 25 °C, as printed on the bottle"},
    {"probe.no_temp", "No water temperature: {reason} – EC can't be calibrated without it"},
    {"probe.ec_zero", "The probe reads no conductivity – is it in the solution?"},
    {"probe.ec_one", "Measure the reference solution first"},
    {"probe.action", "Unknown step"},
    {"latch.none", "There is no such lock"},
    {"latch.volume_confirm", "Confirm the volume, or correct it under Tank"},

    // Configuration (config.cpp)
    {"cfg.system.name", "The device name has 1 to 40 characters"},
    {"cfg.system.language", "Language: en or de"},
    {"cfg.system.timezone", "That is not a time zone name"},
    {"cfg.limits.min_run", "The shortest run must be between 1 and 10 s"},
    {"cfg.limits.max_run", "The longest run is at most 60 s and not below the shortest"},
    {"cfg.limits.runs", "Partial runs per canister: 1 to 6"},
    {"cfg.control.ec_tolerance", "The EC tolerance must be between {min} and {max} mS/cm"},
    {"cfg.control.ph_tolerance", "The pH tolerance must be between {min} and {max}"},
    {"cfg.control.max_ec_step", "The largest EC rise per round must be between {min} and {max} mS/cm"},
    {"cfg.control.ec_floor", "The EC below which pH waits must be between {min} and {max} mS/cm"},
    {"cfg.control.ph_max_ml", "The largest pH− dose must be between {min} and {max} ml"},
    {"cfg.control.ec_rounds", "Nutrient rounds per check: {min} to {max}"},
    {"cfg.control.ph_doses", "pH doses per check: {min} to {max}"},
    {"cfg.tank.volume", "The tank volume must be between {min} and {max} L"},
    {"cfg.tank.mix_min", "The least mixing wait after a dose must be between {min} and {max} min"},
    {"cfg.tank.hold", "The pause between checks must be between {min} and {max} min"},
    {"cfg.canister.id", "Every canister needs an ID of its own"},
    {"cfg.canister.name", "A canister needs a name of at most 40 characters"},
    {"cfg.canister.role", "{name}: the role is nutrient or pH−"},
    {"cfg.canister.ph_down_twice", "{name}: there is one pH− canister at most"},
    {"cfg.canister.channel", "{name}: the channel is 1 to {max}, or none"},
    {"cfg.canister.channel_twice", "{name}: channel {channel} already carries another canister"},
    {"cfg.canister.pair_role", "{name}: only nutrients form a pair"},
    {"cfg.canister.pair_name", "{name}: a pair's name has at most 8 characters"},
    {"cfg.canister.capacity", "{name}: the size must be from {min} to {max} ml"},
    {"cfg.pair.single", "Pair {pair} has only one canister – a pair needs all its parts"},
    {"cfg.recipe.id", "{name}: the recipe's ID is missing or used twice"},
    {"cfg.recipe.name", "A recipe needs a name of at most 60 characters"},
    {"cfg.recipe.empty", "{name} has no parts"},
    {"cfg.recipe.canister", "{name}: a part names a canister that does not exist"},
    {"cfg.recipe.twice", "{name}: {part} is in it twice"},
    {"cfg.recipe.ph", "{name}: {part} is pH correction – it never belongs in a recipe; the robot corrects pH last"},
    {"cfg.recipe.amount", "{name}: {part} needs an amount in ml/L"},
    {"cfg.recipe.fixed_pair", "{name}: {part} is part of a pair and can't have a fixed amount"},
    {"cfg.recipe.pair", "{name}: pair {pair} goes into the recipe with all its parts"},
    {"cfg.recipe.ec_range", "{name}: the EC target must be between {min} and {max} mS/cm"},
    {"cfg.recipe.ec_fit", "{name}: EC {ec:2} does not fit these amounts – between {low:1} and {high:1} mS/cm would fit"},
    {"cfg.recipe.ph_range", "{name}: the pH range lies between {min} and {max}, the lower value first"},
    {"cfg.active.recipe", "The recipe in force does not exist"},
    {"cfg.active.until", "The recipe's end must come after its start"},
    {"cfg.channel.number", "Channel {channel} does not exist on this device"},
    {"cfg.channel.flow", "Channel {channel}: the calibration value is invalid"},
    {"cfg.calibration.ph", "The pH calibration in the backup is not usable: {reason}"},
    {"cfg.calibration.ec", "The EC calibration in the backup is not usable: {reason}"},

    // API and sign-in (api.cpp, auth.cpp)
    {"api.bad_input", "The input has the wrong format"},
    {"api.internal", "Internal error"},
    {"api.not_found", "Unknown path"},
    {"api.json", "Invalid JSON"},
    {"api.origin", "Request from a foreign origin refused"},
    {"api.too_large", "Request too large"},
    {"history.series", "Choose at least one curve"},
    {"history.series_id", "Unknown curve – ph, ec, temp or flow"},
    {"history.series_twice", "The curve {id} is asked for twice"},
    {"auth.required", "Please sign in"},
    {"auth.lost", "The robot can't read its password any more – reset it to factory settings on the device itself"},
    {"auth.kept", "The robot can't read its password file. Your password stays in it – don't reset the robot to factory settings. Restart the robot to try again."},
    {"ev.auth.kept", "Password can't be read"},
    {"auth.code", "Setup code missing or wrong. Use the newest one – every restart makes a new one."},
    {"auth.too_short", "At least 8 characters"},
    {"auth.too_long", "At most 128 characters"},
    {"auth.exists", "A password is already set – reload the page and sign in"},
    {"auth.wrong_old", "The current password is wrong"},
    {"auth.locked", "Too many failed attempts. Please wait {waitS} s."},
    {"auth.no_password", "No password set yet – reload the page to set one"},
    {"auth.wrong", "Wrong password"},
    {"ev.auth.password_set", "Password set"},
    {"ev.auth.first_setup", "During initial setup"},
    {"ev.auth.code_wrong", "Setup code refused"},
    {"ev.auth.code_wrong.text", "Wrong setup codes since the start: {count}. If it was not you, another device on your network tried – in vain."},
    {"ev.auth.failed", "Sign-in failed"},
    {"ev.auth.signed_in", "Signed in"},
    {"ev.auth.password_changed", "Password changed"},
    {"ev.auth.password_changed.text", "Signed out on all devices"},
};
// clang-format on

const std::unordered_map<std::string_view, std::string_view>& table() {
  static const std::unordered_map<std::string_view, std::string_view> t = [] {
    std::unordered_map<std::string_view, std::string_view> m;
    for (const auto& e : kMessages) m.emplace(e.key, e.text);
    return m;
  }();
  return t;
}

// Rounds the exact value half away from zero, like toFixed in the web app,
// so both languages show the same digits (20.25 → 20.3, 20.65 → 20.6).
std::string number(double v, int decimals) {
  if (!isNum(v)) return "–";
  char buf[352];
  if (decimals >= 0) {
    // printf rounds the exact value; only an exact tie (20.25 at one
    // decimal) goes to even there, so a tie is moved a quarter unit away
    // from zero first.
    std::snprintf(buf, sizeof buf, "%.60f", std::fabs(v));
    const std::string exact(buf);
    const size_t next = exact.find('.') + 1 + static_cast<size_t>(decimals);
    const bool tie = next < exact.size() && exact[next] == '5' && exact.find_first_not_of('0', next + 1) == std::string::npos;
    const double a = tie ? std::fabs(v) + 0.25 * std::pow(10.0, -decimals) : std::fabs(v);
    std::snprintf(buf, sizeof buf, "%.*f", decimals, a);
    std::string s(buf);
    const bool zero = s.find_first_not_of("0.") == std::string::npos;
    return v < 0 && !zero ? "-" + s : s;
  }
  std::snprintf(buf, sizeof buf, "%.6f", v);
  std::string s(buf);
  if (s.find('.') != std::string::npos) {
    s.erase(s.find_last_not_of('0') + 1);
    if (s.back() == '.') s.pop_back();
  }
  return s == "-0" ? "0" : s;
}

std::string value(const json& args, const std::string& name, int decimals) {
  if (!args.is_object()) return "–";
  auto it = args.find(name);
  if (it == args.end() || it->is_null()) return "–";
  if (it->is_number()) return number(it->get<double>(), decimals);
  if (it->is_string()) return it->get<std::string>();
  if (it->is_object()) {  // a nested message; an empty one is missing, as in the web app
    const std::string text = jstr(*it, "text");
    return text.empty() ? "–" : text;
  }
  if (it->is_array()) {  // a list, e.g. amounts: "Part A 12.0 ml, Part B 8.0 ml"
    std::string out;
    for (size_t i = 0; i < it->size(); ++i) {
      if (i) out += ", ";
      out += value(json{{"v", (*it)[i]}}, "v", decimals);
    }
    return out.empty() ? "–" : out;
  }
  return "–";
}

bool nameChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

}  // namespace

std::string render(std::string_view tmpl, const json& args) {
  std::string out;
  size_t i = 0;
  while (i < tmpl.size()) {
    if (tmpl[i] == '{') {
      size_t j = i + 1;
      while (j < tmpl.size() && nameChar(tmpl[j])) ++j;
      int decimals = -1;
      size_t end = j;
      if (end < tmpl.size() && tmpl[end] == ':' && end + 1 < tmpl.size() && tmpl[end + 1] >= '0' && tmpl[end + 1] <= '9') {
        decimals = tmpl[end + 1] - '0';
        end += 2;
      }
      if (j > i + 1 && end < tmpl.size() && tmpl[end] == '}') {
        out += value(args, std::string(tmpl.substr(i + 1, j - i - 1)), decimals);
        i = end + 1;
        continue;
      }
    }
    out += tmpl[i++];
  }
  return out;
}

bool knownMessage(std::string_view key) { return table().count(key) > 0; }

bool fitsTemplate(std::string_view key, const json& args) {
  auto it = table().find(key);
  if (it == table().end()) return true;
  const std::string_view tmpl = it->second;
  for (size_t i = 0; i < tmpl.size(); ++i) {
    if (tmpl[i] != '{') continue;
    size_t j = i + 1;
    while (j < tmpl.size() && nameChar(tmpl[j])) ++j;
    if (j == i + 1 || j >= tmpl.size() || (tmpl[j] != '}' && tmpl[j] != ':')) continue;
    if (!args.is_object() || !args.contains(std::string(tmpl.substr(i + 1, j - i - 1)))) return false;
  }
  return true;
}

Msg say(std::string_view key, json args) {
  auto it = table().find(key);
  std::string text = it == table().end() ? std::string(key) : render(it->second, args);
  return {std::string(key), std::move(text), std::move(args)};
}

}  // namespace doseph
