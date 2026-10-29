// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/refill.hpp"

#include <cmath>

#include "doseph/config.hpp"
#include "doseph/gateway.hpp"
#include "doseph/messages.hpp"

namespace doseph {

namespace {
const char* kValve = "refill_valve";
double minutes(Ms ms) { return std::round(static_cast<double>(ms) / 6000.0) / 10.0; }
}  // namespace

std::optional<Msg> RefillValve::blocked(const Ctx& c, const RefillIn& in) const {
  if (filling()) return say("refill.no.filling");
  if (c.stopped) return say("refill.no.stopped");
  if (c.maintenanceUntil > 0) return say("refill.no.maintenance");
  if (!c.hold.empty()) return say("refill.no.hold");
  if (!placementConfirmed(c.cfg)) return say("refill.no.placement");
  if (holds(c.rt, "refill")) return say("refill.no.latched");
  if (in.valve == "not_assigned") return say("refill.no.valve_unassigned");
  if (!in.floatAssigned) return say("refill.no.float_unassigned");
  if (!in.full) return say("refill.no.float");
  if (*in.full) return say("refill.no.full");
  if (in.valve != "off") return say("refill.no.valve");
  if (in.busy) return say("refill.no.busy");
  return std::nullopt;
}

std::optional<Msg> RefillValve::start(Ctx& c, const RefillIn& in, NetOutputs& out) {
  if (auto why = blocked(c, in)) return why;
  open(c, out, "button");
  return std::nullopt;
}

std::optional<bool> RefillValve::stop(Ctx& c, NetOutputs& out, Msg why) {
  if (!filling()) return std::nullopt;
  return end(c, out, "", std::move(why));
}

void RefillValve::open(Ctx& c, NetOutputs& out, const std::string& why) {
  since_ = c.now;
  opened_ = false;
  why_ = why;
  c.rt.refill = {{"since", c.epoch}, {"why", why}};
  out.want(kValve, true);
  c.log.add(c.epoch, "control", "info", say("ev.refill.start"), say("refill.why." + why), {{"why", why}});
}

bool RefillValve::end(Ctx& c, NetOutputs& out, const std::string& latch, Msg why) {
  out.want(kValve, false);
  const bool opened = opened_;
  const double min = minutes(c.now - since_);
  since_ = -1;
  opened_ = false;
  c.rt.refill = nullptr;
  const json data = {{"min", min}, {"opened", opened}};
  if (latch.empty()) {
    const bool full = why.key == "refill.end.full";
    c.log.add(c.epoch, "control", full ? "info" : "warn", say(full ? "ev.refill.done" : "ev.refill.ended", {{"min", min}}), std::move(why), data);
    return opened;
  }
  // A fault ends the fill: no fill opens until a person has looked and released it (RAT-031), or,
  // where the refill only reports (SD-133), it is reported once while its report stands
  const json entry = {{"at", c.epoch}, {"min", min}, {"reason", say("refill.why_latch." + latch)}};
  const bool again = reportStands(c.rt, "refill");
  if (setLatch(c.cfg, c.rt, "refill", entry))
    c.log.add(c.epoch, "alarm", "alarm", say("ev.refill.latched"), say("latch.refill", entry), data);
  else if (!again)
    c.log.add(c.epoch, "alarm", "alarm", reportedEvent("refill", entry), say("latch.refill", entry), data);
  return opened;
}

bool RefillValve::tick(Ctx& c, const RefillIn& in, NetOutputs& out) {
  // "top up water" raised asks for one fill. One standing when the robot starts asks for none:
  // after a restart nothing starts on its own (R6).
  if (!seenTopUp_) {
    seenTopUp_ = true;
    lastTopUp_ = c.rt.topUp;
  }
  if (c.rt.topUp && !lastTopUp_) asked_ = true;
  lastTopUp_ = c.rt.topUp;
  if (!c.rt.topUp) asked_ = false;

  if (filling()) {
    const Ms ran = c.now - since_;
    // A person at the device or the tank, or a hold of every pump: closed, no latch
    if (c.stopped) return end(c, out, "", say("refill.end.stopped"));
    if (c.maintenanceUntil > 0) return end(c, out, "", say("refill.end.maintenance"));
    if (!c.hold.empty()) return end(c, out, "", say("refill.end.hold"));
    if (in.valve == "not_assigned") return end(c, out, "", say("refill.end.unassigned"));
    if (in.valve == "on") opened_ = true;
    // The valve cannot be reached while it may be open: a person looks (RAT-031)
    if (opened_ && in.valve == "offline") return end(c, out, "valve", {});
    if (!in.full) return end(c, out, opened_ ? "float" : "", say("refill.end.float"));
    if (*in.full) return end(c, out, "", say("refill.end.full"));
    if (!opened_ && (in.valve == "refused" || ran >= kRefillOpenMs)) return end(c, out, "", say("refill.end.not_open"));
    // The longest fill without "full": the float or the inlet is faulty (RAT-031, RAT-079)
    if (ran >= static_cast<Ms>(std::llround(c.cfg.refill.maxFillMin * 60.0)) * kSecond) return end(c, out, "time", {});
    return false;
  }
  if (!asked_) return false;
  const auto why = blocked(c, in);
  if (!why) {
    asked_ = false;
    open(c, out, "top_up");
    return false;
  }
  // It waits while a dose or a job runs, the valve is checked or the float has no value now; it
  // lapses where a person is at the device or the tank, the refill is latched, or the tank is full
  const std::string& k = why->key;
  if (k != "refill.no.busy" && k != "refill.no.valve" && k != "refill.no.float") asked_ = false;
  return false;
}

bool RefillValve::reportCut(Ctx& c) {
  if (!c.rt.refill.is_object()) {
    c.rt.refill = nullptr;
    return false;
  }
  c.log.add(c.epoch, "control", "warn", say("ev.refill.cut"), say("ev.refill.cut.text", {{"since", c.rt.refill.value("since", json(nullptr))}}));
  c.rt.refill = nullptr;
  return true;
}

json RefillValve::status(const Ctx& c, const RefillIn& in) const {
  const auto why = blocked(c, in);
  json j = {{"filling", filling()},
            {"asked", asked_},
            {"opened", opened_},
            {"since", filling() ? c.rt.refill.value("since", json(nullptr)) : json(nullptr)},
            {"why", filling() ? json(why_) : json(nullptr)},
            {"full", in.full ? json(*in.full) : json(nullptr)},
            {"blocked", why ? json(*why) : json(nullptr)}};
  return j;
}

}  // namespace doseph
