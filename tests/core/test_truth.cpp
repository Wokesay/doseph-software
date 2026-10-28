// SPDX-License-Identifier: AGPL-3.0-or-later
#include <doctest/doctest.h>

#include "doseph/truth.hpp"
#include "fakes.hpp"

using namespace doseph;

namespace {

// pH: buffers 7 and 4 at raw 7.18 and 4.09; EC: factor 1 (calibrated at 25 °C)
Config calibrated() {
  Config c;
  c.phCal = {{"points", {{7.18, 7.0}, {4.087, 4.0}}}, {"at", 1}};
  c.ecCal = {{"factor", 1.0}, {"at", 1}};
  return c;
}

struct Bench {
  fake::Hardware hw;
  Config cfg = calibrated();
  RuntimeState rt;
  SensorTruth truth;
  Ms now = 10 * kSecond;
  Epoch epoch = 1798761600;
  void feed(double ph, double ec, double temp = 25.0, double flow = 10.0) {
    hw.set(Signal::Ph, ph, now);
    hw.set(Signal::Ec, ec, now);
    hw.set(Signal::Temp, temp, now);
    hw.set(Signal::Flow, flow, now);
  }
  // `steps` readings 2 s apart, each raw value moving a little as a live probe does
  void run(int steps, double ph, double ec, double temp = 25.0) {
    for (int i = 0; i < steps; ++i) {
      now += 2 * kSecond;
      epoch += 2;
      const double wobble = (i % 2) ? 0.001 : -0.001;
      feed(ph + wobble, ec + wobble, temp);
      truth.update(cfg, hw, rt, now, epoch);
    }
  }
};

}  // namespace

TEST_CASE("Sensor truth: no reading is never 0, and a probe without calibration gives no value for control (RAT-006, RAT-025)") {
  Bench b;
  b.truth.update(b.cfg, b.hw, b.rt, b.now, b.epoch);
  const Reading& ph = b.truth.get(Signal::Ph);
  CHECK(ph.quality == Quality::NoData);
  CHECK_FALSE(ph.value.has_value());
  b.hw.isOnline = false;
  b.feed(7.18, 1.0);
  b.truth.update(b.cfg, b.hw, b.rt, b.now, b.epoch);
  CHECK(b.truth.get(Signal::Ec).quality == Quality::Offline);
  CHECK_FALSE(b.truth.get(Signal::Ec).usable());
  b.hw.isOnline = true;
  b.cfg.phCal = nullptr;
  b.run(3, 7.18, 1.0);
  CHECK(b.truth.get(Signal::Ph).quality == Quality::Uncalibrated);
  CHECK(b.truth.get(Signal::Ph).value.has_value());  // the raw value is shown
  CHECK_FALSE(b.truth.get(Signal::Ph).usable());
  CHECK(b.truth.get(Signal::Ec).usable());
}

TEST_CASE("Sensor truth: pH from two buffers; a slope or zero point out of band is refused (RAT-025, RAT-026)") {
  Bench b;
  b.run(3, 6.18, 1.0);
  REQUIRE(b.truth.get(Signal::Ph).usable());
  CHECK(*b.truth.get(Signal::Ph).value == doctest::Approx(6.03).epsilon(0.01));
  CHECK_FALSE(SensorTruth::checkPh(b.cfg.phCal));
  // Swapped buffers give a negative slope
  CHECK(SensorTruth::checkPh(json{{"points", {{7.18, 4.0}, {4.087, 7.0}}}})->key == "calib.ph_slope");
  // A probe whose pH 7 reads at raw 7.8 is 47 mV off
  auto zero = SensorTruth::checkPh(json{{"points", {{7.8, 7.0}, {4.8, 4.0}}}});
  REQUIRE(zero);
  CHECK(zero->key == "calib.ph_zero");
  CHECK(SensorTruth::checkEc(json{{"factor", 3.0}})->key == "calib.ec_factor");
}

TEST_CASE("Sensor truth: EC is compensated to 25 °C at 2 % per °C; without a water temperature it is no value for control (RAT-089)") {
  Bench b;
  b.run(3, 7.18, 1.1, 30.0);  // 1.1 mS/cm at 30 °C is 1.0 at 25 °C
  REQUIRE(b.truth.get(Signal::Ec).usable());
  CHECK(*b.truth.get(Signal::Ec).value == doctest::Approx(1.0).epsilon(0.002));
  // The temperature implausible: EC shown uncompensated, not usable, never taken as at 25 °C (R5)
  b.run(3, 7.18, 1.1, 60.0);
  CHECK(b.truth.get(Signal::Temp).quality == Quality::Implausible);
  const Reading& ec = b.truth.get(Signal::Ec);
  CHECK(ec.quality == Quality::Uncalibrated);
  CHECK(ec.reason.key == "truth.ec_no_temp");
  CHECK(*ec.value == doctest::Approx(1.1).epsilon(0.002));
}

TEST_CASE("Sensor truth: stale and implausible; standstill of the raw value apart from freshness (RAT-020, RAT-023, RAT-027, RAT-059)") {
  Bench b;
  b.run(3, 7.18, 1.0);
  b.now += 61 * kSecond;  // nothing new for more than 60 s
  b.truth.update(b.cfg, b.hw, b.rt, b.now, b.epoch);
  CHECK(b.truth.get(Signal::Ph).quality == Quality::Stale);
  b.run(3, 9.9, 1.0);  // pH ≈ 9.7 is outside 3–9
  CHECK(b.truth.get(Signal::Ph).quality == Quality::Implausible);
  // A raw pH that does not move for 15 min is frozen; the temperature may stand still
  Bench f;
  for (int i = 0; i < 460; ++i) {
    f.now += 2 * kSecond;
    f.epoch += 2;
    f.feed(6.5, 1.0 + ((i % 2) ? 0.001 : -0.001), 21.0);
    f.truth.update(f.cfg, f.hw, f.rt, f.now, f.epoch);
  }
  CHECK(f.truth.get(Signal::Ph).quality == Quality::Frozen);
  CHECK(f.truth.get(Signal::Temp).quality == Quality::Ok);
  CHECK(f.truth.get(Signal::Ec).quality == Quality::Ok);
}

TEST_CASE("Sensor truth: a jump locks for 15 min of calm and survives a restart; an announced change explains it (RAT-039, RAT-042, RAT-044)") {
  Bench b;
  b.run(5, 7.18, 1.0);
  b.run(1, 7.18, 1.8);  // EC +0.8 within 5 min
  CHECK(b.truth.get(Signal::Ec).quality == Quality::Jump);
  REQUIRE(b.rt.jumpLocks.count("ec"));
  // A restart: a new truth, the run-time state as saved
  SensorTruth again;
  RuntimeState saved = runtimeFromJson(json(b.rt));
  again.update(b.cfg, b.hw, saved, b.now + 2000, b.epoch + 2);
  CHECK(again.get(Signal::Ec).quality == Quality::Jump);
  // 15 min of calm: free
  b.run(455, 7.18, 1.8);
  CHECK(b.truth.get(Signal::Ec).quality == Quality::Ok);
  CHECK_FALSE(b.rt.jumpLocks.count("ec"));
  // Announced: the same jump does not lock
  Bench a;
  a.run(5, 7.18, 1.0);
  a.truth.expectChange(Signal::Ec, a.now + kMinute);
  a.run(1, 7.18, 1.8);
  CHECK(a.truth.get(Signal::Ec).quality == Quality::Ok);
}

TEST_CASE("Sensor truth: a pH fall names the robot's own dose as one possible cause, and that it only lowers pH (RAT-041)") {
  Bench b;
  b.run(5, 7.18, 1.0);
  b.run(1, 5.6, 1.0);
  const Reading& ph = b.truth.get(Signal::Ph);
  CHECK(ph.quality == Quality::Jump);
  CHECK(ph.reason.key == "truth.jump_ph_fall");
}

TEST_CASE("Sensor truth: flow has no jump check; it jumps whenever the circulation pump starts or stops") {
  Bench b;
  for (int i = 0; i < 5; ++i) {
    b.now += 2 * kSecond;
    b.feed(7.18, 1.0, 25, 0.0);
    b.truth.update(b.cfg, b.hw, b.rt, b.now, b.epoch);
  }
  b.now += 2 * kSecond;
  b.feed(7.18, 1.0, 25, 12.0);
  b.truth.update(b.cfg, b.hw, b.rt, b.now, b.epoch);
  CHECK(b.truth.get(Signal::Flow).quality == Quality::Ok);
  CHECK(*b.truth.get(Signal::Flow).value == 12.0);
}
