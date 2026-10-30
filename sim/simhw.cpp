// SPDX-License-Identifier: AGPL-3.0-or-later
#include "simhw.hpp"

namespace sim {

std::optional<doseph::Profile> exampleProfile(const std::string& name) {
  doseph::Profile p;
  p.example = true;
  if (name == "home") {
    // Small tanks: four channels, the feed pump in the tank, flow estimated from the outlet level,
    // return by gravity (assumptions)
    p.line = "home";
    p.variant = "Home (example)";
    p.channels.assign(4, {"brushed", "S", true, true, 40});
    p.channels[1].inverted = p.channels[3].inverted = true;  // two of four run the other way (which ones: an assumption)
    for (auto& c : p.channels) c.lineMl = 6.0;
    p.flow = "outlet_level";
    p.pumpIn = "tank";
    p.returnBy = "gravity";
    p.network = {"wifi"};
    p.pumpLpmLow = 10.0;
  } else if (name == "pro") {
    // Larger tanks: six channels, the feed pump in the tank, a flow meter, a return pump, LAN as
    // well (assumptions)
    p.line = "pro";
    p.variant = "Pro (example)";
    // Every pump runs both ways: its slot's direction, and back after a dose (SD-142)
    p.channels.assign(6, {"brushless", "M", true, true, 20});
    p.channels[1].inverted = p.channels[3].inverted = true;  // an assumption, as for Home
    for (auto& c : p.channels) c.lineMl = 6.0;
    p.flow = "turbine";
    p.pumpIn = "tank";
    p.returnBy = "pump";
    p.network = {"wifi", "lan"};
  } else {
    return std::nullopt;
  }
  // The nominal rate of each head size, ml/min (assumptions, SD-143)
  p.headMlPerMin = {{"S", 50.0}, {"M", 125.0}, {"L", 300.0}};
  // The feed pump moves water above 0.5 A (an assumption, SD-146; the twin draws 0.8 A with water,
  // 0.32 A in air)
  p.pumpMinA = 0.5;
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
      // feed pump's delivery while the outlet reports water (SD-109)
      if (profile_.flow == "outlet_level")
        return doseph::isNum(profile_.pumpLpmLow) ? std::optional<doseph::Sample>(doseph::Sample(w_.outletWet() ? profile_.pumpLpmLow : 0.0, ts))
                                                     : std::nullopt;
      return doseph::Sample(w_.rawFlow(), ts);
  }
  return std::nullopt;
}

std::optional<doseph::Drops> SimHardware::drops(int channel) const {
  if (channel < 1 || channel > channelCount() || !profile_.channels[static_cast<size_t>(channel - 1)].dropSensor) return std::nullopt;
  const Pump& p = w_.pumps[static_cast<size_t>(channel - 1)];
  return doseph::Drops{p.drops, p.lastDropMs};
}

doseph::WaterPath SimHardware::waterPath() const {
  doseph::WaterPath p;
  if (profile_.flow == "outlet_level") p.outletWet = w_.outletWet();
  if (profile_.returnBy == "pump") p.returnPump = w_.returnPumpOn;
  return p;
}

bool SimHardware::setFeedPump(bool on, std::string& err) {
  if (on && !w_.released()) {
    err = "hardware release off";
    return false;
  }
  if (on && w_.noFlowCut) {
    err = "switched off for no flow";
    return false;
  }
  if (!on) w_.noFlowCut = false;  // switching it off re-arms the hardware (PD-477)
  w_.output = on;
  return true;
}

}  // namespace sim
