// SPDX-License-Identifier: AGPL-3.0-or-later
// Plants and their phases (SD-128). A plant recipe holds phases; a plant
// started from it copies them, so a later change of the recipe does not move
// it. The leading running plant's current phase sets the water plan: its
// recipe, and its own EC target and pH range where it has them. Without a
// running plant the recipe chosen by hand applies (SD-105). A phase moves on
// at the local midnight after its days, or only once the user confirms; the
// harvest and the end are the user's (RAT-002, RAT-077). Logic reads a
// phase's parameters, never its name (RAT-076).
#pragma once

#include <string>
#include <vector>

#include "doseph/common.hpp"
#include "doseph/config.hpp"

namespace doseph {

// The local midnight of the day an instant falls on, and the midnight `days` local days after one
Epoch localMidnight(Epoch t, const std::string& zone);
Epoch localDaysLater(Epoch midnight, int days, const std::string& zone);

// The plant that leads the water plan: the chosen one while it runs, else the earliest started
// running plant; nullptr without one
const PlantCfg* leadingPlant(const Config& cfg);

// The targets a phase asks for: its own, else its recipe's (RAT-066); NaN where neither has one
double phaseEcTarget(const Config& cfg, const PhaseCfg& ph);
double phasePhTarget(const Config& cfg, const PhaseCfg& ph);

// The water plan in force
struct WaterPlan {
  const RecipeCfg* recipe = nullptr;
  double ecTarget = kNaN, phTarget = kNaN;
  std::string source;               // "plant", "hand" or "" (none)
  const PlantCfg* plant = nullptr;  // the leading plant, for "plant"
  Epoch until = 0;                  // a hand choice's end; 0: none
  bool ended = false;               // a hand choice past its end
};
WaterPlan waterPlan(const Config& cfg, Epoch now);

// When a plant's current phase ends: the local midnight after its days
Epoch phaseEnd(const PlantCfg& p, const std::string& zone);

// Moves each running plant whose current phase moves on by itself to the next one, as often as
// its ends have passed (also after the device was off); returns the ids of the plants that moved
std::vector<std::string> advancePlants(Config& cfg, Epoch now, const std::string& zone);

// What the plants ask of the user, as notes for the morning report and the app (SD-123): a phase
// that waits for its confirmation, a last phase over (record the harvest), a phase that began
// today, a plant whose targets differ from the leading plant's
struct PlantNote {
  std::string id;  // "plant.<id>.<what>"
  Msg label, text;
};
std::vector<PlantNote> plantNotes(const Config& cfg, Epoch now, const std::string& zone);

// What lies ahead for the running plants until `until`, by local day (SD-128): a phase that
// begins by itself ("phase"), one that waits for confirmation ("confirm"), the end of the last
// one ("harvest"); one already past its plan stands at today, marked overdue
json plantDates(const Config& cfg, Epoch now, Epoch until, const std::string& zone);

// The water plan for the app: where it comes from, the recipe, the targets, and the leading
// plant's phase with its end
json waterPlanJson(const Config& cfg, Epoch now, const std::string& zone);

// What the plant was given, frozen when it is completed (RAT-064, SD-138): its days and those of
// each phase it reached, the harvest, per canister the ml that went into the tank while it ran and
// the runs whose amount is not known, the litres watering gave, and the other plants on the tank
// in that time. The amounts are the tank's: plants on one tank share its water. Amounts that were
// not counted, or whose count was lost, are null (R5).
json plantRecord(const Config& cfg, const PlantCfg& p, const RuntimeState::Count& now, const std::string& zone);

// Where the tank's counts stand, kept with a plant at its start
json countedNow(const RuntimeState::Count& c, Epoch at);

// The plant recipes and plants with where each stands now, for the app
json plantsJson(const Config& cfg, Epoch now, const std::string& zone);

}  // namespace doseph
