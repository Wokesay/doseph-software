## What changes?

<!-- Short, from the user's point of view. Link the issue or decision (SD-xxx). -->

## Safety and invariants

- [ ] Pumps only through the gateway, watchdog without a path to them (`tools/arch_check.sh`)
- [ ] A missing value stays missing, never 0
- [ ] No dose without proven flow; a dose is in the journal before its pump starts
- [ ] New domain rule? RAT entry in `docs/RATIONALE.md` and a test that names it

## Tests

- [ ] `tools/ci.sh` green, for UI changes also `E2E=1 tools/ci.sh`
- [ ] New logic has a unit or scenario test

## Language and privacy

- [ ] Everything in English
- [ ] No hardware data, no private sources, no prices
- [ ] Entry in `CHANGELOG.md` under `[Unreleased]`
