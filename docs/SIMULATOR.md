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

The twin has one pump per channel. The water path behind the flow
method, the return and the drop sensors follows in #10, step 3.

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
| Flow | the suction pump delivers 12 L/min; faults: blocked, broken sensor; a pump of your own |
| Dosing channels | four, each pump with its own true flow (42–53 ml/min), may be blocked or empty; calibration and priming go into a cup |
| Liquids | pH−, Part A and B, CalMag with their effects on EC and pH (RAT-053, RAT-055, RAT-080) |
| Probes | pH and EC with their own errors, measured noise (RAT-082); faults: offline, frozen, jump, dry EC; calibration solutions |

The panel in the web app (button "Simulator") shows the true tank and
switches the faults; `POST /api/v1/sim/<action>` does the same for tests.
