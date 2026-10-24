# Contributing

This repository is private; changes come from the project's sessions and
its owner.

- **Language:** everything in the repository is English.
- **Branches and pull requests:** one topic per branch, a draft pull
  request, the owner merges. Commit messages say what changes and why.
- **Checks:** `tools/ci.sh` must pass; for changes to the web app also
  `E2E=1 tools/ci.sh` (`docs/SIMULATOR.md`).
- **Tests:** new logic gets a unit or scenario test; a new domain rule a
  RAT entry in `docs/RATIONALE.md` and a test that names it.
- **Decisions:** control details as SD in `docs/DECISIONS.md`; product
  questions as an issue with the label `product-question`.
- **Privacy:** no hardware data (schematic, layout, bill of materials,
  exact board data), no names, numbers or texts of private sources, no
  prices – also not in issues. Refer to them as "private sources" or
  "reference installation".
- **Headers:** a new source file carries the same SPDX header as the
  others; `LICENSE` and the headers stay unchanged until the license is
  decided.
