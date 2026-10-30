// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/ratecheck.hpp"

#include <cmath>

#include "doseph/pumpcal.hpp"

namespace doseph {

namespace {

const ChannelCal* calOf(const Config& cfg, int channel) {
  const auto it = cfg.channels.find(channel);
  return it != cfg.channels.end() && isNum(it->second.flowMlPerMin) && it->second.flowMlPerMin > 0 ? &it->second : nullptr;
}

// A canister on this device's own channel: a rate on a coupled device's channel is that one's
bool ownChannel(const CanisterCfg& b) { return b.channel > 0 && b.device.empty(); }

}  // namespace

void startUse(const Config& cfg, RuntimeState& rt, const CanisterCfg& b, double read, Epoch now) {
  const ChannelCal* cal = calOf(cfg, b.channel);
  if (!ownChannel(b) || !cal || !isNum(read)) {
    rt.stockRead.erase(b.id);
    return;
  }
  rt.stockRead[b.id] = {{"ml", read}, {"at", now}, {"channel", b.channel}, {"calAt", cal->at}};
}

json checkUse(const Config& cfg, RuntimeState& rt, const CanisterCfg& b, double read, Epoch now) {
  json out = nullptr;
  const ChannelCal* cal = calOf(cfg, b.channel);
  const json from = rt.stockRead.contains(b.id) ? rt.stockRead[b.id] : json(nullptr);
  const auto counted = rt.stockMl.find(b.id);
  // Only along one calibration of the same channel, with the level counted all the way
  if (ownChannel(b) && cal && from.is_object() && jnum(from, "channel") == b.channel && jnum(from, "calAt") == static_cast<double>(cal->at) &&
      counted != rt.stockMl.end() && isNum(counted->second) && isNum(jnum(from, "ml"))) {
    const double countedMl = jnum(from, "ml") - counted->second, usedMl = jnum(from, "ml") - read;
    if (countedMl >= kUseMinMl && usedMl > 0) {
      const double share = usedMl / countedMl - 1;
      const std::string ch = std::to_string(b.channel);
      rt.rateProposal.erase(ch);  // a newer comparison replaces an older one
      out = {{"countedMl", countedMl}, {"usedMl", usedMl}, {"share", share}, {"proposal", nullptr}, {"calibrate", false}};
      if (std::abs(share) > kCalWindow) {
        out["calibrate"] = true;  // too far off for a correction: a leak, a wrong reading or a worn pump
      } else if (std::abs(share) > kUseShare) {
        const double to = std::round(cal->flowMlPerMin * usedMl / countedMl * 100) / 100;
        rt.rateProposal[ch] = {{"canister", b.id}, {"countedMl", countedMl}, {"usedMl", usedMl}, {"from", cal->flowMlPerMin},
                               {"to", to}, {"at", now}, {"calAt", cal->at}};
        out["proposal"] = rt.rateProposal[ch];
      }
    }
  }
  startUse(cfg, rt, b, read, now);
  return out;
}

bool pruneRateChecks(const Config& cfg, RuntimeState& rt) {
  bool changed = false;
  auto prune = [&](json& entries, auto stale) {
    for (auto it = entries.begin(); it != entries.end();) {
      if (stale(it.key(), it.value())) {
        it = entries.erase(it);
        changed = true;
      } else {
        ++it;
      }
    }
  };
  auto calChanged = [&](int channel, const json& e) {
    const ChannelCal* cal = calOf(cfg, channel);
    return !cal || jnum(e, "calAt") != static_cast<double>(cal->at);
  };
  auto channelOf = [](const std::string& key) { return key.size() <= 2 && key.find_first_not_of("0123456789") == std::string::npos ? std::stoi(key) : 0; };
  prune(rt.stockRead, [&](const std::string& id, const json& e) {
    const CanisterCfg* b = cfg.canister(id);
    return !b || !ownChannel(*b) || jnum(e, "channel") != b->channel || calChanged(b->channel, e);
  });
  prune(rt.rateProposal, [&](const std::string& key, const json& e) {
    const int n = channelOf(key);
    const CanisterCfg* b = cfg.canisterOnChannel(n);
    return n == 0 || !b || !ownChannel(*b) || b->id != jstr(e, "canister") || calChanged(n, e);
  });
  prune(rt.dropRate, [&](const std::string& key, const json& e) {
    const int n = channelOf(key);
    return n == 0 || calChanged(n, e);
  });
  return changed;
}

std::optional<double> watchDrops(RuntimeState& rt, int channel, Epoch calAt, double ml, std::uint64_t drops, Epoch now) {
  if (!isNum(ml) || ml <= 0) return std::nullopt;
  json& d = rt.dropRate[std::to_string(channel)];
  if (!d.is_object() || jnum(d, "calAt") != static_cast<double>(calAt)) d = {{"calAt", calAt}, {"ml", 0.0}, {"drops", 0.0}};
  const double sumMl = jnum(d, "ml", 0) + ml, sumDrops = jnum(d, "drops", 0) + static_cast<double>(drops);
  const double perMl = jnum(d, "perMl");
  const double window = isNum(perMl) ? kDriftWindowMl : kDriftLearnMl;
  if (sumMl < window) {
    d["ml"] = sumMl;
    d["drops"] = sumDrops;
    return std::nullopt;
  }
  d["ml"] = 0.0;
  d["drops"] = 0.0;
  if (!isNum(perMl)) {
    // Learned; a dose that did not drip at all says nothing of drop size
    if (sumDrops > 0) d["perMl"] = sumDrops / sumMl;
    return std::nullopt;
  }
  const double share = sumDrops / sumMl / perMl - 1;
  if (std::abs(share) <= kDriftShare || d.contains("drift")) return std::nullopt;
  d["drift"] = {{"share", share}, {"at", now}};
  return share;
}

}  // namespace doseph
