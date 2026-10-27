// SPDX-License-Identifier: AGPL-3.0-or-later
// Recipe: which recipe is in force and for how long; the recipes and
// their parts; templates to start from.
import { useEffect, useState } from "preact/hooks";
import { CalendarClock, FlaskConical, Pencil, Plus, Trash2 } from "lucide-preact";
import { del, get, post, put, type Recipe, type RecipeTemplate } from "../api";
import { dateTime, num } from "../format";
import { lang, t } from "../i18n";
import { LIMITS, canisters, config, phMiddle, recipes, refreshConfig, refreshState, toast } from "../store";
import { Banner, Button, Card, Empty, Field, Modal, NumberInput, Seg, Term, Toggle } from "../ui";

type Span = "days" | "until" | "open";

/** A date and time for <input type="datetime-local">, in the browser's time zone. */
function localInput(epoch: number) {
  const d = new Date(epoch * 1000);
  const p = (n: number) => String(n).padStart(2, "0");
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}T${p(d.getHours())}:${p(d.getMinutes())}`;
}

function ActiveCard() {
  const cfg = config.value!;
  const a = cfg.active;
  const list = recipes.value;
  const [recipe, setRecipe] = useState(a.recipe || list[0]?.id || "");
  const [span, setSpan] = useState<Span>(a.recipe && !a.until ? "open" : "days");
  const [days, setDays] = useState<number | null>(a.until && a.from ? Math.round((a.until - a.from) / 86400) : 28);
  const [until, setUntil] = useState(localInput((a.until || Date.now() / 1000 + 28 * 86400) | 0));
  const current = list.find((r) => r.id === a.recipe);
  const daysBad = span === "days" && (days === null || days < LIMITS.days[0] || days > LIMITS.days[1]);
  const apply = async () => {
    const body: Record<string, unknown> = { recipe };
    if (span === "days") body.days = days;
    if (span === "until") body.until = Math.floor(new Date(until).getTime() / 1000);
    await put("/active", body);
    await Promise.all([refreshConfig(), refreshState()]);
    toast(t("recipe.applied"));
  };
  const clear = async () => {
    await put("/active", { recipe: "" });
    await Promise.all([refreshConfig(), refreshState()]);
    toast(t("recipe.cleared"), "info");
  };
  return (
    <Card title={t("recipe.inForce")} icon={<CalendarClock size={18} />}>
      <div class="stack">
        {current ? (
          <p data-testid="active-recipe">
            {a.until
              ? t("recipe.activeUntil", { name: current.name, from: dateTime(a.from), until: dateTime(a.until) })
              : t("recipe.activeOpen", { name: current.name, from: dateTime(a.from) })}
          </p>
        ) : (
          <Banner tone="info">{t("recipe.noneActive")}</Banner>
        )}
        {list.length ? (
          <>
            <div class="form-grid">
              <Field label={t("recipe.choose")}>
                <select class="select input" value={recipe} onChange={(e) => setRecipe((e.target as HTMLSelectElement).value)} name="recipe">
                  {list.map((r) => (
                    <option value={r.id}>{r.name}</option>
                  ))}
                </select>
              </Field>
              <Field label={t("recipe.howLong")} group>
                <Seg<Span>
                  value={span}
                  onChange={setSpan}
                  options={[
                    ["days", t("recipe.span.days")],
                    ["until", t("recipe.span.until")],
                    ["open", t("recipe.span.open")],
                  ]}
                />
              </Field>
              {span === "days" && (
                <Field label={t("recipe.days")} error={daysBad ? t("recipe.daysRange", { min: LIMITS.days[0], max: LIMITS.days[1] }) : null}>
                  <NumberInput value={days} onValue={setDays} unit={t("recipe.daysUnit")} name="days" />
                </Field>
              )}
              {span === "until" && (
                <Field label={t("recipe.untilDate")}>
                  <input class="input" type="datetime-local" value={until} onInput={(e) => setUntil((e.target as HTMLInputElement).value)} name="until" />
                </Field>
              )}
            </div>
            <div class="row">
              <Button variant="primary" onClick={apply} disabled={!recipe || daysBad} data-testid="apply-recipe">
                {t("recipe.apply")}
              </Button>
              {current && (
                <Button variant="ghost" onClick={clear}>
                  {t("recipe.clear")}
                </Button>
              )}
            </div>
            <p class="muted small">{t("recipe.applyNote")}</p>
          </>
        ) : (
          <p class="muted">{t("recipe.noRecipes")}</p>
        )}
      </div>
    </Card>
  );
}

type Draft = { id?: string; name: string; note: string; ecTarget: number | null; phLow: number | null; phHigh: number | null; steps: { canister: string; mlPerL: number | null; fixed: boolean }[] };

const draftOf = (r: Recipe): Draft => ({
  id: r.id,
  name: r.name,
  note: r.note,
  ecTarget: r.ecTarget,
  phLow: r.ph?.[0] ?? null,
  phHigh: r.ph?.[1] ?? null,
  steps: r.steps.map((s) => ({ ...s })),
});

function Editor(p: { draft: Draft; onClose: () => void }) {
  const [d, setD] = useState<Draft>(p.draft);
  const nutrients = canisters.value.filter((b) => b.role === "nutrient");
  const set = (patch: Partial<Draft>) => setD({ ...d, ...patch });
  const setStep = (i: number, patch: Partial<Draft["steps"][number]>) => set({ steps: d.steps.map((s, j) => (j === i ? { ...s, ...patch } : s)) });
  const save = async () => {
    const ph = d.phLow != null && d.phHigh != null ? [d.phLow, d.phHigh] : null;
    await post("/recipes", { id: d.id, name: d.name.trim(), note: d.note, ecTarget: d.ecTarget, ph, steps: d.steps.filter((s) => s.canister) });
    await refreshConfig();
    toast(t("recipe.saved", { name: d.name.trim() }));
    p.onClose();
  };
  const pairOf = (id: string) => canisters.value.find((b) => b.id === id)?.pair ?? "";
  return (
    <Modal
      wide
      title={d.id ? t("recipe.edit") : t("recipe.new")}
      onClose={p.onClose}
      footer={
        <>
          <button class="btn" onClick={p.onClose}>
            {t("common.cancel")}
          </button>
          <Button variant="primary" onClick={save} disabled={!d.name.trim()} data-testid="save-recipe">
            {t("common.save")}
          </Button>
        </>
      }
    >
      <div class="form-grid">
        <Field label={t("common.name")}>
          <input class="input" value={d.name} maxLength={60} onInput={(e) => set({ name: (e.target as HTMLInputElement).value })} name="name" />
        </Field>
        <Field label={t("recipe.ecTarget")} hint={t("recipe.ecTargetHint")}>
          <NumberInput value={d.ecTarget} onValue={(v) => set({ ecTarget: v })} unit="mS/cm" name="ecTarget" />
        </Field>
        <Field label={t("recipe.phLow")}>
          <NumberInput value={d.phLow} onValue={(v) => set({ phLow: v })} name="phLow" />
        </Field>
        <Field label={t("recipe.phHigh")} hint={d.phLow != null && d.phHigh != null ? t("recipe.phTarget", { ph: num(phMiddle({ ph: [d.phLow, d.phHigh] }), 2) }) : undefined}>
          <NumberInput value={d.phHigh} onValue={(v) => set({ phHigh: v })} name="phHigh" />
        </Field>
      </div>
      <Field label={t("recipe.note")}>
        <textarea class="input" rows={2} maxLength={500} value={d.note} onInput={(e) => set({ note: (e.target as HTMLTextAreaElement).value })} name="note" />
      </Field>
      <div class="stack-sm">
        <div class="section-title">{t("recipe.parts")}</div>
        {!nutrients.length && <Banner tone="warn">{t("recipe.noCanisters")}</Banner>}
        <div class="steps recipe-steps">
          {d.steps.map((s, i) => (
            <div class="step">
              <span class="n">{i + 1}</span>
              <div class="form-grid" style="grid-template-columns:repeat(auto-fit,minmax(150px,1fr))">
                <select class="select input" aria-label={t("term.canister")} value={s.canister} onChange={(e) => setStep(i, { canister: (e.target as HTMLSelectElement).value })}>
                  <option value="">–</option>
                  {nutrients.map((b) => (
                    <option value={b.id}>{b.pair ? `${b.name} (${b.pair})` : b.name}</option>
                  ))}
                </select>
                <NumberInput value={s.mlPerL} onValue={(v) => setStep(i, { mlPerL: v })} unit="ml/L" ariaLabel={t("recipe.mlPerL")} />
              </div>
              <div class="row">
                {!pairOf(s.canister) && (
                  <label class="row small" style="gap:6px">
                    <Toggle checked={s.fixed} onChange={(v) => setStep(i, { fixed: v })} label={t("term.fixed")} />
                    <Term topic="fixed" />
                  </label>
                )}
                <button class="btn ghost sm" aria-label={t("common.delete")} onClick={() => set({ steps: d.steps.filter((_, j) => j !== i) })}>
                  <Trash2 size={15} />
                </button>
              </div>
            </div>
          ))}
        </div>
        <div>
          <button class="btn sm" onClick={() => set({ steps: [...d.steps, { canister: "", mlPerL: null, fixed: false }] })} disabled={!nutrients.length}>
            <Plus size={15} /> {t("recipe.addPart")}
          </button>
        </div>
        <p class="muted small">{t("recipe.partsNote")}</p>
      </div>
    </Modal>
  );
}

function RecipeList(p: { onEdit: (d: Draft) => void }) {
  const list = recipes.value;
  const active = config.value!.active.recipe;
  const name = (id: string) => canisters.value.find((b) => b.id === id)?.name ?? id;
  const remove = async (r: Recipe) => {
    if (!confirm(t("recipe.deleteAsk", { name: r.name }))) return;
    await del(`/recipes/${encodeURIComponent(r.id)}`);
    await refreshConfig();
    toast(t("recipe.deleted"), "info");
  };
  return (
    <Card
      title={t("recipe.recipes")}
      icon={<FlaskConical size={18} />}
      actions={
        <button class="btn sm" onClick={() => p.onEdit({ name: "", note: "", ecTarget: null, phLow: 5.8, phHigh: 6.2, steps: [] })} data-testid="new-recipe">
          <Plus size={15} /> {t("recipe.new")}
        </button>
      }
    >
      {list.length ? (
        <div class="list">
          {list.map((r) => (
            <div class="item wrap-row" data-testid={`recipe-${r.id}`}>
              <div class="grow">
                <div class="title">
                  {r.name} {r.id === active && <span class="pill ok">{t("recipe.inForceTag")}</span>}
                </div>
                <div class="muted small">
                  {t("recipe.summary", { ec: num(r.ecTarget, 2), lo: num(r.ph?.[0] ?? null, 2), hi: num(r.ph?.[1] ?? null, 2) })}
                  {" · "}
                  {r.steps.map((s) => `${name(s.canister)} ${num(s.mlPerL, 2)}${s.fixed ? "*" : ""}`).join(", ")}
                </div>
              </div>
              <div class="row-actions">
                <button class="btn sm" onClick={() => p.onEdit(draftOf(r))} aria-label={`${t("common.edit")}: ${r.name}`}>
                  <Pencil size={15} />
                </button>
                <Button size="sm" variant="danger-soft" onClick={() => remove(r)} disabled={r.id === active} aria-label={`${t("common.delete")}: ${r.name}`}>
                  <Trash2 size={15} />
                </Button>
              </div>
            </div>
          ))}
        </div>
      ) : (
        <Empty title={t("recipe.noRecipes")} text={t("recipe.noRecipesText")} />
      )}
    </Card>
  );
}

function Templates(p: { onUse: (d: Draft) => void }) {
  const [list, setList] = useState<RecipeTemplate[]>([]);
  useEffect(() => {
    get<{ recipes: RecipeTemplate[] }>("/templates").then((r) => setList(r.recipes)).catch(() => {});
  }, []);
  const de = lang.value === "de";
  // A part goes to the canister of the same name, else to the canister of its pair in the same place
  const match = (step: RecipeTemplate["steps"][number]) => {
    const names = [step.name, step.nameDe].filter(Boolean).map((n) => n!.toLowerCase());
    return canisters.value.find((b) => b.role === "nutrient" && names.includes(b.name.toLowerCase()))?.id ?? "";
  };
  const use = (tp: RecipeTemplate) =>
    p.onUse({
      name: (de ? tp.nameDe : tp.name) ?? tp.name,
      note: (de ? tp.noteDe : tp.note) ?? "",
      ecTarget: tp.ecTarget ?? null,
      phLow: tp.ph?.[0] ?? null,
      phHigh: tp.ph?.[1] ?? null,
      steps: tp.steps.map((s) => ({ canister: match(s), mlPerL: s.mlPerL, fixed: false })),
    });
  if (!list.length) return null;
  return (
    <Card title={t("recipe.templates")}>
      <div class="stack-sm">
        <p class="muted small">{t("recipe.templatesNote")}</p>
        <div class="tpl-grid">
          {list.map((tp) => (
            <div class="tpl" data-testid={`template-${tp.id}`}>
              <strong>{(de ? tp.nameDe : tp.name) ?? tp.name}</strong>
              <div class="tpl-rows">
                {tp.steps.map((s) => (
                  <div>
                    <span>{(de ? s.nameDe : s.name) ?? s.name}</span>
                    <span>{num(s.mlPerL, 1)} ml/L</span>
                  </div>
                ))}
                <div>
                  <span>EC</span>
                  <span>{num(tp.ecTarget ?? null, 1)} mS/cm</span>
                </div>
              </div>
              <span class="muted small">{(de ? tp.noteDe : tp.note) ?? ""}</span>
              <div>
                <button class="btn sm" onClick={() => use(tp)}>
                  {t("recipe.useTemplate")}
                </button>
              </div>
            </div>
          ))}
        </div>
      </div>
    </Card>
  );
}

export function RecipePage() {
  const [draft, setDraft] = useState<Draft | null>(null);
  return (
    <div class="stack">
      <ActiveCard key={config.value!.revision} />
      <RecipeList onEdit={setDraft} />
      <Templates onUse={setDraft} />
      {draft && <Editor draft={draft} onClose={() => setDraft(null)} />}
    </div>
  );
}
