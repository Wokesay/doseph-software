// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/service.hpp"

#include <algorithm>

#include "doseph/localtime.hpp"
#include "doseph/messages.hpp"

namespace doseph {

namespace {

Epoch calibratedAt(const json& cal) {
  return cal.is_object() && cal.contains("at") && cal["at"].is_number_integer() ? cal["at"].get<Epoch>() : 0;
}

// The local midnight of the day an instant falls on, a change of time that day included
Epoch dayStart(Epoch t, const std::string& zone) { return nextLocalMinute(t - 86400 + 1, zone, 0); }

}  // namespace

int serviceDefaultDays(const std::string& kind) {
  for (const auto& k : kServiceKinds)
    if (kind == k.kind) return k.days;
  return 0;
}

std::vector<ServiceItem> serviceItems(const Config& cfg) {
  std::vector<ServiceItem> out;
  auto add = [&](std::string id, const char* kind, int channel, Msg label) {
    ServiceItem i;
    i.id = std::move(id);
    i.kind = kind;
    i.channel = channel;
    auto d = cfg.service.days.find(kind);
    i.days = d != cfg.service.days.end() ? d->second : serviceDefaultDays(kind);
    i.label = std::move(label);
    out.push_back(std::move(i));
  };
  add("probe.ph.cal", "probe.ph.cal", 0, say("service.probe.ph.cal"));
  add("probe.ec.cal", "probe.ec.cal", 0, say("service.probe.ec.cal"));
  bool anyChannel = false;
  for (const auto& b : cfg.canisters) {
    if (b.channel <= 0) continue;
    anyChannel = true;
    add("channel." + std::to_string(b.channel) + ".cal", "channel.cal", b.channel, say("service.channel.cal", {{"channel", b.channel}, {"name", b.name}}));
  }
  add("chamber.clean", "chamber.clean", 0, say("service.chamber.clean"));
  if (anyChannel) add("heads.check", "heads.check", 0, say("service.heads.check"));
  add("probe.ph.replace", "probe.ph.replace", 0, say("service.probe.ph.replace"));
  return out;
}

bool noteService(const Config& cfg, RuntimeState& rt, Epoch now) {
  bool changed = false;
  std::vector<std::string> ids;
  for (const auto& i : serviceItems(cfg)) {
    ids.push_back(i.id);
    if (!rt.serviceSince.count(i.id)) {
      rt.serviceSince[i.id] = now;
      changed = true;
    }
  }
  // An item the device no longer has (a channel without a canister) is forgotten
  for (auto it = rt.serviceSince.begin(); it != rt.serviceSince.end();) {
    if (std::find(ids.begin(), ids.end(), it->first) == ids.end()) {
      it = rt.serviceSince.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  return changed;
}

ServicePlan planService(const Config& cfg, const RuntimeState& rt, Epoch now, const std::string& zone) {
  ServicePlan plan;
  for (auto i : serviceItems(cfg)) {
    // Done by hand, or calibrated, which resets its item by itself; without either, since it was
    // first seen
    Epoch last = 0;
    if (auto d = rt.serviceDone.find(i.id); d != rt.serviceDone.end()) last = d->second;
    if (i.kind == "probe.ph.cal") last = std::max(last, calibratedAt(cfg.phCal));
    if (i.kind == "probe.ec.cal") last = std::max(last, calibratedAt(cfg.ecCal));
    if (i.kind == "channel.cal")
      if (auto c = cfg.channels.find(i.channel); c != cfg.channels.end()) last = std::max(last, c->second.at);
    if (last == 0)
      if (auto s = rt.serviceSince.find(i.id); s != rt.serviceSince.end()) last = s->second;
    if (last == 0) last = now;
    i.last = last;
    i.due = last + static_cast<Epoch>(i.days) * 86400;
    plan.items.push_back(std::move(i));
  }
  // A pump calibration comes with the pH probe's calibration (SD-144): it is due with the last one of
  // those that falls due before it, so both are done together
  const auto ph = std::find_if(plan.items.begin(), plan.items.end(), [](const ServiceItem& i) { return i.kind == "probe.ph.cal"; });
  if (ph != plan.items.end() && ph->days > 0)
    for (auto& i : plan.items) {
      if (i.kind != "channel.cal" || ph->due > i.due) continue;
      const Epoch step = static_cast<Epoch>(ph->days) * 86400;
      i.due = ph->due + (i.due - ph->due) / step * step;
    }
  std::sort(plan.items.begin(), plan.items.end(), [](const ServiceItem& a, const ServiceItem& b) { return a.due < b.due; });
  // Bundled: the earliest open item opens a date; whatever is due within a week of it joins
  Epoch open = 0;
  for (auto& i : plan.items) {
    if (open == 0 || i.due > open + kServiceBundleS) {
      open = i.due;
      plan.dates.push_back(dayStart(open, zone));
    }
    i.date = plan.dates.back();
    i.dueNow = now >= i.date;
  }
  return plan;
}

json serviceJson(const ServicePlan& p) {
  json items = json::array();
  for (const auto& i : p.items)
    items.push_back({{"id", i.id},
                     {"kind", i.kind},
                     {"channel", i.channel},
                     {"days", i.days},
                     {"defaultDays", serviceDefaultDays(i.kind)},
                     {"last", i.last},
                     {"due", i.due},
                     {"date", i.date},
                     {"dueNow", i.dueNow},
                     {"label", i.label}});
  return {{"dates", p.dates}, {"items", items}};
}

}  // namespace doseph
