// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/hardware.hpp"

#include <algorithm>

namespace doseph {

const char* signalName(Signal c) {
  switch (c) {
    case Signal::Ph: return "ph";
    case Signal::Ec: return "ec";
    case Signal::Temp: return "temp";
    case Signal::Flow: return "flow";
  }
  return "unknown";
}

json profileJson(const Profile& p) {
  json channels = json::array();
  for (const ChannelCaps& c : p.channels)
    channels.push_back({{"pump", c.pump}, {"head", c.head.empty() ? json(nullptr) : json(c.head)}, {"dropSensor", c.dropSensor}, {"reverse", c.reverse},
                        {"dailyMaxMin", numOrNull(c.dailyMaxMin)}});
  return {{"line", p.line}, {"variant", p.variant}, {"channels", channels}, {"flow", p.flow},
          {"pumpIn", p.pumpIn}, {"return", p.returnBy}, {"network", p.network}, {"pumpLpmLow", numOrNull(p.pumpLpmLow)}, {"example", p.example}};
}

std::optional<std::string> MemoryStorage::read(const std::string& name) {
  for (const auto& [n, d] : files_)
    if (n == name) return d;
  return std::nullopt;
}

bool MemoryStorage::write(const std::string& name, const std::string& data) {
  for (auto& [n, d] : files_)
    if (n == name) {
      d = data;
      return true;
    }
  files_.emplace_back(name, data);
  return true;
}

bool MemoryStorage::remove(const std::string& name) {
  files_.erase(std::remove_if(files_.begin(), files_.end(), [&](const auto& f) { return f.first == name; }), files_.end());
  return true;
}

bool MemoryStorage::clear(const std::string& keep) {
  files_.erase(std::remove_if(files_.begin(), files_.end(), [&](const auto& f) { return f.first != keep; }), files_.end());
  return true;
}

bool MemoryStorage::append(const std::string& name, const std::string& data) {
  for (auto& [n, d] : files_)
    if (n == name) {
      d += data;
      return true;
    }
  files_.emplace_back(name, data);
  return true;
}

}  // namespace doseph
