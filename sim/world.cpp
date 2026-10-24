// SPDX-License-Identifier: AGPL-3.0-or-later
#include "world.hpp"

#include <algorithm>
#include <cmath>

namespace sim {

namespace {
constexpr Ms kSampleMs = 2000;         // the measuring part delivers every 2 s (assumption)
constexpr Ms kPhDeadMs = 60000;        // pH dead time (RAT-052: about 80–99 s measured, shortened here)
constexpr Ms kHwLimitMs = 90000;       // the pump driver's own time limit (assumption, RAT-007)
constexpr double kTwinEcCoeff = 0.019; // the twin's EC temperature term, deliberately not the robot's 0.02 (assumption)
constexpr double kPlume = 0.2;         // share of a dose not mixed in yet that reaches the intake (assumption)
constexpr double kPi = 3.14159265358979323846;
}  // namespace

World::World() : rng_(42) { reset(); }

void World::reset(int n) {
  now_ = 0;
  lastSample_ = -100000;
  tank = Tank{};
  cell = Cell{};
  output = external = false;
  pumpLpm = 12.0;
  flowFault.clear();
  probeFault.clear();
  phBuffer.reset();
  ecBuffer.reset();
  phTrail_.clear();
  running_ = 0;
  frozenSet_ = false;
  rng_.seed(42);
  // Effects from measurements on the reference installation (docs/SIMULATOR.md); pH effects as assumptions
  liquids = {
      {"ph_down", {"pH−", 1.04, -4.0}},
      {"grow_a", {"Part A", 0.275, -0.12}},
      {"grow_b", {"Part B", 0.275, -0.10}},
      {"calmag", {"CalMag", 0.217, -0.02}},
  };
  pumps.assign(static_cast<size_t>(std::max(1, n)), Pump{});
  const char* order[] = {"ph_down", "grow_a", "grow_b", "calmag"};
  // Flow rates of the pump heads scatter as measured (RAT-054)
  std::uniform_real_distribution<double> fl(42.0, 53.0);
  for (size_t i = 0; i < pumps.size(); ++i) {
    pumps[i].liquid = i < 4 ? order[i] : "";
    pumps[i].trueFlow = fl(rng_);
  }
}

// Sensor noise as measured on the reference installation (RAT-082)
double World::noise(double sigma) const {
  std::normal_distribution<double> d(0.0, sigma);
  return d(rng_);
}

void World::fill(double volumeL, double ec, double ph) {
  tank.volumeL = volumeL;
  tank.ec = ec;
  tank.ph = ph;
  tank.pendingEc = tank.pendingPh = 0;
  phTrail_.clear();
}

bool World::startRun(int pump, Ms ms, const std::string& jobId, std::string& err) {
  if (pump < 1 || pump > static_cast<int>(pumps.size())) {
    err = "no such pump";
    return false;
  }
  Pump& p = pumps[static_cast<size_t>(pump - 1)];
  if (p.jobId == jobId && p.state != 0) return true;  // a repeat: not twice
  if (running_ != 0) {
    err = "busy: a pump is already running";
    return false;
  }
  if (ms <= 0 || ms > kHwLimitMs) {
    err = "over the driver's time limit";
    return false;
  }
  p.jobId = jobId;
  p.requested = ms;
  p.elapsed = 0;
  p.state = 1;
  p.error.clear();
  if (jobId.rfind("cal-", 0) == 0) p.cupMl = 0;
  running_ = pump;
  return true;
}

doseph::RunStatus World::runStatus(int pump) const {
  doseph::RunStatus st;
  if (pump < 1 || pump > static_cast<int>(pumps.size())) return st;
  const Pump& p = pumps[static_cast<size_t>(pump - 1)];
  st.jobId = p.jobId;
  st.requestedMs = p.requested;
  st.actualMs = p.elapsed;
  st.error = p.error;
  st.state = p.state == 1 ? doseph::RunStatus::State::Running
             : p.state == 2 ? doseph::RunStatus::State::Done
             : p.state == 3 ? doseph::RunStatus::State::Failed
                            : doseph::RunStatus::State::Idle;
  return st;
}

void World::stopAllPumps() {
  for (auto& p : pumps)
    if (p.state == 1) {
      p.state = 3;
      p.error = "stopped";
    }
  running_ = 0;
}

void World::advance(Ms now) {
  while (now_ < now) {
    const Ms dt = std::min<Ms>(500, now - now_);
    now_ += dt;
    step(dt);
  }
}

void World::step(Ms dt) {
  const double dts = static_cast<double>(dt) / 1000.0;
  // Pumps: a dose goes into the return line and with it into the tank, not mixed in yet
  for (size_t i = 0; i < pumps.size(); ++i) {
    Pump& p = pumps[i];
    if (p.state != 1) continue;
    p.elapsed += dt;
    if (p.blocked && p.elapsed >= 300) {
      p.state = 3;
      p.error = "blockage detected (motor current)";
      running_ = 0;
      continue;
    }
    const Ms eff = std::min(dt, p.requested - (p.elapsed - dt));
    const double ml = p.trueFlow * static_cast<double>(std::max<Ms>(0, eff)) / 60000.0;
    const bool intoCup = p.jobId.rfind("cal-", 0) == 0 || p.jobId.rfind("prime-", 0) == 0;
    if (intoCup) {
      p.cupMl += p.empty ? 0 : ml;
    } else if (!p.empty && tank.volumeL > 0.1 && ml > 0 && liquids.count(p.liquid)) {
      const Liquid& l = liquids[p.liquid];
      const double mlPerL = ml / tank.volumeL;
      const double buffer = 1.6 / (0.6 + std::max(0.0, tank.ec + tank.pendingEc));  // a stronger solution buffers more (assumption)
      tank.pendingEc += l.ecPerMlL * mlPerL;
      tank.pendingPh += l.phPerMlL * mlPerL * buffer;
      tank.volumeL += ml / 1000.0;
    }
    if (p.elapsed >= p.requested) {
      p.elapsed = p.requested;
      p.state = 2;
      running_ = 0;
    }
  }
  // Water temperature: a daily curve (assumption)
  tank.temp = 20.5 + 1.5 * std::sin(2 * kPi * static_cast<double>(now_ % 86400000) / 86400000.0);
  // Mixing: with circulation one turnover is about t63 (assumption; RAT-052 measured t63 26 s at 20.4 L);
  // without it slow
  const double tau = flowing() ? std::max(20.0, tank.volumeL / pumpLpm * 60.0) : 1800.0 * std::max(1.0, tank.volumeL / 20.0);
  const double k = 1.0 - std::exp(-dts / tau);
  tank.ec += tank.pendingEc * k;
  tank.pendingEc *= (1.0 - k);
  tank.ph += tank.pendingPh * k;
  tank.pendingPh *= (1.0 - k);
  // Drift (assumption, not measured): pH rises, EC falls as plants take up, water evaporates
  const double hours = dts / 3600.0;
  if (tank.volumeL > 1) {
    tank.ph += 0.006 * hours * (7.6 - tank.ph) / 1.5;
    tank.ec = std::max(0.0, tank.ec - 0.003 * hours * tank.ec);
    tank.volumeL = std::max(0.0, tank.volumeL - 0.002 * hours * tank.volumeL);
  }
  tank.ph = std::clamp(tank.ph, 2.5, 9.5);
  phTrail_.emplace_back(now_, tank.ph);
  while (phTrail_.size() > 2 && phTrail_[1].first <= now_ - kPhDeadMs) phTrail_.pop_front();
  // The cell: with flow it holds the intake's water within seconds; without, it stands and takes the room's temperature
  if (flowing()) {
    const double kc = 1.0 - std::exp(-dts / 5.0);
    const double delayedPh = phTrail_.empty() ? tank.ph : phTrail_.front().second;
    cell.ec += (tank.ec + kPlume * tank.pendingEc - cell.ec) * kc;
    cell.ph += (delayedPh + kPlume * tank.pendingPh - cell.ph) * kc;
    cell.temp += (tank.temp - cell.temp) * kc;
  } else {
    cell.temp += (roomTemp - cell.temp) * (1.0 - std::exp(-dts / 1200.0));
  }
  if (probeFault != "offline" && now_ - lastSample_ >= kSampleMs) lastSample_ = now_;
  if (probeFault == "frozen" && !frozenSet_) {
    frozenPh_ = rawPh();
    frozenEc_ = rawEc();
    frozenSet_ = true;
  }
  if (probeFault != "frozen") frozenSet_ = false;
}

double World::rawPh() const {
  if (probeFault == "frozen" && frozenSet_) return frozenPh_;
  double truth = phBuffer ? *phBuffer : cell.ph;
  if (!phBuffer && tank.volumeL < 0.5) truth = 7.3 + noise(0.2);  // a dry probe
  double raw = 7.0 + (truth - 7.0) / phSlope + phOffset + noise(0.006);
  if (probeFault == "jump") raw += 2.1;
  return std::round(raw * 1000.0) / 1000.0;
}

double World::rawEc() const {
  if (probeFault == "frozen" && frozenSet_) return frozenEc_;
  if (probeFault == "ec_zero") return std::round(std::max(0.0, 0.01 + noise(0.004)) * 1000.0) / 1000.0;
  // The solution's EC at 25 °C, read at the cell's temperature (not compensated)
  const double ec25 = ecBuffer ? *ecBuffer : (tank.volumeL < 0.5 ? 0.0 : cell.ec);
  const double raw = ec25 * (1.0 + kTwinEcCoeff * (cell.temp - 25.0)) * ecFactor + noise(0.004);
  return std::max(0.0, std::round(raw * 1000.0) / 1000.0);
}

double World::rawTemp() const { return std::round((cell.temp + noise(0.02)) * 100.0) / 100.0; }

double World::rawFlow() const {
  if (flowFault == "sensor" || !flowing()) return 0.0;
  return std::round(pumpLpm * (1.0 + noise(0.01)) * 10.0) / 10.0;
}

json World::toJson() const {
  json ps = json::array();
  for (size_t i = 0; i < pumps.size(); ++i) {
    const Pump& p = pumps[i];
    ps.push_back({{"pump", i + 1}, {"liquid", p.liquid}, {"trueFlow", p.trueFlow}, {"blocked", p.blocked}, {"empty", p.empty},
                  {"state", p.state}, {"cupMl", std::round(p.cupMl * 100.0) / 100.0}});
  }
  json liq = json::object();
  for (const auto& [k, v] : liquids) liq[k] = v.name;
  return {{"tank", {{"volumeL", tank.volumeL}, {"ec", tank.ec}, {"ph", tank.ph}, {"temp", tank.temp},
                    {"pendingEc", tank.pendingEc}, {"pendingPh", tank.pendingPh}}},
          {"cell", {{"ec", cell.ec}, {"ph", cell.ph}, {"temp", cell.temp}}},
          {"pumps", ps},
          {"output", output},
          {"external", external},
          {"pumpLpm", pumpLpm},
          {"flowing", flowing()},
          {"flowFault", flowFault},
          {"probeFault", probeFault},
          {"phBuffer", phBuffer ? json(*phBuffer) : json(nullptr)},
          {"ecBuffer", ecBuffer ? json(*ecBuffer) : json(nullptr)},
          {"liquids", liq}};
}

}  // namespace sim
