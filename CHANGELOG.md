# Changelog

All notable changes to this software. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- The mixing robot: one check as a state machine – pump on, prove flow,
  flush, measure until stable, EC before pH, dose in partial doses, mix in
  with flow, measure again and learn, hold; no dose without proven flow; a
  small test dose that checks the volume of a new tank (SD-105).
- The dosing journal: a dose is on record before its pump starts, and one
  cut by a restart is reported and mixed before the next measurement
  (SD-106).
- EC compensated to 25 °C; without a water temperature EC is no value for
  control (RAT-089).
- Web app: status, recipe with how long it applies, tank with volume, flow,
  turnover time and mixing waits, history of pH, EC, temperature and flow,
  calibration of the dosing pumps and probes, settings, and the Stop on
  every page.
- Simulator with a twin of tank, measuring cell, flow, pumps and probes.
- "Why it doses – or not" on the status page: a checklist of what
  nutrients and pH− each need now – recipe and tank volume, flow at the
  last check, readings, canisters on calibrated pumps, locks, the EC gate
  and the rest after a nutrient dose – each ticked or not, with the
  reason (RAT-037, from the predecessor). The API has it as
  `cycle.checks`.

### Changed

- Started from growcontroller-software at a3b00d1 without its history
  (`docs/PROVENANCE.md`); grow-only parts left behind.
