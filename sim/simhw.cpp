// SPDX-License-Identifier: AGPL-3.0-or-later
#include "simhw.hpp"

namespace sim {

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

bool SimHardware::setCirculation(bool on, std::string& err) {
  (void)err;
  w_.output = on;
  return true;
}

}  // namespace sim
