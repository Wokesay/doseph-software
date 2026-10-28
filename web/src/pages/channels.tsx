// SPDX-License-Identifier: AGPL-3.0-or-later
// Channels & probes: which canister sits on which channel, calibrating and
// priming the channels, calibrating the pH and EC probes.
import { useEffect, useState } from "preact/hooks";
import { Beaker, Gauge, Pencil, Plus, RefreshCw, Ruler, Syringe, Trash2 } from "lucide-preact";
import { del, post, type Canister, type Stock } from "../api";
import { dateTime, num } from "../format";
import { msg, t } from "../i18n";
import { canisters, config, info, refreshConfig, refreshState, state, toast } from "../store";
import { Banner, Button, Card, Empty, Field, Modal, NumberInput, Pill, Seg, Term } from "../ui";

const COLORS = ["#c0392b", "#2e86c1", "#d4ac0d", "#239b56", "#8e44ad", "#d35400", "#566573", "#17a589"];

function CanisterEditor(p: { canister: Partial<Canister>; onClose: () => void }) {
  const [b, setB] = useState<Partial<Canister>>({ role: "nutrient", channel: 0, pair: "", color: COLORS[canisters.value.length % COLORS.length], capacityMl: null, ...p.canister });
  // A new canister: how much is in it now; empty leaves the level unknown (SD-119)
  const [stockMl, setStockMl] = useState<number | null>(null);
  const channels = info.value?.channelCount ?? 0;
  const used = (n: number) => canisters.value.find((x) => x.channel === n && x.id !== b.id);
  // One pH− canister at most (SD-108)
  const otherPhDown = b.role === "ph_down" ? canisters.value.find((x) => x.role === "ph_down" && x.id !== b.id) : undefined;
  const save = async () => {
    await post("/canisters", { ...b, name: (b.name ?? "").trim(), pair: b.role === "nutrient" ? (b.pair ?? "").trim() : "", ...(b.id ? {} : { stockMl }) });
    await refreshConfig();
    await refreshState();
    toast(t("channels.canisterSaved"));
    p.onClose();
  };
  return (
    <Modal
      title={b.id ? t("channels.editCanister") : t("channels.newCanister")}
      onClose={p.onClose}
      footer={
        <>
          <button class="btn" onClick={p.onClose}>
            {t("common.cancel")}
          </button>
          <Button variant="primary" onClick={save} disabled={!(b.name ?? "").trim() || !!otherPhDown} data-testid="save-canister">
            {t("common.save")}
          </Button>
        </>
      }
    >
      <div class="form-grid">
        <Field label={t("channels.content")}>
          <input class="input" value={b.name ?? ""} maxLength={40} onInput={(e) => setB({ ...b, name: (e.target as HTMLInputElement).value })} name="name" />
        </Field>
        <Field label={t("channels.role")} group>
          <Seg<Canister["role"]>
            value={b.role ?? "nutrient"}
            onChange={(role) => setB({ ...b, role })}
            options={[
              ["nutrient", t("channels.role.nutrient")],
              ["ph_down", "pH−"],
            ]}
          />
          {otherPhDown && <p class="small warn-text">{t("channels.phDownTaken", { name: otherPhDown.name })}</p>}
        </Field>
        <Field label={t("channels.channel")}>
          <select class="select input" value={String(b.channel ?? 0)} onChange={(e) => setB({ ...b, channel: Number((e.target as HTMLSelectElement).value) })} name="channel">
            <option value="0">{t("channels.noChannel")}</option>
            {Array.from({ length: channels }, (_, i) => i + 1).map((n) => (
              <option value={String(n)} disabled={!!used(n)}>
                {used(n) ? t("channels.channelTaken", { channel: n, name: used(n)!.name }) : t("channels.channelN", { channel: n })}
              </option>
            ))}
          </select>
        </Field>
        <Field label={t("channels.capacity")} hint={t("channels.capacityHint")}>
          <NumberInput value={b.capacityMl} onValue={(v) => setB({ ...b, capacityMl: v })} unit="ml" min={10} max={25000} step={50} name="capacity" />
        </Field>
        {!b.id && (
          <Field label={t("channels.levelNow")} hint={t("channels.levelNowHint")}>
            <NumberInput value={stockMl} onValue={setStockMl} unit="ml" min={0} max={25000} step={50} name="stock" />
          </Field>
        )}
        {b.role === "nutrient" && (
          <Field label={<Term topic="pair" />} hint={t("channels.pairHint")}>
            <input class="input" value={b.pair ?? ""} maxLength={8} onInput={(e) => setB({ ...b, pair: (e.target as HTMLInputElement).value })} name="pair" />
          </Field>
        )}
      </div>
      <Field label={t("channels.color")} group>
        <div class="row" role="radiogroup" aria-label={t("channels.color")}>
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
      <p class="muted small">{t("channels.colorNote")}</p>
    </Modal>
  );
}

/** Entering what is in a canister now, after measuring it or a partial refill. */
function LevelDialog(p: { canister: Canister; onClose: () => void }) {
  const [ml, setMl] = useState<number | null>(null);
  return (
    <Modal
      title={t("channels.levelOf", { name: p.canister.name })}
      onClose={p.onClose}
      footer={
        <>
          <button class="btn" onClick={p.onClose}>
            {t("common.cancel")}
          </button>
          <Button
            variant="primary"
            disabled={ml === null || ml < 0}
            onClick={async () => {
              await post(`/canisters/${encodeURIComponent(p.canister.id)}/stock`, { ml });
              await refreshState();
              toast(t("channels.levelSaved"));
              p.onClose();
            }}
          >
            {t("common.save")}
          </Button>
        </>
      }
    >
      <Field label={t("channels.levelNow")}>
        <NumberInput value={ml} onValue={setMl} unit="ml" min={0} max={25000} step={10} name="level" />
      </Field>
    </Modal>
  );
}

/** The level as counted, with a bar where the size is known (SD-119). */
function Level(p: { canister: Canister; stock: Stock | undefined }) {
  const s = p.stock;
  if (!s || s.status === "unknown" || s.ml === null) return <span class="muted small">{t("channels.levelUnknown")}</span>;
  const tone = s.status === "empty" ? "bad" : s.status === "low" ? "warn" : "ok";
  const cap = p.canister.capacityMl;
  const text = cap ? t("channels.levelOfSize", { ml: num(Math.max(0, s.ml), 0), size: num(cap, 0) }) : t("channels.levelMl", { ml: num(Math.max(0, s.ml), 0) });
  return (
    <span class="row" style="gap:8px" data-testid={`level-${p.canister.id}`}>
      {cap ? (
        <span class="bar" style="width:80px" aria-hidden="true">
          <span style={`width:${Math.min(100, Math.max(0, (s.ml / cap) * 100))}%;background:var(--${tone})`} />
        </span>
      ) : null}
      <span class="small">{text}</span>
      {s.status !== "ok" && <Pill tone={tone}>{t(s.status === "empty" ? "channels.levelEmpty" : "channels.levelLow")}</Pill>}
    </span>
  );
}

function CanistersCard(p: { onEdit: (b: Partial<Canister>) => void }) {
  const list = canisters.value;
  const stock = state.value?.stock ?? {};
  const [levelOf, setLevelOf] = useState<Canister | null>(null);
  const refilled = async (b: Canister) => {
    await post(`/canisters/${encodeURIComponent(b.id)}/stock`, { full: true });
    await refreshState();
    toast(t("channels.refilledDone", { name: b.name }));
  };
  const remove = async (b: Canister) => {
    if (!confirm(t("channels.deleteAsk", { name: b.name }))) return;
    await del(`/canisters/${encodeURIComponent(b.id)}`);
    await refreshConfig();
    toast(t("channels.canisterDeleted"), "info");
  };
  return (
    <Card
      title={t("channels.canisters")}
      icon={<Beaker size={18} />}
      actions={
        <button class="btn sm" onClick={() => p.onEdit({})} data-testid="new-canister">
          <Plus size={15} /> {t("channels.newCanister")}
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
                  {b.role === "ph_down" ? "pH−" : t("channels.role.nutrient")}
                  {b.pair && ` · ${t("channels.pairName", { pair: b.pair })}`}
                  {" · "}
                  {b.channel ? t("channels.channelN", { channel: b.channel }) : t("channels.noChannel")}
                </div>
                <Level canister={b} stock={stock[b.id]} />
              </div>
              <div class="row-actions">
                {b.capacityMl ? (
                  <Button size="sm" onClick={() => refilled(b)} title={t("channels.refilledHint")} data-testid={`refilled-${b.id}`}>
                    <RefreshCw size={15} /> {t("channels.refilled")}
                  </Button>
                ) : null}
                <button class="btn sm" onClick={() => setLevelOf(b)} aria-label={`${t("channels.enterLevel")}: ${b.name}`} title={t("channels.enterLevel")}>
                  <Ruler size={15} />
                </button>
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
        <Empty title={t("channels.noCanisters")} text={t("channels.noCanistersText")} />
      )}
      {list.length > 0 && <p class="muted small">{t("channels.levelNote")}</p>}
      {levelOf && <LevelDialog canister={levelOf} onClose={() => setLevelOf(null)} />}
    </Card>
  );
}

/** Calibrating or priming one channel: start, the run, the amount in the cup, the result. */
function ChannelJobDialog(p: { channel: number; type: "calibration" | "prime"; onClose: () => void }) {
  const st = state.value!;
  // Only the job this dialog started: a finished one from before is not this one's
  const [started, setStarted] = useState<string | null>(null);
  const job = st.channelJob && st.channelJob.id === started ? st.channelJob : null;
  const [seconds, setSeconds] = useState<number | null>(p.type === "calibration" ? 30 : 5);
  const [ml, setMl] = useState<number | null>(null);
  const max = config.value!.limits.maxRunS;
  const start = async () => {
    const r = await post<{ id: string }>(`/channels/${p.channel}/${p.type === "calibration" ? "calibrate" : "prime"}`, { seconds });
    setStarted(r.id);
    await refreshState();
  };
  const cancel = async () => {
    if (job) await post("/channels/cancel").catch(() => {});
    await refreshState();
    p.onClose();
  };
  const result = async () => {
    await post("/channels/result", { ml });
    await Promise.all([refreshState(), refreshConfig()]);
  };
  const bad = seconds === null || seconds < 1 || seconds > max;
  return (
    <Modal
      title={t(p.type === "calibration" ? "channels.calibrateN" : "channels.primeN", { channel: p.channel })}
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
          <p>{t(p.type === "calibration" ? "channels.calibrateIntro" : "channels.primeIntro")}</p>
          <Field label={t("channels.seconds")} error={bad ? t("channels.secondsRange", { max }) : null}>
            <NumberInput value={seconds} onValue={setSeconds} unit="s" name="seconds" />
          </Field>
          <div>
            <Button variant="primary" onClick={start} disabled={bad} data-testid="job-start">
              {t("channels.start")}
            </Button>
          </div>
        </div>
      )}
      {job?.state === "running" && (
        <div class="stack-sm" data-testid="job-running">
          <p>{t("channels.running", { s: num(job.seconds, 0) })}</p>
          <div class="progress">
            <span style="width:100%;opacity:0.5" />
          </div>
        </div>
      )}
      {job?.state === "waiting_result" && (
        <div class="stack">
          <p>{msg(job.message)}</p>
          <Field label={t("channels.mlInCup")}>
            <NumberInput value={ml} onValue={setMl} unit="ml" name="ml" />
          </Field>
          <div>
            <Button variant="primary" onClick={result} disabled={ml === null || ml <= 0} data-testid="job-result">
              {t("common.save")}
            </Button>
          </div>
        </div>
      )}
      {job?.state === "done" && (
        <Banner tone="ok">
          <span data-testid="job-done">{msg(job.message)}</span>
        </Banner>
      )}
      {job?.state === "failed" && <Banner tone="bad">{msg(job.message)}</Banner>}
    </Modal>
  );
}

function ChannelsCard() {
  const st = state.value!;
  const [dialog, setDialog] = useState<{ channel: number; type: "calibration" | "prime" } | null>(null);
  const name = (id: string | null) => canisters.value.find((b) => b.id === id)?.name;
  return (
    <Card title={t("channels.channels")} icon={<Syringe size={18} />}>
      <div class="list">
        {st.channels.map((p) => (
          <div class="item wrap-row" data-testid={`channel-${p.channel}`}>
            <div class="grow">
              <div class="title">
                {t("channels.channelN", { channel: p.channel })} · {name(p.canister) ?? <span class="muted">{t("channels.empty")}</span>}
              </div>
              <div class="small" data-testid={`head-${p.channel}`}>
                {!p.head ? (
                  <Pill tone={p.canister ? "bad" : "neutral"}>{t("channels.noHead")}</Pill>
                ) : p.flowMlPerMin != null ? (
                  <span class="muted">
                    {t("channels.head", { head: p.head })} · {t("channels.flow", { flow: num(p.flowMlPerMin, 1), date: p.at ? dateTime(p.at) : "–" })}
                  </span>
                ) : (
                  <>
                    <span class="muted">{t("channels.head", { head: p.head })}</span> <Pill tone={p.canister ? "warn" : "neutral"}>{t("channels.notCalibrated")}</Pill>
                  </>
                )}
              </div>
            </div>
            <div class="row-actions">
              <button class="btn sm" onClick={() => setDialog({ channel: p.channel, type: "prime" })} disabled={st.stopped || !p.head}>
                {t("term.prime")}
              </button>
              <button
                class="btn sm"
                onClick={() => setDialog({ channel: p.channel, type: "calibration" })}
                disabled={st.stopped || !p.head}
                data-testid={`calibrate-${p.channel}`}
              >
                {t("term.calibratePump")}
              </button>
            </div>
          </div>
        ))}
      </div>
      <p class="muted small" style="margin-top:8px">
        <Term topic="calibratePump" />
      </p>
      <p class="muted small">{t("channels.headNote")}</p>
      {dialog && <ChannelJobDialog {...dialog} onClose={() => setDialog(null)} />}
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
    toast(t("channels.probeSaved"));
    p.onClose();
  };
  const raw = p.kind === "ph" ? s?.rawPh : s?.rawEc;
  const ready = p.kind === "ph" ? points.length === 2 : points.length === 1;
  return (
    <Modal
      title={t(p.kind === "ph" ? "channels.calibratePh" : "channels.calibrateEc")}
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
          <p>{t(p.kind === "ph" ? "channels.phIntro" : "channels.ecIntro")}</p>
          <div>
            <Button variant="primary" onClick={() => call({ action: "start" })} data-testid="probe-start">
              {t("channels.start")}
            </Button>
          </div>
        </div>
      ) : (
        <div class="stack">
          <div class="kv">
            <span>{t("channels.raw")}</span>
            <span data-testid="probe-raw">{num(raw ?? null, p.kind === "ph" ? 2 : 3)}</span>
            {p.kind === "ec" && (
              <>
                <span>{t("status.temp")}</span>
                <span>{s.temp != null ? `${num(s.temp, 1)} °C` : "–"}</span>
              </>
            )}
            {points.map(([r, v], i) => (
              <>
                <span>{t("channels.point", { n: i + 1 })}</span>
                <span>
                  {num(v, p.kind === "ph" ? 2 : 3)} ← {num(r, p.kind === "ph" ? 2 : 3)}
                </span>
              </>
            ))}
          </div>
          <p class="muted small">{t(p.kind === "ph" ? (points.length ? "channels.phNext" : "channels.phFirst") : "channels.ecStep")}</p>
          {!ready && (
            <div class="row" style="align-items:flex-end">
              <Field label={t(p.kind === "ph" ? "channels.buffer" : "channels.reference")}>
                <NumberInput value={ref} onValue={setRef} unit={p.kind === "ec" ? "mS/cm" : undefined} name="reference" />
              </Field>
              <Button onClick={() => call({ action: "point", reference: ref })} disabled={ref === null} data-testid="probe-point">
                {t("channels.takePoint")}
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
          <div class="small">{at ? <span class="muted">{t("channels.calibratedAt", { date: dateTime(at) })}</span> : <Pill tone="warn">{t("widgets.quality.uncalibrated")}</Pill>}</div>
        </div>
        <button class="btn sm" onClick={() => setKind(k)} data-testid={`calibrate-${k}`}>
          {t("channels.calibrate")}
        </button>
      </div>
    );
  };
  return (
    <Card title={<Term topic="calibrateProbe" />} icon={<Gauge size={18} />}>
      <div class="list">
        {row("ph", t("channels.phProbe"))}
        {row("ec", t("channels.ecProbe"))}
      </div>
      <p class="muted small" style="margin-top:8px">
        {t("channels.probeNote")}
      </p>
      {kind && <ProbeDialog kind={kind} onClose={() => setKind(null)} />}
    </Card>
  );
}

export function ChannelsPage() {
  const [edit, setEdit] = useState<Partial<Canister> | null>(null);
  return (
    <div class="stack">
      <CanistersCard onEdit={setEdit} />
      <ChannelsCard />
      <ProbesCard />
      {edit && <CanisterEditor canister={edit} onClose={() => setEdit(null)} />}
    </div>
  );
}
