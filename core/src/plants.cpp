// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/plants.hpp"

#include <algorithm>
#include <cmath>

#include "doseph/localtime.hpp"
#include "doseph/messages.hpp"

namespace doseph {

Epoch localMidnight(Epoch t, const std::string& zone) { return nextLocalMinute(t - 86400 + 1, zone, 0); }

Epoch localDaysLater(Epoch midnight, int days, const std::string& zone) {
  Epoch t = midnight;
  for (int i = 0; i < days; ++i) t = nextLocalMinute(t + 1, zone, 0);  // a change of time that day included
  return t;
}

const PlantCfg* leadingPlant(const Config& cfg) {
  if (const PlantCfg* p = cfg.lead.empty() ? nullptr : cfg.plant(cfg.lead); p && p->running()) return p;
  const PlantCfg* first = nullptr;
  for (const auto& p : cfg.plants)
    if (p.running() && (!first || p.start < first->start)) first = &p;
  return first;
}

double phaseEcTarget(const Config& cfg, const PhaseCfg& ph) {
  if (isNum(ph.ecTarget)) return ph.ecTarget;
  const RecipeCfg* r = cfg.recipe(ph.recipe);
  return r ? r->ecTarget : kNaN;
}

double phasePhTarget(const Config& cfg, const PhaseCfg& ph) {
  if (isNum(ph.phLow) && isNum(ph.phHigh)) return std::round((ph.phLow + ph.phHigh) / 2.0 * 100.0) / 100.0;
  const RecipeCfg* r = cfg.recipe(ph.recipe);
  return r ? phTarget(*r) : kNaN;
}

WaterPlan waterPlan(const Config& cfg, Epoch now) {
  WaterPlan w;
  if (const PlantCfg* p = leadingPlant(cfg)) {
    if (p->phase < 0 || p->phase >= static_cast<int>(p->phases.size())) return w;
    const PhaseCfg& ph = p->phases[static_cast<size_t>(p->phase)];
    w.recipe = cfg.recipe(ph.recipe);
    w.source = "plant";
    w.plant = p;
    w.ecTarget = phaseEcTarget(cfg, ph);
    w.phTarget = phasePhTarget(cfg, ph);
    return w;
  }
  if (cfg.active.recipe.empty()) return w;
  w.recipe = cfg.recipe(cfg.active.recipe);
  w.source = "hand";
  w.until = cfg.active.until;
  w.ended = cfg.active.until != 0 && now >= cfg.active.until;
  if (w.recipe) {
    w.ecTarget = w.recipe->ecTarget;
    w.phTarget = phTarget(*w.recipe);
  }
  return w;
}

Epoch phaseEnd(const PlantCfg& p, const std::string& zone) {
  if (p.phase < 0 || p.phase >= static_cast<int>(p.phases.size())) return 0;
  return localDaysLater(p.phaseStart, p.phases[static_cast<size_t>(p.phase)].days, zone);
}

std::vector<std::string> advancePlants(Config& cfg, Epoch now, const std::string& zone) {
  std::vector<std::string> moved;
  for (auto& p : cfg.plants) {
    bool any = false;
    while (p.running() && p.phase + 1 < static_cast<int>(p.phases.size()) && !p.phases[static_cast<size_t>(p.phase)].confirm) {
      const Epoch end = phaseEnd(p, zone);
      if (now < end) break;
      p.phase++;
      p.phaseStart = end;
      any = true;
    }
    if (any) moved.push_back(p.id);
  }
  return moved;
}

namespace {

// Local days from one midnight to another, a change of time included
int daysBetween(Epoch from, Epoch to) { return static_cast<int>(std::lround(static_cast<double>(to - from) / 86400.0)); }

}  // namespace

std::vector<PlantNote> plantNotes(const Config& cfg, Epoch now, const std::string& zone) {
  std::vector<PlantNote> out;
  const PlantCfg* lead = leadingPlant(cfg);
  const Epoch today = localMidnight(now, zone);
  for (const auto& p : cfg.plants) {
    if (!p.running() || p.phase < 0 || p.phase >= static_cast<int>(p.phases.size())) continue;
    const PhaseCfg& ph = p.phases[static_cast<size_t>(p.phase)];
    const Msg label = say("watch.plant", {{"name", p.name}});
    const bool last = p.phase + 1 == static_cast<int>(p.phases.size());
    const Epoch end = phaseEnd(p, zone);
    // Over its days: counted from the first day past the plan
    const int over = now >= end ? daysBetween(end, today) + 1 : 0;
    if (p.phase > 0 && p.phaseStart == today)
      out.push_back({"plant." + p.id + ".phase", label, say("plant.note.phase", {{"phase", ph.name}})});
    if (over > 0 && last && p.harvestAt == 0)
      out.push_back({"plant." + p.id + ".harvest", label, say("plant.note.harvest", {{"phase", ph.name}, {"days", over}})});
    else if (over > 0 && !last && ph.confirm)
      out.push_back({"plant." + p.id + ".confirm", label, say("plant.note.confirm", {{"phase", ph.name}, {"days", over}})});
    // One tank, one water: a plant that asks for other targets than the leading one is said
    if (lead && &p != lead) {
      const PhaseCfg& lp = lead->phases[static_cast<size_t>(lead->phase)];
      const double ec = phaseEcTarget(cfg, ph), lec = phaseEcTarget(cfg, lp);
      const double pht = phasePhTarget(cfg, ph), lpht = phasePhTarget(cfg, lp);
      const bool differs = ph.recipe != lp.recipe || isNum(ec) != isNum(lec) || (isNum(ec) && std::fabs(ec - lec) > 1e-9) ||
                           isNum(pht) != isNum(lpht) || (isNum(pht) && std::fabs(pht - lpht) > 1e-9);
      if (differs) out.push_back({"plant." + p.id + ".differs", label, say("plant.note.differs", {{"lead", lead->name}})});
    }
  }
  return out;
}

json plantDates(const Config& cfg, Epoch now, Epoch until, const std::string& zone) {
  json out = json::array();
  const Epoch today = localMidnight(now, zone);
  for (const auto& p : cfg.plants) {
    if (!p.running() || p.phase < 0 || p.phase >= static_cast<int>(p.phases.size())) continue;
    Epoch start = p.phaseStart;
    for (size_t i = static_cast<size_t>(p.phase); i < p.phases.size(); ++i) {
      const Epoch end = localDaysLater(start, p.phases[i].days, zone);
      const bool last = i + 1 == p.phases.size();
      if (last && p.harvestAt != 0) break;  // harvested: nothing left to plan
      const Epoch at = std::max(end, today);
      if (at > until) break;
      out.push_back({{"date", at},
                     {"kind", last ? "harvest" : p.phases[i].confirm ? "confirm" : "phase"},
                     {"plant", p.id},
                     {"name", p.name},
                     {"phase", last ? p.phases[i].name : p.phases[i + 1].name},
                     {"overdue", end < today}});
      start = at;  // a later phase begins once this one ends, today at the earliest
    }
  }
  return out;
}

json waterPlanJson(const Config& cfg, Epoch now, const std::string& zone) {
  const WaterPlan w = waterPlan(cfg, now);
  json j = {{"source", w.source},
            {"recipe", w.recipe ? json(w.recipe->id) : json(nullptr)},
            {"ecTarget", numOrNull(w.ecTarget)},
            {"phTarget", numOrNull(w.phTarget)},
            {"until", w.until},
            {"ended", w.ended},
            {"plant", nullptr}};
  if (w.plant) {
    const PhaseCfg& ph = w.plant->phases[static_cast<size_t>(w.plant->phase)];
    j["plant"] = {{"id", w.plant->id}, {"name", w.plant->name}, {"phase", ph.name}, {"phaseEnd", phaseEnd(*w.plant, zone)}, {"confirm", ph.confirm}};
  }
  return j;
}

json plantsJson(const Config& cfg, Epoch now, const std::string& zone) {
  const PlantCfg* lead = leadingPlant(cfg);
  const Epoch today = localMidnight(now, zone);
  const json all = cfg;
  json plants = json::array();
  for (const auto& p : cfg.plants) {
    json j = all["plants"][static_cast<size_t>(&p - cfg.plants.data())];
    j["running"] = p.running();
    j["leading"] = &p == lead;
    j["day"] = daysBetween(p.start, today) + 1;
    if (p.running() && p.phase >= 0 && p.phase < static_cast<int>(p.phases.size())) {
      const PhaseCfg& ph = p.phases[static_cast<size_t>(p.phase)];
      const Epoch end = phaseEnd(p, zone);
      j["phaseDay"] = daysBetween(p.phaseStart, today) + 1;
      j["phaseEnd"] = end;
      j["daysOver"] = now >= end ? daysBetween(end, today) + 1 : 0;
      j["ecTarget"] = numOrNull(phaseEcTarget(cfg, ph));
      j["phTarget"] = numOrNull(phasePhTarget(cfg, ph));
    }
    plants.push_back(std::move(j));
  }
  const WaterPlan w = waterPlan(cfg, now);
  return {{"plantRecipes", all["plantRecipes"]},
          {"plants", plants},
          {"lead", lead ? json(lead->id) : json(nullptr)},
          {"plan", {{"source", w.source}, {"recipe", w.recipe ? json(w.recipe->id) : json(nullptr)}, {"plant", w.plant ? json(w.plant->id) : json(nullptr)}}}};
}

}  // namespace doseph
