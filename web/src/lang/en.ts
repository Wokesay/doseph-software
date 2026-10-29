// SPDX-License-Identifier: AGPL-3.0-or-later
// English texts. Every key of de.ts must exist here (checked by the compiler).
import type { de, deBase } from "./de";
import { en as login } from "./login";
import { en as status } from "./status";
import { en as recipe } from "./recipe";
import { en as tank } from "./tank";
import { en as history } from "./history";
import { en as channels } from "./channels";
import { en as settings } from "./settings";
import { en as sim } from "./sim";
import { en as water } from "./water";
import { en as plants } from "./plants";
import { en as setup } from "./setup";
import { en as watering } from "./watering";
import { en as light } from "./light";
import { en as climate } from "./climate";

const base: Record<keyof typeof deBase, string> = {
  "app.tagline": "Mixing robot for nutrient solution",
  "common.back": "Back",
  "common.name": "Name",
  "common.save": "Save",
  "common.cancel": "Cancel",
  "common.close": "Close",
  "common.gotIt": "Got it",
  "common.edit": "Edit",
  "common.delete": "Delete",
  "common.add": "Add",
  "common.more": "More",
  "common.all": "All",
  "common.on": "on",
  "common.off": "off",
  "common.none": "none",
  "common.optional": "optional",
  "common.help": "Explanation",
  "common.stop": "STOP",
  "common.live": "live",
  "common.offline": "disconnected",
  "common.connecting": "connecting …",
  "common.simulator": "Simulator",
  "common.renderError": "This part can't be shown. The robot is not affected, STOP at the top works.",
  "common.reload": "Reload page",
  "common.version": "Version {v}",
  "common.testingOnly": "For testing only: this version is a development build, not released for continuous use.",
  "common.cra":
    "It does not comply with Regulation (EU) 2024/2847 (Cyber Resilience Act) and will not be made available on the market for purposes other than testing. Each test version is superseded by the next one.",
  "common.justNow": "just now",
  "common.secondsAgo": "{n} s ago",
  "common.minutesAgo": "{n} min ago",
  "common.hoursAgo": "{n} h ago",
  "common.daysAgo": "{n} days ago",
  "common.language": "Language",
  "common.minutes": "{n} min",
  "common.suggested": "Suggested: {v}",

  "nav.status": "Status",
  "nav.recipe": "Recipe",
  "nav.plants": "Plants",
  "nav.tank": "Tank",
  "nav.water": "Water path",
  "nav.watering": "Watering",
  "nav.light": "Light",
  "nav.climate": "Climate",
  "nav.history": "History",
  "nav.channels": "Channels",
  "nav.settings": "Settings",
  "nav.label": "Menu",

  "term.volume": "Tank volume",
  "help.volume": "This many litres of nutrient solution are in the tank. The robot works out every dose for it; the first dose of a new tank is a small test dose that checks the value.",
  "term.turnover": "Turnover time",
  "help.turnover": "How long the feed pump takes to pump the tank once through the device: tank volume divided by flow.",
  "term.waitTime": "Mixing wait",
  "help.waitTime": "How long the robot circulates after a dose before it measures again: one turnover, at least 2 minutes, the same for EC and pH, then until the readings are steady. The clock runs only while water flows. You may lengthen it, never shorten it.",
  "term.holdTime": "Pause between checks",
  "help.holdTime": "Once the tank is on target, the robot rests this long and then checks again. It does not mix all the time.",
  "term.circulation": "Flow",
  "help.circulation": "The device's feed pump lies in the tank and pushes its water through the measuring chamber for each check; it stands while the robot holds or waits. If no water flows within a few seconds, it goes off, so it does not run dry or spill water through a loose hose. After two tries in a row it stays locked until you release it under Status.",
  "term.testDose": "Test dose",
  "help.testDose": "The first dose of a new tank is small. If it acts much more or much less than expected for the entered volume, the robot stops and asks about the volume.",
  "term.tolerance": "Tolerance",
  "help.tolerance": "Leeway around the target within which the robot does nothing. Too narrow means constant correcting.",
  "term.canister": "Canister",
  "help.canister": "A canister of nutrient or pH−, on a dosing channel. The robot can't see what is inside – you tell it.",
  "term.pair": "Pair (A/B)",
  "help.pair": "Two parts that only work together. The robot always doses them in a fixed ratio, also in smaller doses.",
  "term.fixed": "Fixed amount",
  "help.fixed": "This amount goes in per litre of fresh water and is not dosed when EC is raised, e.g. CalMag for soft water.",
  "term.calibratePump": "Calibrate",
  "help.calibratePump": "The pump runs for a set time into a measuring cup, you enter the ml. Without this value it doses nothing.",
  "term.prime": "Prime tube",
  "help.prime": "Pumps until the liquid reaches the end of the tube. Needed before calibrating and after every canister change. Hold the tube's end in a cup, not in the tank.",
  "term.calibrateProbe": "Calibrate probe",
  "help.calibrateProbe": "pH: dip the probe into two buffer solutions in turn (e.g. pH 7 and 4). EC: into a reference solution (e.g. 1.413 mS/cm). Without calibration the robot only shows values and doses nothing.",
  "term.latched": "Locked until released",
  "help.latched": "A dose had no effect, or the test dose did not fit the tank volume. The robot doses nothing of this kind until you have checked the cause and released it.",
  "term.jumpLock": "Jump lock",
  "help.jumpLock": "A reading jumped unexpectedly. The robot uses it again once it has been steady for 15 min – nothing to release.",

  "shell.resume": "Resume",
  "shell.resumed": "Resumed",
  "shell.stopTitle": "Stop: all pumps off",
  "shell.stopAsk": "Stop everything?",
  "shell.stopAll": "Stop everything",
  "shell.stopped": "Stopped: everything off",
  "shell.stoppedBar": "Stopped – dosing channels and feed pump are off and stay off until you resume.",
  "shell.stopText": "All pumps, valves and watering circuits go off at once, and so do heaters, humidifiers, dehumidifiers and CO2. Fans and light keep running. A running dose is cancelled, the robot rests until you choose \"Resume\".",
  "shell.stopNote": "Measuring goes on.",
  "shell.stopSending": "Sending …",
  "shell.stopRetry": "Send again",
  "shell.stopRefused": "The robot refused the Stop: {reason}",
  "shell.stopFailed": "The Stop did not get through",
  "shell.stopHint": "In an emergency, unplug the robot – then everything is off.",
  "shell.liveTitle": "Live connection to the robot",
  "shell.offlineTitle": "No connection to the robot – it keeps working",
  "shell.offlineBar": "No connection to the robot – the values are not current. It keeps working.",
  "shell.watchdogStale": "Monitoring without an assessment",
  "shell.connecting": "Connecting to the robot …",
  "shell.loading": "Loading …",
  "shell.noAnswer": "The robot does not answer. Check that it is switched on and on the same network.",
  "shell.failed": "That didn't work.",
  "shell.httpError": "Error {status}",
};

// The base texts plus the texts of each area.
export const en: Record<keyof typeof de, string> = { ...base, ...login, ...status, ...recipe, ...tank, ...history, ...channels, ...settings, ...sim, ...water, ...plants, ...setup, ...watering, ...light, ...climate };
