// SPDX-License-Identifier: AGPL-3.0-or-later
#include "simhw.hpp"

namespace sim {

std::optional<doseph::Profile> exampleProfile(const std::string& name) {
  doseph::Profile p;
  p.example = true;
  if (name == "home") {
    // Small tanks: four channels, flow estimated from the outlet level, return by gravity (assumptions)
    p.line = "home";
    p.variant = "Home (example)";
    p.channels.assign(4, {"brushed", "S", true});
    p.flow = "outlet_level";
    p.returnBy = "gravity";
    p.network = {"wifi"};
  } else if (name == "pro") {
    // Larger tanks: six channels, a flow meter, a return pump, LAN as well (assumptions)
    p.line = "pro";
    p.variant = "Pro (example)";
    p.channels.assign(6, {"brushless", "M", true});
    p.flow = "turbine";
    p.returnBy = "pump";
    p.network = {"wifi", "lan"};
  } else {
    return std::nullopt;
  }
  return p;
}

std::optional<doseph::Sample> SimHardware::sample(doseph::Signal c) const {
  if (!online() || w_.sampleTs() < 0) return std::nullopt;
  const doseph::Ms ts = w_.sampleTs();
  switch (c) {
    case doseph::Signal::Ph: return doseph::Sample(w_.rawPh(), ts);
    case doseph::Signal::Ec: return doseph::Sample(w_.rawEc(), ts);
    case doseph::Signal::Temp: return doseph::Sample(w_.rawTemp(), ts);
    case doseph::Signal::Flow: return doseph::Sample(w_.rawFlow(), ts);
  }
  return std::nullopt;
}

bool SimHardware::setSuctionPump(bool on, std::string& err) {
  (void)err;
  w_.output = on;
  return true;
}

}  // namespace sim
