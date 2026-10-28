// SPDX-License-Identifier: AGPL-3.0-or-later
// Maintenance dates (SD-125): what the device has to have looked after, each
// with its interval, bundled into dates. An item due within a week of the
// earliest joins that date, so one visit does them all. On its date an item
// becomes a note (info, SD-123): in the morning report and in the app until
// it is done. A calibration resets its item by itself; anything else is
// marked done by hand. The intervals are assumptions and can be set.
#pragma once

#include <string>
#include <vector>

#include "doseph/common.hpp"
#include "doseph/config.hpp"

namespace doseph {

constexpr int kServiceMinDays = 7, kServiceMaxDays = 730;
constexpr Epoch kServiceBundleS = 7 * 86400;  // items due within a week of a date join it

// The kinds of item with their intervals in days (assumptions, SD-125)
struct ServiceKind {
  const char* kind;
  int days;
};
inline constexpr ServiceKind kServiceKinds[] = {
    {"probe.ph.cal", 30}, {"probe.ec.cal", 90}, {"channel.cal", 90}, {"chamber.clean", 30}, {"heads.check", 180}, {"probe.ph.replace", 365},
};
int serviceDefaultDays(const std::string& kind);  // 0 for a kind there is none of

struct ServiceItem {
  std::string id;    // "probe.ph.cal", "channel.2.cal", …
  std::string kind;  // its interval's key
  int channel = 0;   // for a channel's calibration
  int days = 0;      // the interval in force
  Epoch last = 0;    // done, calibrated, or first seen
  Epoch due = 0;     // last + interval
  Epoch date = 0;    // the maintenance date it belongs to: the local midnight of that day
  bool dueNow = false;
  Msg label;
};

struct ServicePlan {
  std::vector<ServiceItem> items;  // sorted by date, then by due
  std::vector<Epoch> dates;        // the maintenance dates, earliest first
};

// The items this configuration has: the probes, each channel with a canister, the chamber and
// its suction filter, and the pump heads while any channel is in use
std::vector<ServiceItem> serviceItems(const Config& cfg);
// Notes when each item was first seen, so a new device or channel is not due at once; returns
// whether anything changed
bool noteService(const Config& cfg, RuntimeState& rt, Epoch now);
ServicePlan planService(const Config& cfg, const RuntimeState& rt, Epoch now, const std::string& zone);
json serviceJson(const ServicePlan& p);

}  // namespace doseph
