// SPDX-License-Identifier: AGPL-3.0-or-later
// Plants and their phases (SD-128): which plant sets the water, the running
// plants with their phase and what is due, the calendar of the next weeks,
// and the plant recipes their phases come from.
import { useEffect, useState } from "preact/hooks";
import { CalendarDays, ListTree, Pencil, Plus, Sprout, Trash2 } from "lucide-preact";
import { del, get, post, put, type Calendar, type Plant, type PlantPhase, type PlantRecipe, type PlantRecord, type Plants } from "../api";
import { day, num } from "../format";
import { msg, t } from "../i18n";
import { activeRecipe, config, recipes, refreshConfig, refreshState, state, toast, toastError } from "../store";
import { Button, Card, Empty, Field, Modal, NumberInput, Pill, Toggle } from "../ui";

// Today's date for a date field, in the browser's local time
const todayInput = () => {
  const d = new Date();
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, "0")}-${String(d.getDate()).padStart(2, "0")}`;
};

function PlanCard() {
  const plan = state.value!.plan;
  const r = activeRecipe.value;
  return (
    <Card title={t("plants.plan")} icon={<Sprout size={18} />}>
      <p data-testid="plants-plan">
        {plan.plant
          ? t("plants.planByPlant", { name: plan.plant.name, phase: plan.plant.phase, recipe: r?.name ?? "–" })
          : plan.source === "hand"
            ? t("plants.planByHand", { recipe: r?.name ?? "–" })
            : t("plants.planNone")}
      </p>
    </Card>
  );
}

function PlantRow(p: { plant: Plant; data: Plants; onChange: (d: Plants) => void }) {
  const pl = p.plant;
  const [editing, setEditing] = useState(false);
  const ph = pl.phases[pl.phase];
  const last = pl.phase + 1 === pl.phases.length;
  const act = async (path: string, ask?: string) => {
    if (ask && !confirm(ask)) return;
    try {
      p.onChange(await post<Plants>(`/plants/${encodeURIComponent(pl.id)}/${path}`));
      await Promise.all([refreshConfig(), refreshState()]);
    } catch (e) {
      toastError(e);
    }
  };
  const remove = async () => {
    if (!confirm(t("plants.removeAsk", { name: pl.name }))) return;
    try {
      p.onChange(await del<Plants>(`/plants/${encodeURIComponent(pl.id)}`));
      await Promise.all([refreshConfig(), refreshState()]);
    } catch (e) {
      toastError(e);
    }
  };
  const when = last ? "plants.lastEnds" : ph.confirm ? "plants.waitsConfirm" : "plants.ends";
  return (
    <div class="item wrap-row" data-testid={`plant-${pl.id}`}>
      <div class="grow">
        <div class="title">
          {pl.name} <span class="muted small">{t("plants.day", { n: pl.day })}</span> {pl.leading && <Pill tone="ok">{t("plants.leading")}</Pill>}
        </div>
        <div class="small" data-testid={`plant-phase-${pl.id}`}>
          {t("plants.phaseOf", { phase: ph.name, n: pl.phaseDay ?? 1, days: ph.days })}
          {pl.phaseEnd ? ` · ${t(when, { date: day(pl.phaseEnd) })}` : ""}
        </div>
        <div class="muted small">
          {t("plants.targets", { ec: pl.ecTarget != null ? num(pl.ecTarget, 2) : "–", ph: pl.phTarget != null ? num(pl.phTarget, 2) : "–" })}
          {pl.harvestAt ? ` · ${t("plants.harvested", { date: day(pl.harvestAt) })}` : ""}
        </div>
        {pl.daysOver ? (
          <div>
            <Pill tone="info">{t("plants.over", { n: pl.daysOver })}</Pill>
          </div>
        ) : null}
      </div>
      <div class="row-actions">
        {!last && (
          <Button size="sm" onClick={() => act("next")} data-testid={`plant-next-${pl.id}`}>
            {t("plants.next")}
          </Button>
        )}
        {!pl.harvestAt && (
          <Button size="sm" onClick={() => act("harvest", t("plants.harvestAsk", { name: pl.name }))} data-testid={`plant-harvest-${pl.id}`}>
            {t("plants.harvest")}
          </Button>
        )}
        <Button size="sm" onClick={() => setEditing(true)} aria-label={t("plants.editPhasesAria", { name: pl.name })} data-testid={`plant-phases-${pl.id}`}>
          {t("plants.editPhases")}
        </Button>
        {!pl.leading && (
          <Button size="sm" onClick={() => act("lead")} data-testid={`plant-lead-${pl.id}`}>
            {t("plants.lead")}
          </Button>
        )}
        <Button size="sm" onClick={() => act("complete", t("plants.completeAsk", { name: pl.name }))} data-testid={`plant-complete-${pl.id}`}>
          {t("plants.complete")}
        </Button>
        <Button size="sm" variant="danger-soft" onClick={remove} aria-label={`${t("plants.remove")}: ${pl.name}`}>
          <Trash2 size={15} />
        </Button>
      </div>
      {editing && <PlantPhasesEditor plant={pl} data={p.data} onClose={() => setEditing(false)} onDone={p.onChange} />}
    </div>
  );
}

// A running plant's own phases (SD-137): the current one and those to come change; those past stay
// as they were, and the current one stays. Only this plant changes, not its plant recipe.
function PlantPhasesEditor(p: { plant: Plant; data: Plants; onClose: () => void; onDone: (d: Plants) => void }) {
  const systemPh = p.data.system ? p.data.systems[p.data.system] : null;
  const current = p.plant.phase;
  const [phases, setPhases] = useState<PlantPhase[]>(p.plant.phases);
  const set = (i: number, part: Partial<PlantPhase>) => setPhases(phases.map((x, n) => (n === i ? { ...x, ...part } : x)));
  const save = async () => {
    try {
      p.onDone(await put<Plants>(`/plants/${encodeURIComponent(p.plant.id)}`, { phases }));
      await Promise.all([refreshConfig(), refreshState()]);
      toast(t("plants.saved"));
      p.onClose();
    } catch (e) {
      toastError(e);
    }
  };
  return (
    <Modal
      wide
      title={t("plants.editPhasesOf", { name: p.plant.name })}
      onClose={p.onClose}
      footer={
        <>
          <button class="btn" onClick={p.onClose}>
            {t("common.cancel")}
          </button>
          <Button variant="primary" onClick={save} disabled={phases.some((x) => !x.name.trim())} data-testid="save-plant-phases">
            {t("common.save")}
          </Button>
        </>
      }
    >
      <div class="stack" data-testid="plant-phases-form">
        <p class="muted small">{t("plants.editPhasesNote")}</p>
        {phases.map((ph, i) =>
          i < current ? (
            <div class="muted small" data-testid={`phase-${i}`}>
              {t("plants.phasePast", { n: i + 1, name: ph.name, days: ph.days })}
            </div>
          ) : (
            <fieldset class="stack-sm" data-testid={`phase-${i}`}>
              <legend class="small">
                {t("plants.phaseN", { n: i + 1 })}
                {i === current && ` · ${t("plants.phaseCurrent")}`}
              </legend>
              <PhaseFields ph={ph} i={i} set={set} plant />
              <div class="wrap-row">
                <Toggle checked={ph.confirm} onChange={(v) => set(i, { confirm: v })} label={t("plants.confirm")} />
                {i > current && (
                  <Button size="sm" variant="danger-soft" onClick={() => setPhases(phases.filter((_, n) => n !== i))}>
                    {t("plants.removePhase")}
                  </Button>
                )}
              </div>
            </fieldset>
          ),
        )}
        <div>
          <Button size="sm" onClick={() => setPhases([...phases, newPhase(phases[phases.length - 1]?.recipe ?? recipes.value[0]?.id ?? "", systemPh)])} data-testid="add-plant-phase">
            <Plus size={15} /> {t("plants.addPhase")}
          </Button>
        </div>
      </div>
    </Modal>
  );
}

function StartDialog(p: { data: Plants; onClose: () => void; onDone: (d: Plants) => void }) {
  const [name, setName] = useState("");
  const [from, setFrom] = useState(p.data.plantRecipes[0]?.id ?? "");
  const [start, setStart] = useState(todayInput());
  const go = async () => {
    try {
      const [y, m, d] = start.split("-").map(Number);
      // Noon of the chosen day in the browser's time: the robot counts from that day's local midnight
      const at = Math.floor(new Date(y, m - 1, d, 12).getTime() / 1000);
      const body: Record<string, unknown> = { name: name.trim(), plantRecipe: from };
      if (start !== todayInput()) body.start = Math.min(at, Math.floor(Date.now() / 1000));
      p.onDone(await post<Plants>("/plants", body));
      await Promise.all([refreshConfig(), refreshState()]);
      toast(t("plants.startedToast"));
      p.onClose();
    } catch (e) {
      toastError(e);
    }
  };
  return (
    <Modal
      title={t("plants.start")}
      onClose={p.onClose}
      footer={
        <>
          <button class="btn" onClick={p.onClose}>
            {t("common.cancel")}
          </button>
          <Button variant="primary" onClick={go} disabled={!name.trim() || !from} data-testid="start-plant">
            {t("plants.start")}
          </Button>
        </>
      }
    >
      <div class="form-grid">
        <Field label={t("common.name")}>
          <input class="input" value={name} maxLength={40} onInput={(e) => setName((e.target as HTMLInputElement).value)} name="plant-name" />
        </Field>
        <Field label={t("plants.from")}>
          <select class="select input" value={from} onChange={(e) => setFrom((e.target as HTMLSelectElement).value)} name="plant-recipe">
            {p.data.plantRecipes.map((r) => (
              <option value={r.id}>{r.name}</option>
            ))}
          </select>
        </Field>
        <Field label={t("plants.startDate")} hint={t("plants.startHint")}>
          <input class="input" type="date" value={start} max={todayInput()} onInput={(e) => setStart((e.target as HTMLInputElement).value)} name="plant-start" />
        </Field>
      </div>
    </Modal>
  );
}

const amount = (ml: number) => num(ml, ml < 10 ? 1 : 0);

// A completed plant's record, frozen at its end, and its yield by hand (RAT-064, SD-138)
function RecordLines(p: { rec: PlantRecord }) {
  const r = p.rec;
  const known = r.countedFrom != null && !r.countLost;
  const list = r.canisters
    .map((c) => (c.unknown ? t("plants.rec.amountUnknown", { name: c.name, ml: amount(c.ml), n: c.unknown }) : t("plants.rec.amount", { name: c.name, ml: amount(c.ml) })))
    .join(", ");
  return (
    <div class="small stack-sm">
      <div>{t(r.days === 1 ? "plants.rec.span1" : "plants.rec.span", { from: day(r.from), to: day(r.to), days: r.days })}</div>
      <div>
        {r.phases
          .map((ph) => (ph.days == null ? t("plants.rec.phaseUnknown", { name: ph.name }) : t(ph.days === 1 ? "plants.rec.phase1" : "plants.rec.phase", { name: ph.name, days: ph.days })))
          .join(" → ")}
        {r.harvestAt ? ` · ${t("plants.rec.harvest", { date: day(r.harvestAt) })}` : ""}
      </div>
      {known ? (
        <>
          <div>{list ? t("plants.rec.amounts", { list }) : t("plants.rec.nothing")}</div>
          {r.wateredL != null && (r.wateredL > 0 || r.wateredUnknown > 0) && (
            <div>
              {r.wateredUnknown ? t("plants.rec.wateredUnknown", { l: num(r.wateredL, 1), n: r.wateredUnknown }) : t("plants.rec.watered", { l: num(r.wateredL, 1) })}
            </div>
          )}
          {r.with.length > 0 && <div class="muted">{t("plants.rec.with", { names: r.with.join(", ") })}</div>}
        </>
      ) : (
        <div class="muted">{r.countLost ? t("plants.rec.lost") : t("plants.rec.notCounted")}</div>
      )}
    </div>
  );
}

function CompletedPlant(p: { plant: Plant; onChange: (d: Plants) => void }) {
  const pl = p.plant;
  const [yieldG, setYieldG] = useState<number | null>(pl.yieldG);
  const save = async () => {
    try {
      p.onChange(await put<Plants>(`/plants/${encodeURIComponent(pl.id)}`, { yieldG }));
      toast(t("plants.yieldSaved"));
    } catch (e) {
      toastError(e);
    }
  };
  return (
    <div class="item stacked" data-testid={`plant-done-${pl.id}`}>
      <div class="title">{t("plants.completedOn", { name: pl.name, date: day(pl.completedAt) })}</div>
      {pl.record && <RecordLines rec={pl.record} />}
      <Field label={t("plants.yield")} hint={t("plants.yieldHint")}>
        <NumberInput value={yieldG} onValue={setYieldG} unit="g" min={0} max={1000000} name={`plant-yield-${pl.id}`} />
      </Field>
      <div>
        <Button size="sm" onClick={save} disabled={yieldG === pl.yieldG} aria-label={t("plants.yieldSaveAria", { name: pl.name })} data-testid={`plant-yield-save-${pl.id}`}>
          {t("plants.yieldSave")}
        </Button>
      </div>
    </div>
  );
}

function PlantsCard(p: { data: Plants; onChange: (d: Plants) => void }) {
  const [starting, setStarting] = useState(false);
  const running = p.data.plants.filter((x) => x.running);
  const done = p.data.plants.filter((x) => !x.running);
  const canStart = p.data.plantRecipes.length > 0;
  return (
    <Card
      title={t("plants.running")}
      icon={<Sprout size={18} />}
      actions={
        <Button size="sm" variant="primary" onClick={() => setStarting(true)} disabled={!canStart} data-testid="open-start-plant">
          <Plus size={15} /> {t("plants.start")}
        </Button>
      }
    >
      <div class="stack">
        {running.length ? (
          <div class="list">
            {running.map((pl) => (
              <PlantRow plant={pl} data={p.data} onChange={p.onChange} />
            ))}
          </div>
        ) : (
          <Empty icon={<Sprout size={22} />} title={t("plants.none")} text={canStart ? t("plants.noneText") : t("plants.needRecipe")} />
        )}
        {done.length > 0 && (
          <details>
            <summary>{t("plants.completed")}</summary>
            <div class="list" data-testid="plants-completed">
              {done.map((x) => (
                <CompletedPlant plant={x} onChange={p.onChange} />
              ))}
            </div>
          </details>
        )}
      </div>
      {starting && <StartDialog data={p.data} onClose={() => setStarting(false)} onDone={p.onChange} />}
    </Card>
  );
}

function CalendarCard(p: { cal: Calendar | null }) {
  if (!p.cal) return null;
  const dates = [...new Set(p.cal.entries.map((e) => e.date))];
  const text = (e: Calendar["entries"][number]) =>
    e.kind === "maintenance"
      ? t("plants.cal.maintenance", { items: (e.items ?? []).map((i) => msg(i)).join(", ") })
      : t(`plants.cal.${e.kind}`, { name: e.name, phase: e.phase });
  return (
    <Card title={t("plants.calendar")} icon={<CalendarDays size={18} />}>
      <div class="stack" data-testid="calendar">
        <p class="muted small">{t("plants.calendarIntro")}</p>
        {dates.length ? (
          dates.map((d) => (
            <div class="stack-sm">
              <div class="section-title">{d === p.cal!.today ? t("plants.today") : day(d)}</div>
              <ul class="small">
                {p.cal!.entries
                  .filter((e) => e.date === d)
                  .map((e) => (
                    <li data-testid={`cal-${e.kind}`}>
                      {text(e)} {e.overdue && <Pill tone="info">{t("plants.overdue")}</Pill>}
                    </li>
                  ))}
              </ul>
            </div>
          ))
        ) : (
          <p class="muted">{t("plants.calendarEmpty")}</p>
        )}
      </div>
    </Card>
  );
}

// A new phase: its recipe's pH range, else the cultivation system's (SD-128)
const newPhase = (recipe: string, systemPh: [number, number] | null): PlantPhase => ({
  name: "",
  days: 14,
  confirm: false,
  recipe,
  ecTarget: null,
  ph: recipes.value.find((r) => r.id === recipe)?.ph ? null : systemPh,
});

// A new plant recipe's proposal (RAT-066): growth at 18 h of light until the user confirms, since the
// switch to 12 h starts flowering; then 56 days of flowering at 12 h, with the second recipe where
// there is one. The names are labels only (RAT-076).
const proposal = (systemPh: [number, number] | null): PlantPhase[] => {
  const first = recipes.value[0]?.id ?? "";
  const second = recipes.value[1]?.id ?? first;
  return [
    { ...newPhase(first, systemPh), name: t("plants.proposeGrowth"), days: 21, confirm: true, lightHours: 18 },
    { ...newPhase(second, systemPh), name: t("plants.proposeBloom"), days: 56, lightHours: 12 },
  ];
};

// The fields of one phase, as a plant recipe and a running plant edit them
// `plant`: a running plant's own: the hints on plants started from a recipe do not apply
function PhaseFields(p: { ph: PlantPhase; i: number; set: (i: number, part: Partial<PlantPhase>) => void; plant?: boolean }) {
  const list = recipes.value;
  const { ph, i, set } = p;
  const hint = (key: Parameters<typeof t>[0]) => (p.plant ? undefined : t(key));
  // Below the EC floor pH is never dosed (RAT-046): said where a phase's target lies there
  const ec = ph.ecTarget ?? list.find((r) => r.id === ph.recipe)?.ecTarget ?? null;
  const floor = config.value!.control.ecFloor;
  return (
    <div class="form-grid">
      <Field label={t("plants.phaseName")}>
        <input class="input" value={ph.name} maxLength={40} onInput={(e) => set(i, { name: (e.target as HTMLInputElement).value })} name={`phase-name-${i}`} />
      </Field>
      <Field label={t("plants.days")}>
        <NumberInput value={ph.days} onValue={(v) => set(i, { days: v ?? 1 })} min={1} max={999} name={`phase-days-${i}`} />
      </Field>
      <Field label={t("plants.recipe")}>
        <select class="select input" value={ph.recipe} onChange={(e) => set(i, { recipe: (e.target as HTMLSelectElement).value })} name={`phase-recipe-${i}`}>
          {list.map((r) => (
            <option value={r.id}>{r.name}</option>
          ))}
        </select>
      </Field>
      <Field
        label={t("plants.ecOwn")}
        hint={
          ec != null && ec < floor ? (
            <span class="warn-text" data-testid={`phase-ec-floor-${i}`}>
              {t("plants.ecBelowFloor", { ec: num(ec, 1), floor: num(floor, 1) })}
            </span>
          ) : (
            t("plants.ecOwnHint")
          )
        }
      >
        <NumberInput value={ph.ecTarget} onValue={(v) => set(i, { ecTarget: v })} unit="mS/cm" step={0.1} name={`phase-ec-${i}`} />
      </Field>
      <Field label={t("plants.phLow")} hint={t("plants.phOwnHint")}>
        <NumberInput value={ph.ph?.[0] ?? null} onValue={(v) => set(i, { ph: v == null ? null : [v, ph.ph?.[1] ?? v + 0.4] })} step={0.1} name={`phase-ph-low-${i}`} />
      </Field>
      <Field label={t("plants.phHigh")}>
        <NumberInput value={ph.ph?.[1] ?? null} onValue={(v) => set(i, { ph: v == null ? null : [ph.ph?.[0] ?? v - 0.4, v] })} step={0.1} name={`phase-ph-high-${i}`} />
      </Field>
      <Field label={t("plants.shotsPerDay")} hint={hint("plants.shotsHint")}>
        <NumberInput value={ph.shotsPerDay ?? null} onValue={(v) => set(i, { shotsPerDay: v == null ? null : Math.round(v) })} min={1} max={1440} name={`phase-shots-${i}`} />
      </Field>
      <Field label={t("plants.shotS")}>
        <NumberInput value={ph.shotS ?? null} onValue={(v) => set(i, { shotS: v })} unit="s" min={1} name={`phase-shot-${i}`} />
      </Field>
      <Field label={t("plants.lightHours")} hint={hint("plants.lightHint")}>
        <NumberInput value={ph.lightHours ?? null} onValue={(v) => set(i, { lightHours: v })} unit="h" min={0} max={24} step={0.5} name={`phase-light-hours-${i}`} />
      </Field>
      <Field label={t("plants.lightPct")} hint={t("plants.lightPctHint")}>
        <NumberInput value={ph.lightPct ?? null} onValue={(v) => set(i, { lightPct: v })} unit="%" min={1} max={100} name={`phase-light-pct-${i}`} />
      </Field>
      <Field label={t("plants.dayC")} hint={hint("plants.climateHint")}>
        <NumberInput value={ph.dayC ?? null} onValue={(v) => set(i, { dayC: v })} unit="°C" min={5} max={40} step={0.5} name={`phase-day-c-${i}`} />
      </Field>
      <Field label={t("plants.nightC")}>
        <NumberInput value={ph.nightC ?? null} onValue={(v) => set(i, { nightC: v })} unit="°C" min={5} max={40} step={0.5} name={`phase-night-c-${i}`} />
      </Field>
      <Field label={t("plants.rhPct")}>
        <NumberInput value={ph.rhPct ?? null} onValue={(v) => set(i, { rhPct: v })} unit="%" min={20} max={95} name={`phase-rh-${i}`} />
      </Field>
      <Field label={t("plants.vpdKpa")}>
        <NumberInput value={ph.vpdKpa ?? null} onValue={(v) => set(i, { vpdKpa: v })} unit="kPa" min={0.2} max={2.5} step={0.05} name={`phase-vpd-${i}`} />
      </Field>
      <Field label={t("plants.co2Ppm")}>
        <NumberInput value={ph.co2Ppm ?? null} onValue={(v) => set(i, { co2Ppm: v == null ? null : Math.round(v) })} unit="ppm" min={400} max={1500} step={50} name={`phase-co2-${i}`} />
      </Field>
    </div>
  );
}

function RecipeEditor(p: { recipe: PlantRecipe | null; data: Plants; onClose: () => void; onDone: (d: Plants) => void }) {
  const list = recipes.value;
  const systemPh = p.data.system ? p.data.systems[p.data.system] : null;
  const [name, setName] = useState(p.recipe?.name ?? "");
  const [initial] = useState<PlantPhase[]>(() => p.recipe?.phases ?? proposal(systemPh));
  const [phases, setPhases] = useState<PlantPhase[]>(initial);
  const set = (i: number, part: Partial<PlantPhase>) => setPhases(phases.map((x, n) => (n === i ? { ...x, ...part } : x)));
  const save = async () => {
    try {
      await put("/plant-recipes", { id: p.recipe?.id, name: name.trim(), phases });
      p.onDone(await get<Plants>("/plants"));
      await refreshConfig();
      toast(t("plants.saved"));
      p.onClose();
    } catch (e) {
      toastError(e);
    }
  };
  return (
    <Modal
      wide
      title={p.recipe ? t("plants.editRecipe") : t("plants.newRecipe")}
      onClose={p.onClose}
      footer={
        <>
          <button class="btn" onClick={p.onClose}>
            {t("common.cancel")}
          </button>
          <Button variant="primary" onClick={save} disabled={!name.trim() || !phases.length || phases.some((x) => !x.name.trim())} data-testid="save-plant-recipe">
            {t("common.save")}
          </Button>
        </>
      }
    >
      <div class="stack">
        <Field label={t("common.name")}>
          <input class="input" value={name} maxLength={40} onInput={(e) => setName((e.target as HTMLInputElement).value)} name="plant-recipe-name" />
        </Field>
        <div class="section-title">{t("plants.phases")}</div>
        {!p.recipe && phases === initial && <p class="muted small">{t("plants.proposal")}</p>}
        {systemPh && p.data.system && (
          <p class="muted small">{t("plants.systemPh", { system: t(`tank.system.${p.data.system}`), low: num(systemPh[0], 1), high: num(systemPh[1], 1) })}</p>
        )}
        {phases.map((ph, i) => (
          <fieldset class="stack-sm" data-testid={`phase-${i}`}>
            <legend class="small">{t("plants.phaseN", { n: i + 1 })}</legend>
            <PhaseFields ph={ph} i={i} set={set} />
            <div class="wrap-row">
              <Toggle checked={ph.confirm} onChange={(v) => set(i, { confirm: v })} label={t("plants.confirm")} />
              {phases.length > 1 && (
                <Button size="sm" variant="danger-soft" onClick={() => setPhases(phases.filter((_, n) => n !== i))}>
                  {t("plants.removePhase")}
                </Button>
              )}
            </div>
          </fieldset>
        ))}
        <div>
          <Button size="sm" onClick={() => setPhases([...phases, newPhase(phases[phases.length - 1]?.recipe ?? list[0]?.id ?? "", systemPh)])} data-testid="add-phase">
            <Plus size={15} /> {t("plants.addPhase")}
          </Button>
        </div>
      </div>
    </Modal>
  );
}

function RecipesCard(p: { data: Plants; onChange: (d: Plants) => void }) {
  const [editing, setEditing] = useState<PlantRecipe | null | undefined>(undefined);
  const remove = async (r: PlantRecipe) => {
    if (!confirm(`${t("common.delete")}: ${r.name}?`)) return;
    try {
      await del(`/plant-recipes/${encodeURIComponent(r.id)}`);
      p.onChange(await get<Plants>("/plants"));
      await refreshConfig();
    } catch (e) {
      toastError(e);
    }
  };
  return (
    <Card
      title={t("plants.recipes")}
      icon={<ListTree size={18} />}
      actions={
        <Button size="sm" onClick={() => setEditing(null)} disabled={!recipes.value.length} data-testid="new-plant-recipe">
          <Plus size={15} /> {t("plants.newRecipe")}
        </Button>
      }
    >
      {p.data.plantRecipes.length ? (
        <div class="list">
          {p.data.plantRecipes.map((r) => (
            <div class="item wrap-row" data-testid={`plant-recipe-${r.id}`}>
              <div class="grow">
                <div class="title">{r.name}</div>
                <div class="muted small">
                  {r.phases.map((x) => x.name).join(" → ")} · {t("plants.phaseCount", { n: r.phases.length, days: r.phases.reduce((a, x) => a + x.days, 0) })}
                </div>
              </div>
              <div class="row-actions">
                <Button size="sm" onClick={() => setEditing(r)} aria-label={`${t("common.edit")}: ${r.name}`}>
                  <Pencil size={15} />
                </Button>
                <Button size="sm" variant="danger-soft" onClick={() => remove(r)} aria-label={`${t("common.delete")}: ${r.name}`}>
                  <Trash2 size={15} />
                </Button>
              </div>
            </div>
          ))}
        </div>
      ) : (
        <p class="muted">{t("plants.recipesNone")}</p>
      )}
      {editing !== undefined && <RecipeEditor recipe={editing} data={p.data} onClose={() => setEditing(undefined)} onDone={p.onChange} />}
    </Card>
  );
}

export function PlantsPage() {
  const [data, setData] = useState<Plants | null>(null);
  const [cal, setCal] = useState<Calendar | null>(null);
  const rev = config.value!.revision;
  const load = () => {
    get<Plants>("/plants").then(setData).catch(() => {});
    get<Calendar>("/calendar").then(setCal).catch(() => {});
  };
  useEffect(load, [rev]);
  const change = (d: Plants) => {
    setData(d);
    get<Calendar>("/calendar").then(setCal).catch(() => {});
  };
  if (!data) return null;
  return (
    <>
      <PlanCard />
      <PlantsCard data={data} onChange={change} />
      <CalendarCard cal={cal} />
      <RecipesCard data={data} onChange={change} />
    </>
  );
}
