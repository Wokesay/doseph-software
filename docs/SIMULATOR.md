# Simulator

The simulator runs the real core against a digital twin of the tank, the
measuring chamber, the flow, the dosing pumps and the probes, and serves the
web app. Development, tests and demos need no hardware.

## Start

```sh
cmake -S . -B build -G Ninja && cmake --build build
(cd web && npm ci && npm run build)
build/doseph_sim_server --scenario demo --web web/dist --password demo-password
```

Open http://localhost:8080. Options (`--help`):

| Option | |
|---|---|
| `--port N`, `--host ADDR` | where it listens (8080, 127.0.0.1) |
| `--scenario demo\|new` | `demo`: set up, 100 L of fresh water, a recipe in force; `new`: nothing set up, the first password asked for |
| `--profile home\|pro` | the example device (`home`): what the twin's hardware reports, see below |
| `--password PW` | the password of the demo scenario |
| `--data DIR` | keep settings and history in this folder; else in memory only. One simulator per folder: it holds `simulator.lock` there while it runs, and a second start on the folder ends with a message |
| `--speed X` | time-lapse factor |
| `--prefill H` | hours the demo computes ahead, so the history has data |
| `--allow-reset` | change the scenario without signing in (automated tests only) |

Without a password set, the console shows the setup code the first
password needs, as the device will.

**Pushes** (SD-124): the simulator sends the channels set up in the
settings (ntfy, Telegram, e-mail) through the computer's `curl`, found on
the `PATH` (part of Linux, macOS and Windows 10 and later); the variable
`DP_CURL` names another program. Without curl nothing is sent and the
settings say why. With a time-lapse, a critical alarm's reminders come as
fast as the simulated time runs.

**MQTT and Home Assistant** (SD-130): switched on under Settings → MQTT
and Home Assistant, the simulator connects to a broker in your network
over plain TCP and sends its values. Home Assistant finds the device by
itself (discovery). The simulator has no TLS, so with TLS on it says so
and does not connect; for the same reason it takes no commands, which
need TLS (PD-461). To try it, use a Mosquitto broker without TLS, for
example Home Assistant's own add-on, with a user of its own.

### Example profiles

The device reports what it has (SD-108); `GET /api/v1/info` shows it as
`profile`. The simulator has two example profiles. Their numbers are
**assumptions**, not the data of a board, and `example` is `true`:

| | `home` | `pro` |
|---|---|---|
| Channels | 4, brushed pumps, head S | 6, brushless pumps, head M |
| Drop sensor | on every outlet | on every outlet |
| Flow | `outlet_level` (estimated) | `turbine` (a flow meter) |
| Return | `gravity` | `pump` |
| Network | Wi-Fi | Wi-Fi, LAN |
| Feed pump | in the tank | in the tank |
| Feed pump, low end | 10 L/min | – |
| Pull back | the pumps run backwards | they cannot |
| Run time per 24 h (SD-113) | 40 min per head | 20 min per head |

The twin has one pump per channel. A pump head can be changed for another
size or taken off (`channelHead`): a larger head moves more (S 1, M 2.5,
L 6 times, an **assumption**), none moves nothing, and the device reports
the head it has now. Without a flow meter (`outlet_level`)
the device reports the flow as the low end of the feed pump's delivery
while the outlet's level sensor reports water, and 0 otherwise, so a
turnover errs long (SD-109).

The download of a prototype has the web app built in: it needs no
`--web`, and started without arguments (a double-click) it takes the next
free port from 8080 and opens the browser (`docs/RELEASE.md`).

For the web app's development server with hot reload: start the
simulator on port 8080, then `cd web && npm run dev` and open
http://localhost:5173 (the API is passed through).

## The twin

| | |
|---|---|
| Tank | volume, EC, pH, temperature; a dose spreads with the flow (time constant about one turnover, at least 20 s), pH with a dead time of 60 s |
| Measuring chamber | holds what flows through it; without flow it keeps its water |
| Feed pump | in the tank it delivers 12 L/min once its hose has filled (2 s), and the hardware switches it and every dosing pump off after 9 s without flow its sensor sees, until the robot switches it off (PD-477); in the device (`pumpIn` of the options) it primes for 8 s and has no such cut; it primes or fills again after each stop; faults: inlet blocked, out of the water (dry), hose off, flow or outlet sensor broken. Its current (assumptions): 0.80 A moving water, 0.32 A running dry or against a blocked inlet (and while a pump in the device primes), 0.86 A with the hose off. A damaged cable shifts pH by 0.3 and EC by 0.12 mS/cm while it runs (assumptions) |
| Water path | the measuring chamber holds 0.15 L up to its weir, which keeps the probes wet; above it the water returns by gravity or by a return pump that runs from 0.25 L to 0.18 L, unless the robot holds it off; faults: return blocked, return pump failed (it runs and moves nothing). At 0.35 L the high-level sensor trips. A power cut forgets a return pump held off |
| Hardware release | a leak, high level or the stop button switch the feed pump and every dosing pump off below the robot, and no run starts; the state shows it as `hardware` |
| Dosing channels | four or six, each pump with its own true flow (42–53 ml/min with the head of its profile), may be blocked or empty; a head can be changed or taken off; calibration and priming go into a cup |
| Outlets | a drop sensor at each outlet counts drops of 0.05 ml; a line pulled back (at most 2 ml) fills first on the next run; faults: outlet blocked (the pump runs, nothing drips), a canister above the device that siphons 0.6 ml/min while its pump stands |
| Liquids | pH−, Part A and B, CalMag with their effects on EC and pH (RAT-053, RAT-055, RAT-080) |
| Probes | pH and EC with their own errors, measured noise (RAT-082); faults: offline, frozen, jump, dry EC; calibration solutions |
| Network devices | twins the simulation answers itself, without a network (SD-132): `shelly-1.local`, a Shelly Gen2 relay with one output and one input that out of the box restores its last state after a power loss and is switched by its input; `shelly-plug.local`, a Shelly Gen2 plug without an input, into which a circulation pump in the tank is plugged: while it runs, the tank mixes as with 20 L/min of flow (an **assumption**); `shelly-old.local`, a first-generation Shelly without the RPC interface; `shelly-valve.local`, a Shelly Gen2 relay at the tank's inlet, set up so that its input does not switch it: its output lets tap water into the tank at 4 L/min (EC 0.45, pH 7.6; **assumptions**) and its input is the float switch, which says full from 105 L (an **assumption**); `shelly-pump.local`, a Shelly Gen2 relay with two outputs, each a watering pump that takes 2 L/min from the tank (an **assumption**); `shelly-dimmer.local`, a Shelly Gen2 dimmer for a 0–10 V line with one light whose brightness it keeps (SD-135); `shelly-climate.local`, a Shelly Gen2 relay with four metered outputs in a grow room, its exhaust fan, circulation fan, heater and dehumidifier, and `shelly-climate2.local`, one with two outputs, its humidifier and CO2 valve; `esp-climate.local`, an ESPHome device with the room's `sensor/air_temp`, `sensor/air_rh` and `sensor/co2` (SD-136); `esp-1.local`, an ESPHome device with `binary_sensor/tank_full`, `sensor/water_temp` (21.5), and `sensor/tank_level` and `binary_sensor/tank_empty`, which follow the tank: its litres, and empty below 20 L (an **assumption**). A Shelly's settings act as the real one's: its auto-off switches it off, and back from a power loss its outputs follow their initial state |
| Grow room | the air of the room the climate twins sit in (SD-136), all **assumptions**: outside 18 °C, 55 % and 420 ppm; through its walls the room follows the outside with a time constant of 30 min, and with the exhaust fan of about 5 min; the lamps of `shelly-dimmer.local` at full brightness warm a closed room by 14 °C and the heater by 12 °C; the plants wet the air by 8 % in the dark and 20 % with full light, the humidifier and the dehumidifier move it by 25 %; the plants take up 0.3 ppm/s of CO2 with full light, and the open valve gives 3 ppm/s. Tests may hold the air at values of their own |

The panel in the web app (button "Simulator") shows the true tank and
switches the faults; `POST /api/v1/sim/<action>` does the same for tests.
Scenario tests (`tests/core/test_scenarios.cpp`) drive the core through
the API with the harness in `tests/core/harness.hpp` and read what the
core cannot see from the twin.
The water path's faults are in the panel and in the API: `flowFault` with
`{"fault": "dry"}`, `returnFault` with `{"fault": "blocked"}` or
`{"fault": "failed"}`, `leak` with `{"on": true}`, `stopButton` with
`{"pressed": true}`, and on `channelFault` the keys `outletBlocked` and
`siphon`. `channelHead` with `{"channel": 2, "head": "M"}` fits another
head, `"head": ""` takes it off (SD-108). `lineEmpty` with
`{"channel": 4}` fills a channel's line with air, as after a canister
change: it holds 6 ml (an **assumption**), which a run fills before the
outlet drips (SD-111).
The network devices' twins (SD-132): `netOnline` with
`{"address": "shelly-1.local", "online": false}` takes a device away, as a
power loss would, and `true` brings it back; `netReachable` with
`{"address": "shelly-1.local", "reachable": false}` cuts only the network
to it; `netId` with `{"address": "shelly-1.local", "id": "…"}` puts
another device at the address; `netInput` with
`{"address": "shelly-1.local", "channel": 0, "state": true}` sets a
Shelly's input, which switches its output while it is not detached; `netValue` with `{"address": "esp-1.local", "entity":
"binary_sensor/tank_full", "value": true}` sets an ESPHome value;
`netFloat` with `{"stuck": true}` makes the inlet's float switch stick, so
that it never says full (RAT-031); the inlet's float follows the tank and
takes no `netInput`. The
simulator's MQTT link also subscribes to the topics roles read, without
TLS, since values need none (SD-130).
