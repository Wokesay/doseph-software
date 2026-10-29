// SPDX-License-Identifier: AGPL-3.0-or-later
// Twins of network devices (SD-132). The simulation answers the robot's requests to their
// addresses itself, so the simulator and the tests need no network:
//   shelly-1.local     a Shelly Gen2 relay with one output and one input; out of the box it
//                      restores its last state after a power loss and its input switches it
//   shelly-plug.local  a Shelly Gen2 plug: one output, no input; a circulation pump in the tank is
//                      plugged into it
//   shelly-old.local   a Shelly of the first generation, which has no RPC interface
//   shelly-valve.local a Shelly Gen2 relay at the tank's inlet: its output opens the inlet valve,
//                      and its input is the float switch in the tank, which says full at its
//                      line; set up so that the float does not switch the valve
//   shelly-pump.local  a Shelly Gen2 relay with two outputs, each a watering pump that takes water
//                      from the tank
//   shelly-dimmer.local a Shelly dimmer with one light on the lamps' 0–10 V line (SD-135)
//   shelly-climate.local  a Shelly Gen2 relay with four outputs in a grow room: its exhaust fan,
//                      circulation fan, heater and dehumidifier (SD-136)
//   shelly-climate2.local a Shelly Gen2 relay with two outputs: the room's humidifier and CO2 valve
//   esp-climate.local  an ESPHome device in the room: sensor/air_temp, sensor/air_rh, sensor/co2
//   esp-1.local        an ESPHome device: binary_sensor/tank_full, binary_sensor/tank_empty,
//                      sensor/water_temp, sensor/tank_level (the tank's litres)
// A Shelly's settings act as the real one's: the auto-off switches it off, and a power cycle
// sets its outputs by their initial state. A test may give one more addresses, as by its IP.
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "doseph/devices.hpp"

namespace sim {

struct FakeShelly {
  std::string address, id, model;
  std::vector<std::string> aliases;  // other addresses it answers at, as by its IP and its name
  int gen = 2;
  bool online = true;
  bool reachable = true;  // the network to it; its power stays
  struct Switch {
    bool on = false;
    doseph::Ms onSince = 0;
    doseph::json config;  // as Switch.GetConfig gives it
  };
  std::vector<Switch> switches;
  // A dimmer on a 0–10 V line (SD-135): on and its brightness, as Light.GetStatus gives them
  struct Light {
    bool on = false;
    double brightness = 100;
    doseph::json config;  // as Light.GetConfig gives it
    int sets = 0;         // Light.Set calls taken
  };
  std::vector<Light> lights;
  std::vector<bool> inputs;
  bool ignoresConfig = false;  // takes Switch.SetConfig and keeps its settings, as a faulty one might
  bool inlet = false;       // its output opens the tank's inlet, its input 0 is the float switch (SD-132)
  bool circulates = false;  // a circulation pump in the tank is plugged into its output (SD-132)
  bool waters = false;      // each output is a watering pump that takes water from the tank (SD-134)
  bool floatStuck = false;  // the float switch sticks: it never says full
  // An input changes; out of the box ("follow") it switches its output with it, detached it does not
  void setInput(int n, bool state);
  // The RPC's answer body: {"id", "src", "result"} or {"id", "src", "error"}
  doseph::json handle(const doseph::json& request, doseph::Ms now);
  void advance(doseph::Ms now);  // the auto-off acts
  void powerCycle();             // the outputs as their initial state says
};

struct FakeEsphome {
  std::string address, name;
  bool online = true;
  std::map<std::string, doseph::json> entities;  // "binary_sensor/tank_full" → true
};

class NetTwin {
 public:
  NetTwin();
  // The answer to a request: HTTP status and body; status 0 when nothing answers at its address
  std::pair<int, doseph::json> answer(const doseph::NetRequest& r, doseph::Ms now);
  void advance(doseph::Ms now);
  FakeShelly* shelly(const std::string& address);
  FakeEsphome* esphome(const std::string& address);
  FakeShelly* inlet();  // the relay at the tank's inlet, where there is one
  bool circulating() const;  // the circulation pump runs
  int watering() const;      // how many watering pumps run
  FakeEsphome* sensors();    // the ESPHome device, whose level and floats follow the tank
  // The grow room (SD-136): its air goes on by `dt` with what runs in it, and its sensors say so
  struct Room {
    double tempC = 22, rhPct = 60, co2Ppm = 600;
  };
  void climate(doseph::Ms dt);
  Room& room() { return room_; }
  doseph::json toJson() const;
  // The simulator panel: a device away or back, an input, an ESPHome value
  bool control(const std::string& action, const doseph::json& body);

 private:
  std::vector<FakeShelly> shellies_;
  std::vector<FakeEsphome> esps_;
  Room room_;
};

}  // namespace sim
