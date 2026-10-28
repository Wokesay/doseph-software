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
    p.suctionLpmLow = 10.0;
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
    case doseph::Signal::Flow:
      // Without a flow meter the device estimates the flow from the low end of the
      // suction pump's delivery while the outlet reports water (SD-109)
      if (profile_.flow == "outlet_level")
        return doseph::isNum(profile_.suctionLpmLow) ? std::optional<doseph::Sample>(doseph::Sample(w_.outletWet() ? profile_.suctionLpmLow : 0.0, ts))
                                                     : std::nullopt;
      return doseph::Sample(w_.rawFlow(), ts);
  }
  return std::nullopt;
}

doseph::WaterPath SimHardware::waterPath() const {
  doseph::WaterPath p;
  if (profile_.flow == "outlet_level") p.outletWet = w_.outletWet();
  if (profile_.returnBy == "pump") p.returnPump = w_.returnPumpOn;
  return p;
}

bool SimHardware::setSuctionPump(bool on, std::string& err) {
  if (on && !w_.released()) {
    err = "hardware release off";
    return false;
  }
  w_.output = on;
  return true;
}

}  // namespace sim
