// SPDX-License-Identifier: AGPL-3.0-or-later
// The dosing journal (SD-106): a record when a pump run begins, written
// before the pump starts, and one when it ends, with what ran. After a
// restart a run that began and never ended is reported: up to its amount may
// be in the tank, how much is unknown (R6, RAT-007). Runs that ended but were
// not mixed in yet make the next cycle mix first. The journal is emptied
// once the cycle has mixed in and measured everything it dosed (it holds).
#pragma once

#include <string>
#include <vector>

#include "doseph/hardware.hpp"

namespace doseph {

struct JournalRun {
  std::string order;
  int run = 0;            // partial run, from 1
  std::string purpose;    // ec | ph | calibration | prime | fill
  std::string canister, name;
  int channel = 0;
  double ml = kNaN;       // planned for this run
  Ms ms = 0;              // planned run time
  Epoch at = 0;
  bool ended = false;
  Ms msDone = 0;          // as the driver reported it
  double mlDone = kNaN;   // missing: not known (R5)
  bool cut = false;       // ended by a restart: how much ran is not known
  // Whether it goes into the tank: calibration and priming go into a cup (RAT-070); a line fill
  // drips into the tank (SD-111)
  bool intoTank() const { return purpose == "ec" || purpose == "ph" || purpose == "fill"; }
};

class DoseJournal {
 public:
  explicit DoseJournal(IStorage& store, std::string file = "journal.json") : store_(store), file_(std::move(file)) {}
  // Reads what the storage holds (at the start). A damaged file counts as a
  // run that may have gone in, of an unknown amount: never as nothing (R5).
  void load();
  // Before the pump starts; false when it could not be written: then the run does not start.
  bool begin(const JournalRun& r);
  void end(const std::string& order, int run, Ms msDone, double mlDone);
  // After a restart: the runs that began and never ended, marked as cut, so
  // a second restart does not report them again
  std::vector<JournalRun> closeOpen();
  const std::vector<JournalRun>& runs() const { return runs_; }
  bool damaged() const { return damaged_; }
  // Something went into the tank since it was emptied
  bool anyIntoTank() const;
  void clear();

 private:
  bool save();
  IStorage& store_;
  std::string file_;
  std::vector<JournalRun> runs_;
  bool damaged_ = false;
};

}  // namespace doseph
