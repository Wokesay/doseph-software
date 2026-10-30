// SPDX-License-Identifier: AGPL-3.0-or-later
// A pump calibration by the user (SD-143): the channel's intake goes into a cylinder, or a glass of
// water on a kitchen scale; the pump draws that water along its normal path, and the person reads
// how much went. Steps, one at a time:
//   reverse      the line's concentrate back into its canister (backwards, the line and a margin)
//   to_water     the person moves the intake into the water
//   prime        water until it drips steadily, then pulled back as after a dose
//   reading      the person enters the start reading, then one after each measuring run
//   run          a measuring run, as a dose: refill, run, pull back
//   empty        the water back out of the line
//   to_canister  the person moves the intake back into its canister
//   fill         the line filled until its first drop: what falls after it is the first dose
//   done | failed | cancelled
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "doseph/gateway.hpp"

namespace doseph {

// Assumptions until measured on the bench (SD-143)
constexpr double kCalRunMl = 20.0;          // each measuring run, ml: three and the priming fit a 100 ml cylinder
constexpr double kCalAgree = 0.03;          // two runs agree within this share, else a third
constexpr double kCalWindow = 0.30;         // a rate this far from the head's nominal one is refused
constexpr double kCylinderShift = 1.02;     // a cylinder reads this much more than went, for the intake in it
constexpr double kCalLineMargin = 0.5;      // backwards by the line and this share of it
constexpr Ms kCalLineUnknownMs = 30 * kSecond;  // backwards this long where the line is not known
constexpr Ms kCalPrimeRunMs = 10 * kSecond;
constexpr int kCalPrimeRuns = 12;           // at most 2 min to drip steadily
constexpr Ms kCalIdleMs = 30 * kMinute;     // no step for this long: the calibration ends

class PumpCal {
 public:
  // A calibration of `channel` with "cylinder" or "scale"; refused with the reason
  bool start(Ctx& c, Actuators& act, int channel, const std::string& method, Msg& err);
  // The person's step: the intake moved, or a reading entered ({"reading": ml or g})
  bool next(Ctx& c, Actuators& act, const json& body, Msg& err);
  // Ended by the person or the robot: a run going on stops; where the intake may be out of its
  // canister the channel is locked until a person releases it
  void cancel(Ctx& c, Actuators& act, Doser& doser, const Msg& why);
  // Each tick: starts the run a step wants once it may, ends a calibration left idle
  void tick(Ctx& c, Actuators& act, Doser& doser);
  // An order of its own ended
  void orderDone(Ctx& c, Actuators& act, const DoseProgress& p);
  bool owns(const std::string& orderId) const { return active() && !order_.empty() && orderId == order_; }
  // Under way: the cycle circulates and doses nothing
  bool active() const { return channel_ > 0 && step_ != "done" && step_ != "failed" && step_ != "cancelled"; }
  int channel() const { return channel_; }
  const std::string& step() const { return step_; }
  // A measured rate to save, once: the robot saves it with the head it measured
  std::optional<double> takeResult() {
    auto r = result_;
    result_.reset();
    return r;
  }
  const std::string& head() const { return head_; }
  json toJson() const;

  // The channel's lock after a calibration cut short with its intake perhaps out of its canister
  static std::string cutLatch(int channel) { return channelLatch(channel, "cal_cut"); }

 private:
  bool waiting() const { return step_ == "to_water" || step_ == "reading" || step_ == "to_canister"; }
  bool intakeOut() const;  // the intake may be out of its canister
  void go(Ctx& c, const std::string& step, Msg message);
  void fail(Ctx& c, Actuators& act, Msg why);
  bool launch(Ctx& c, Actuators& act, Doser& doser);
  void judge(Ctx& c, Actuators& act);

  int channel_ = 0;
  std::string method_, step_, head_, canister_, name_;
  double nominal_ = kNaN, lineMl_ = kNaN;
  bool drops_ = false, pending_ = false;
  std::string order_;
  int seq_ = 0;
  Ms lastStep_ = 0;
  std::vector<double> readings_;
  struct Run {
    Ms ms = 0;
    std::uint64_t drops = 0;
    double flow = kNaN;
  };
  std::vector<Run> runs_;
  std::uint64_t dropsFrom_ = 0;
  double flow_ = kNaN;
  std::optional<double> result_;
  Msg message_;
  Epoch at_ = 0;
};

}  // namespace doseph
