# Contributing

Bug reports, feedback and ideas are welcome as issues
([`SUPPORT.md`](SUPPORT.md)). Code comes only from the project's own
sessions and its owner for now: pull requests from outside are not
merged, so the project keeps one copyright holder while the product takes
shape. If you have a fix, describe it in an issue.

How the project works on its code:

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
- **Headers:** every source file carries
  `SPDX-License-Identifier: AGPL-3.0-or-later`.
- **Releases:** a prototype comes out weekly while there is something new
  (SD-116).
