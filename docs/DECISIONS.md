# Software decisions (SD)

Binding decisions for the software. Format `## SD-XXX: Title`, sequential,
chronological, two blank lines between entries. No status field; entries
are not deleted, a later SD replaces an earlier one and refers to it.

Principles, costs and hardware are decided by the product session; a
product question is filed as an issue with the label `product-question`.
Control details (locks, releases, thresholds, sequences, waits) are decided
here by the minimal principle: affordable, simple, a safety frame instead
of guarding every scenario; a crash or restart switches off everything
that can do harm.

**Numbering.** SD-001 to SD-103 are the decisions of the predecessor
growcontroller-software (`docs/PROVENANCE.md`) and are cited by their
number. SD-104 is left out, as an open change in the predecessor may have
taken it. This log continues with SD-105.


## SD-105: The mixing robot: one check as a state machine, flow first, a test dose for a new tank

2026-10-24. The product is no longer a grow controller but a mixing robot
at the nutrient tank (decided by the product session). This entry decides
the control flow; the values are starting points, not fixed.

**The check.** One state machine (`core/src/cycle.cpp`):

1. **Pump on** – with circulation `cycle` the robot switches the
   circulation pump on; with `always` it runs anyway; with `external` the
   user's own pump has to deliver.
2. **Prove flow** – at least 0.5 L/min without a break for 10 s. No flow
   within 2 min: the pump goes off, the check waits 5 min and tries again,
   and the watchdog reports it.
3. **Flush** – 60 s of flow, so the measuring cell holds tank water.
4. **Measure until stable** – pH within 0.05 and EC within 0.03 for 60 s;
   not stable within 15 min: wait and try again.
5. **Evaluate EC before pH** (RAT-004). EC rounds aim below the target by
   the reserve for the pH− that follows (RAT-053); pH waits below the EC
   floor and 240 s after a nutrient dose (RAT-046).
6. **Dose in partial doses** (RAT-054, RAT-055, RAT-050).
7. **Mix in** – the user's mixing wait; the clock runs only while water
   flows. 30 min without flow: wait.
8. **Re-measure and learn** the effect from clean rounds (RAT-057); two
   rounds without effect latch (RAT-055, RAT-020).
9. **Hold** – on target, the circulation pump off (with `cycle`), the next
   check after the pause between checks (default 60 min).

**Never dose without proven flow.** The gateway refuses a run into the
tank without flow (`act.no_flow`) and stops a running one at once when the
flow stops. A calibration or priming into a cup needs no flow.

**The mixing wait** after a dose is set by the user per tank, for EC and
for pH. A value from tank volume and measured flow is only a suggestion:
about 2 turnovers after nutrients, 3 after pH−, in whole minutes, between 2
and 600 min. Without a value of the user's, the suggestion applies. Whether
the suggestion acts as a lower bound is open (product question #1); until
it is decided it does not, and the web app says when a wait is shorter.

**A new tank** – the first start, a changed volume or "New tank" – gets a
small test dose first: EC rounds aim at a rise of 0.1 mS/cm at the largest
effect the recipe may have; a pH− test dose at a drop of 0.1. If the rise
is more than twice or less than a third of what the entered volume
expects, dosing stops (latch `tank.volume`) until the user corrects the
volume or confirms it. A test dose that passed is learned; one that did not
is not. For pH only the "much stronger" side is judged, since pH− acts on
a buffer the robot does not know.

**The recipe in force** is chosen with how long it applies: a number of
days (1–366), until a date (at most a year ahead) or without an end. Once it
has ended, the robot doses nothing and says so.

**Out of scope:** climate, light, watering, tank refill and drain; mixing in
a bucket or watering can; stock of bottles; notifications and a store app
(later).


## SD-106: A dose is written to the dosing journal before its pump starts

2026-10-24. Every run that goes into the tank is recorded in
`journal.json` (written atomically) before the gateway starts the pump,
and its end afterwards. A run whose record cannot be written does not
start. After a restart an open record is reported once as cut ("up to
x ml may be in the tank – how much is unknown"), and the first check mixes
the tank before it measures and learns nothing from it (R6, RAT-028). A
damaged journal counts as "a dose may be in", never as nothing (R5).
Calibration and priming runs go into a cup and are only recorded. The
journal is cleared when the tank is on target.


## SD-117: Doseph stands alone; the predecessor is deleted

2026-10-27. Decided by the owner.

- **No advisories.** The predecessor's software runs on no device and
  nobody downloaded it, so advisories would protect nobody.
- **The predecessor's repositories** are private and archived. The owner
  deletes them once everything Doseph needs is in this repository. No
  backup is kept.
- **No references to the predecessor.** Code, rules and their reasons
  live here. No `gc#n`, no `gc PD-`, `gc SD-` or `gc RAT-` IDs, no commit
  hashes, no links. Where a reference carries a reason, the reason is
  written here (this log, `docs/RATIONALE.md`, or the comment itself);
  otherwise the reference is dropped. RAT and SD IDs keep their numbers.
  New work adds no such reference.
- **Last import step:** the remaining references go, `docs/PROVENANCE.md`
  and the ID section of `CLAUDE.md` are rewritten to need nothing outside
  this repository, and `tools/arch_check.sh` checks that none comes back.
  Then the predecessor can be deleted.
- **Copyright.** `REUSE.toml` names "The doseph-software contributors".
  The predecessor's contributors are the same, so the copyright holder
  does not change.
