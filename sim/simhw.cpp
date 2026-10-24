// SPDX-License-Identifier: AGPL-3.0-or-later
#include "simhw.hpp"

namespace sim {

std::optional<doseph::Sample> SimHardware::sample(doseph::Channel c) const {
  if (!online() || w_.sampleTs() < 0) return std::nullopt;
  const doseph::Ms ts = w_.sampleTs();
  switch (c) {
    case doseph::Channel::Ph: return doseph::Sample(w_.rawPh(), ts);
    case doseph::Channel::Ec: return doseph::Sample(w_.rawEc(), ts);
    case doseph::Channel::Temp: return doseph::Sample(w_.rawTemp(), ts);
    case doseph::Channel::Flow: return doseph::Sample(w_.rawFlow(), ts);
  }
  return std::nullopt;
}

bool SimHardware::setCirculation(bool on, std::string& err) {
  (void)err;
  w_.output = on;
  return true;
}

}  // namespace sim
