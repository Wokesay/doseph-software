// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/hardware.hpp"

namespace doseph {

const char* channelName(Channel c) {
  switch (c) {
    case Channel::Ph: return "ph";
    case Channel::Ec: return "ec";
    case Channel::Temp: return "temp";
    case Channel::Flow: return "flow";
  }
  return "unknown";
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
