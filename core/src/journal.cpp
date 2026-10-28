// SPDX-License-Identifier: AGPL-3.0-or-later
#include "doseph/journal.hpp"

namespace doseph {

namespace {

json runJson(const JournalRun& r) {
  return {{"order", r.order}, {"run", r.run},       {"purpose", r.purpose}, {"canister", r.canister},
          {"name", r.name},   {"channel", r.channel},     {"ml", numOrNull(r.ml)}, {"ms", r.ms},
          {"at", r.at},       {"ended", r.ended},   {"msDone", r.msDone},   {"mlDone", numOrNull(r.mlDone)}, {"cut", r.cut}};
}

}  // namespace

void DoseJournal::load() {
  runs_.clear();
  damaged_ = false;
  auto data = store_.read(file_);
  if (!data || data->empty()) return;
  const json j = json::parse(*data, nullptr, false);
  if (!j.is_array()) {
    damaged_ = true;
    return;
  }
  for (const auto& e : j) {
    if (!e.is_object()) {
      damaged_ = true;
      continue;
    }
    JournalRun r;
    r.order = jstr(e, "order");
    r.run = static_cast<int>(jnum(e, "run", 0));
    r.purpose = jstr(e, "purpose");
    r.canister = jstr(e, "canister", jstr(e, "bottle"));  // "bottle" before SD-107
    r.name = jstr(e, "name");
    r.channel = static_cast<int>(jnum(e, "channel", jnum(e, "pump", 0)));  // written before SD-107: "pump"
    r.ml = jnum(e, "ml");
    r.ms = static_cast<Ms>(jnum(e, "ms", 0));
    r.at = static_cast<Epoch>(jnum(e, "at", 0));
    r.ended = jbool(e, "ended", false);
    r.msDone = static_cast<Ms>(jnum(e, "msDone", 0));
    r.mlDone = jnum(e, "mlDone");
    r.cut = jbool(e, "cut", false);
    // A record whose purpose cannot be read is taken as a dose into the tank (the safe side)
    if (r.purpose != "ec" && r.purpose != "ph" && r.purpose != "calibration" && r.purpose != "prime" && r.purpose != "fill") {
      r.purpose = "ec";
      damaged_ = true;
    }
    runs_.push_back(r);
  }
}

bool DoseJournal::save() {
  json j = json::array();
  for (const auto& r : runs_) j.push_back(runJson(r));
  return store_.write(file_, j.dump());
}

bool DoseJournal::begin(const JournalRun& r) {
  runs_.push_back(r);
  runs_.back().ended = false;
  if (save()) return true;
  runs_.pop_back();
  return false;
}

void DoseJournal::end(const std::string& order, int run, Ms msDone, double mlDone) {
  for (auto it = runs_.rbegin(); it != runs_.rend(); ++it)
    if (it->order == order && it->run == run && !it->ended) {
      it->ended = true;
      it->msDone = msDone;
      it->mlDone = mlDone;
      save();  // a failed write leaves it begun: after a restart it is reported as possibly in, the safe side
      return;
    }
}

std::vector<JournalRun> DoseJournal::closeOpen() {
  std::vector<JournalRun> open;
  for (auto& r : runs_)
    if (!r.ended) {
      open.push_back(r);
      r.ended = true;
      r.cut = true;
      r.msDone = 0;
      r.mlDone = kNaN;
    }
  if (!open.empty()) save();
  return open;
}

bool DoseJournal::anyIntoTank() const {
  if (damaged_) return true;
  for (const auto& r : runs_)
    if (r.intoTank()) return true;
  return false;
}

void DoseJournal::clear() {
  if (runs_.empty() && !damaged_) return;
  runs_.clear();
  damaged_ = false;
  save();
}

}  // namespace doseph
