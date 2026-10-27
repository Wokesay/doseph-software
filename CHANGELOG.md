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
- Simulator with a twin of tank, measuring chamber, flow, pumps and probes.

### Changed

- Started from growcontroller-software at a3b00d1 without its history
  (`docs/PROVENANCE.md`); grow-only parts left behind.
- The repository is public, under AGPL-3.0-or-later with the additional
  permission (SD-116). New: `SUPPORT.md`, `CODE_OF_CONDUCT.md`, issue
  forms for bugs and feedback; `SECURITY.md` reports through GitHub's
  private vulnerability reporting.
- Two recipe templates name no manufacturer any more: "Three-part feed,
  vegetative wk 1–4" and "… flowering wk 1–2", with the same values
  (SD-116).
- Doseph's terms (SD-107): a bottle is now a canister, and what it holds
  is its role (nutrient or pH−); the flow cell is the measuring chamber.
  The API names them `canisters`, `canister` and `role`
  (`POST /api/v1/canisters`, `DELETE /api/v1/canisters/<id>`). A saved
  configuration of schema 1 is read and brought to schema 2.
