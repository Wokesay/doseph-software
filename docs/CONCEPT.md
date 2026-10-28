# Concept

How the software of the mixing robot is built and the rules it keeps.
What the robot does step by step is decided in SD-105; why the domain
rules are as they are is in `docs/RATIONALE.md`.

## One core, two platforms

The domain core is platform-neutral C++17 (`core/`). The same code runs in
the simulator (`sim/`), against a digital twin of tank, measuring chamber,
flow, pumps and probes, and later on the device, where only the platform
interfaces are replaced: `IHardware` (the device's profile, pumps,
circulation pump, raw readings), `IStorage` (files), `IClock` (time) and
the HTTP binding. The profile says what the device has (SD-108); logic
reads its capabilities, never a line name.

## Layers of the core

```
  web app ── REST /api/v1 + live stream ── Api (api.cpp)
                                             │
                                          Robot (robot.cpp): settings, jobs, state, files
                                             │
        ┌──────────────┬──────────────┬──────┴───────┬───────────────┐
  SensorTruth      MixCycle        Doser/Actuators   Watchdog       EventLog,
  (truth.cpp)      (cycle.cpp)     (gateway.cpp)     (watchdog.cpp) History,
  raw → readings   the state       the only path     assesses only  DoseJournal
  with quality     machine         to the pumps
        │              │                 │
        └── plan.cpp: EC and pH rounds, partial runs, effects
                                         │
                                    IHardware
```

- **Sensor truth** turns raw values into readings with a quality: offline,
  no data, stale, frozen, implausible, jump, not calibrated or OK. Only an
  OK reading is used for a dose. EC is compensated to 25 °C (RAT-089).
- **The mixing cycle** is one state machine: pump on, prove flow, flush,
  measure until stable, EC before pH, dose in partial doses, mix in (the
  clock runs only with flow), measure again and learn, hold (SD-105).
- **The gateway** is the only code that switches pumps (R1). It refuses a
  dose into the tank without flow, while stopped, on an uncalibrated pump,
  shorter than 1 s or longer than 60 s, or while another run is active.
- **The dosing journal** records a run before its pump starts (SD-106).
- **The watchdog** reads the read model and the configuration only and
  never switches anything (R2, RAT-074).

## Rules

| Rule | |
|---|---|
| R1 | Only the gateway switches pumps. |
| R2 | The watchdog reads only the read model and the configuration. |
| R5 | A missing value is never 0 and never a dose. |
| R6 | After a crash or restart everything is off; the robot reports and does not resume a dose. |
| R7 | Settings only tighten within hard limits. |
| R8 | One event loop with an injected clock. |

`tools/arch_check.sh` checks R1, R2 and R5 in the code.

## Operation

Only through the web app on the device, on the home network: a mandatory
password at the first start (with a setup code the device shows), sessions
in a cookie, a lockout per client after failed attempts, host and origin
checks against DNS rebinding and cross-site requests. The app speaks
English and German; the robot sends English messages with a key, and the
app shows them in German by that key.

## Not built yet

These follow in the order of work of #10: the twin of the new water path
with drop sensors, push notifications and maintenance reminders, plant
recipes with phases and a calendar, the app and commissioning, coupling
two devices, Shelly and ESP devices (climate, light, watering, a refill
valve), and an optional account. Control stays on the device in every
step.
