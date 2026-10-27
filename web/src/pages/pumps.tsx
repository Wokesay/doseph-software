// SPDX-License-Identifier: AGPL-3.0-or-later
// Pumps & probes: which canister sits on which pump, calibrating and priming
// the dosing pumps, calibrating the pH and EC probes.
import { useEffect, useState } from "preact/hooks";
import { Beaker, Gauge, Pencil, Plus, Syringe, Trash2 } from "lucide-preact";
import { del, post, type Canister } from "../api";
import { dateTime, num } from "../format";
import { msg, t } from "../i18n";
import { canisters, config, info, refreshConfig, refreshState, state, toast } from "../store";
import { Banner, Button, Card, Empty, Field, Modal, NumberInput, Pill, Seg, Term } from "../ui";

const COLORS = ["#c0392b", "#2e86c1", "#d4ac0d", "#239b56", "#8e44ad", "#d35400", "#566573", "#17a589"];

function CanisterEditor(p: { canister: Partial<Canister>; onClose: () => void }) {
  const [b, setB] = useState<Partial<Canister>>({ role: "nutrient", pump: 0, pair: "", color: COLORS[canisters.value.length % COLORS.length], ...p.canister });
  const pumps = info.value?.pumpCount ?? 0;
  const used = (n: number) => canisters.value.find((x) => x.pump === n && x.id !== b.id);
  const save = async () => {
    await post("/canisters", { ...b, name: (b.name ?? "").trim(), pair: b.role === "nutrient" ? (b.pair ?? "").trim() : "" });
    await refreshConfig();
    toast(t("pumps.canisterSaved"));
    p.onClose();
  };
  return (
    <Modal
      title={b.id ? t("pumps.editCanister") : t("pumps.newCanister")}
      onClose={p.onClose}
      footer={
        <>
          <button class="btn" onClick={p.onClose}>
            {t("common.cancel")}
          </button>
          <Button variant="primary" onClick={save} disabled={!(b.name ?? "").trim()} data-testid="save-canister">
            {t("common.save")}
          </Button>
        </>
      }
    >
      <div class="form-grid">
        <Field label={t("pumps.content")}>
          <input class="input" value={b.name ?? ""} maxLength={40} onInput={(e) => setB({ ...b, name: (e.target as HTMLInputElement).value })} name="name" />
        </Field>
        <Field label={t("pumps.role")} group>
          <Seg<Canister["role"]>
            value={b.role ?? "nutrient"}
            onChange={(role) => setB({ ...b, role })}
            options={[
              ["nutrient", t("pumps.role.nutrient")],
              ["ph_down", "pH−"],
            ]}
          />
        </Field>
        <Field label={t("pumps.pump")}>
          <select class="select input" value={String(b.pump ?? 0)} onChange={(e) => setB({ ...b, pump: Number((e.target as HTMLSelectElement).value) })} name="pump">
            <option value="0">{t("pumps.noPump")}</option>
            {Array.from({ length: pumps }, (_, i) => i + 1).map((n) => (
              <option value={String(n)} disabled={!!used(n)}>
                {used(n) ? t("pumps.pumpTaken", { pump: n, name: used(n)!.name }) : t("pumps.pumpN", { pump: n })}
              </option>
            ))}
          </select>
        </Field>
        {b.role === "nutrient" && (
          <Field label={<Term topic="pair" />} hint={t("pumps.pairHint")}>
            <input class="input" value={b.pair ?? ""} maxLength={8} onInput={(e) => setB({ ...b, pair: (e.target as HTMLInputElement).value })} name="pair" />
          </Field>
        )}
      </div>
      <Field label={t("pumps.color")} group>
        <div class="row" role="radiogroup" aria-label={t("pumps.color")}>
          {COLORS.map((c) => (
            <button
              type="button"
              role="radio"
              aria-checked={b.color === c}
              aria-label={c}
              onClick={() => setB({ ...b, color: c })}
              style={`width:30px;height:30px;border-radius:8px;background:${c};border:${b.color === c ? "3px solid var(--text)" : "1px solid var(--border)"};cursor:pointer`}
            />
          ))}
        </div>
      </Field>
      <p class="muted small">{t("pumps.colorNote")}</p>
    </Modal>
  );
}

function CanistersCard(p: { onEdit: (b: Partial<Canister>) => void }) {
  const list = canisters.value;
  const remove = async (b: Canister) => {
    if (!confirm(t("pumps.deleteAsk", { name: b.name }))) return;
    await del(`/canisters/${encodeURIComponent(b.id)}`);
    await refreshConfig();
    toast(t("pumps.canisterDeleted"), "info");
  };
  return (
    <Card
      title={t("pumps.canisters")}
      icon={<Beaker size={18} />}
      actions={
        <button class="btn sm" onClick={() => p.onEdit({})} data-testid="new-canister">
          <Plus size={15} /> {t("pumps.newCanister")}
        </button>
      }
    >
      {list.length ? (
        <div class="list">
          {list.map((b) => (
            <div class="item wrap-row" data-testid={`canister-${b.id}`}>
              <span class="swatch" style={`background:${b.color || "var(--faint)"}`} />
              <div class="grow">
                <div class="title">{b.name}</div>
                <div class="muted small">
                  {b.role === "ph_down" ? "pH−" : t("pumps.role.nutrient")}
                  {b.pair && ` · ${t("pumps.pairName", { pair: b.pair })}`}
                  {" · "}
                  {b.pump ? t("pumps.pumpN", { pump: b.pump }) : t("pumps.noPump")}
                </div>
              </div>
              <div class="row-actions">
                <button class="btn sm" onClick={() => p.onEdit(b)} aria-label={`${t("common.edit")}: ${b.name}`}>
                  <Pencil size={15} />
                </button>
                <Button size="sm" variant="danger-soft" onClick={() => remove(b)} aria-label={`${t("common.delete")}: ${b.name}`}>
                  <Trash2 size={15} />
                </Button>
              </div>
            </div>
          ))}
        </div>
      ) : (
        <Empty title={t("pumps.noCanisters")} text={t("pumps.noCanistersText")} />
      )}
    </Card>
  );
}

/** Calibrating or priming one pump: start, the run, the amount in the cup, the result. */
function PumpJobDialog(p: { pump: number; type: "calibration" | "prime"; onClose: () => void }) {
  const st = state.value!;
  // Only the job this dialog started: a finished one from before is not this one's
  const [started, setStarted] = useState<string | null>(null);
  const job = st.pumpJob && st.pumpJob.id === started ? st.pumpJob : null;
  const [seconds, setSeconds] = useState<number | null>(p.type === "calibration" ? 30 : 5);
  const [ml, setMl] = useState<number | null>(null);
  const max = config.value!.limits.maxRunS;
  const start = async () => {
    const r = await post<{ id: string }>(`/pumps/${p.pump}/${p.type === "calibration" ? "calibrate" : "prime"}`, { seconds });
    setStarted(r.id);
    await refreshState();
  };
  const cancel = async () => {
    if (job) await post("/pumps/cancel").catch(() => {});
    await refreshState();
    p.onClose();
  };
  const result = async () => {
    await post("/pumps/result", { ml });
    await Promise.all([refreshState(), refreshConfig()]);
  };
  const bad = seconds === null || seconds < 1 || seconds > max;
  return (
    <Modal
      title={t(p.type === "calibration" ? "pumps.calibrateN" : "pumps.primeN", { pump: p.pump })}
      onClose={cancel}
      footer={
        job?.state === "done" ? (
          <button class="btn primary" onClick={cancel}>
            {t("common.close")}
          </button>
        ) : (
          <button class="btn" onClick={cancel}>
            {t("common.cancel")}
          </button>
        )
      }
    >
      {!job && (
        <div class="stack">
          <p>{t(p.type === "calibration" ? "pumps.calibrateIntro" : "pumps.primeIntro")}</p>
          <Field label={t("pumps.seconds")} error={bad ? t("pumps.secondsRange", { max }) : null}>
            <NumberInput value={seconds} onValue={setSeconds} unit="s" name="seconds" />
          </Field>
          <div>
            <Button variant="primary" onClick={start} disabled={bad} data-testid="pump-start">
              {t("pumps.start")}
            </Button>
          </div>
        </div>
      )}
      {job?.state === "running" && (
        <div class="stack-sm" data-testid="pump-running">
          <p>{t("pumps.running", { s: num(job.seconds, 0) })}</p>
          <div class="progress">
            <span style="width:100%;opacity:0.5" />
          </div>
        </div>
      )}
      {job?.state === "waiting_result" && (
        <div class="stack">
          <p>{msg(job.message)}</p>
          <Field label={t("pumps.mlInCup")}>
            <NumberInput value={ml} onValue={setMl} unit="ml" name="ml" />
          </Field>
          <div>
            <Button variant="primary" onClick={result} disabled={ml === null || ml <= 0} data-testid="pump-result">
              {t("common.save")}
            </Button>
          </div>
        </div>
      )}
      {job?.state === "done" && (
        <Banner tone="ok">
          <span data-testid="pump-done">{msg(job.message)}</span>
        </Banner>
      )}
      {job?.state === "failed" && <Banner tone="bad">{msg(job.message)}</Banner>}
    </Modal>
  );
}

function PumpsCard() {
  const st = state.value!;
  const [dialog, setDialog] = useState<{ pump: number; type: "calibration" | "prime" } | null>(null);
  const name = (id: string | null) => canisters.value.find((b) => b.id === id)?.name;
  return (
    <Card title={t("pumps.pumps")} icon={<Syringe size={18} />}>
      <div class="list">
        {st.pumps.map((p) => (
          <div class="item wrap-row" data-testid={`pump-${p.pump}`}>
            <div class="grow">
              <div class="title">
                {t("pumps.pumpN", { pump: p.pump })} · {name(p.canister) ?? <span class="muted">{t("pumps.empty")}</span>}
              </div>
              <div class="small">
                {p.flowMlPerMin != null ? (
                  <span class="muted">{t("pumps.flow", { flow: num(p.flowMlPerMin, 1), date: p.at ? dateTime(p.at) : "–" })}</span>
                ) : (
                  <Pill tone={p.canister ? "warn" : "neutral"}>{t("pumps.notCalibrated")}</Pill>
                )}
              </div>
            </div>
            <div class="row-actions">
              <button class="btn sm" onClick={() => setDialog({ pump: p.pump, type: "prime" })} disabled={st.stopped}>
                {t("term.prime")}
              </button>
              <button class="btn sm" onClick={() => setDialog({ pump: p.pump, type: "calibration" })} disabled={st.stopped} data-testid={`calibrate-${p.pump}`}>
                {t("term.calibratePump")}
              </button>
            </div>
          </div>
        ))}
      </div>
      <p class="muted small" style="margin-top:8px">
        <Term topic="calibratePump" />
      </p>
      {dialog && <PumpJobDialog {...dialog} onClose={() => setDialog(null)} />}
    </Card>
  );
}

function ProbeDialog(p: { kind: "ph" | "ec"; onClose: () => void }) {
  const st = state.value!;
  const s = st.probe;
  const points = s?.points ?? [];
  const [ref, setRef] = useState<number | null>(p.kind === "ph" ? 7 : 1.413);
  useEffect(() => {
    // The next buffer once the first is measured
    if (p.kind === "ph" && points.length === 1 && ref === points[0][1]) setRef(points[0][1] >= 6 ? 4 : 7);
  }, [points.length]);
  const call = async (body: Record<string, unknown>) => {
    await post("/probe", { kind: p.kind, ...body });
    await refreshState();
  };
  const close = async () => {
    if (st.probe) await post("/probe", { action: "cancel" }).catch(() => {});
    await refreshState();
    p.onClose();
  };
  const save = async () => {
    await call({ action: "save" });
    await refreshConfig();
    toast(t("pumps.probeSaved"));
    p.onClose();
  };
  const raw = p.kind === "ph" ? s?.rawPh : s?.rawEc;
  const ready = p.kind === "ph" ? points.length === 2 : points.length === 1;
  return (
    <Modal
      title={t(p.kind === "ph" ? "pumps.calibratePh" : "pumps.calibrateEc")}
      onClose={close}
      footer={
        <>
          <button class="btn" onClick={close}>
            {t("common.cancel")}
          </button>
          {s && (
            <Button variant="primary" onClick={save} disabled={!ready} data-testid="probe-save">
              {t("common.save")}
            </Button>
          )}
        </>
      }
    >
      {!s ? (
        <div class="stack">
          <p>{t(p.kind === "ph" ? "pumps.phIntro" : "pumps.ecIntro")}</p>
          <div>
            <Button variant="primary" onClick={() => call({ action: "start" })} data-testid="probe-start">
              {t("pumps.start")}
            </Button>
          </div>
        </div>
      ) : (
        <div class="stack">
          <div class="kv">
            <span>{t("pumps.raw")}</span>
            <span data-testid="probe-raw">{num(raw ?? null, p.kind === "ph" ? 2 : 3)}</span>
            {p.kind === "ec" && (
              <>
                <span>{t("status.temp")}</span>
                <span>{s.temp != null ? `${num(s.temp, 1)} °C` : "–"}</span>
              </>
            )}
            {points.map(([r, v], i) => (
              <>
                <span>{t("pumps.point", { n: i + 1 })}</span>
                <span>
                  {num(v, p.kind === "ph" ? 2 : 3)} ← {num(r, p.kind === "ph" ? 2 : 3)}
                </span>
              </>
            ))}
          </div>
          <p class="muted small">{t(p.kind === "ph" ? (points.length ? "pumps.phNext" : "pumps.phFirst") : "pumps.ecStep")}</p>
          {!ready && (
            <div class="row" style="align-items:flex-end">
              <Field label={t(p.kind === "ph" ? "pumps.buffer" : "pumps.reference")}>
                <NumberInput value={ref} onValue={setRef} unit={p.kind === "ec" ? "mS/cm" : undefined} name="reference" />
              </Field>
              <Button onClick={() => call({ action: "point", reference: ref })} disabled={ref === null} data-testid="probe-point">
                {t("pumps.takePoint")}
              </Button>
            </div>
          )}
        </div>
      )}
    </Modal>
  );
}

function ProbesCard() {
  const cfg = config.value!;
  const st = state.value!;
  const [kind, setKind] = useState<"ph" | "ec" | null>(st.probe?.kind ?? null);
  const row = (k: "ph" | "ec", label: string) => {
    const at = cfg.probes[k]?.at;
    return (
      <div class="item wrap-row" data-testid={`probe-${k}`}>
        <div class="grow">
          <div class="title">{label}</div>
          <div class="small">{at ? <span class="muted">{t("pumps.calibratedAt", { date: dateTime(at) })}</span> : <Pill tone="warn">{t("widgets.quality.uncalibrated")}</Pill>}</div>
        </div>
        <button class="btn sm" onClick={() => setKind(k)} data-testid={`calibrate-${k}`}>
          {t("pumps.calibrate")}
        </button>
      </div>
    );
  };
  return (
    <Card title={<Term topic="calibrateProbe" />} icon={<Gauge size={18} />}>
      <div class="list">
        {row("ph", t("pumps.phProbe"))}
        {row("ec", t("pumps.ecProbe"))}
      </div>
      <p class="muted small" style="margin-top:8px">
        {t("pumps.probeNote")}
      </p>
      {kind && <ProbeDialog kind={kind} onClose={() => setKind(null)} />}
    </Card>
  );
}

export function PumpsPage() {
  const [edit, setEdit] = useState<Partial<Canister> | null>(null);
  return (
    <div class="stack">
      <CanistersCard onEdit={setEdit} />
      <PumpsCard />
      <ProbesCard />
      {edit && <CanisterEditor canister={edit} onClose={() => setEdit(null)} />}
    </div>
  );
}
