// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/hardware.hpp"

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
