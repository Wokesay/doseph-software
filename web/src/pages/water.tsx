// SPDX-License-Identifier: AGPL-3.0-or-later
// Water path: the flow through the device and how it is proven, the
// measuring chamber with its probes, and the way back to the tank with the
// hardware release (SD-109, SD-110).
import { useEffect, useState } from "preact/hooks";
import { Beaker, RefreshCw, ShieldCheck } from "lucide-preact";
import { holds, post, type Reading } from "../api";
import { ago, dateTime, num } from "../format";
import { msg, t, type TextKey } from "../i18n";
import { config, info, refreshConfig, refreshState, state, toast } from "../store";
import { Banner, Button, Card, Field, Modal, NumberInput, Pill, Term, type Tone } from "../ui";

// A yes/no the device reports; null: it has no such sensor
function YesNo(p: { value: boolean | null; yes: TextKey; no: TextKey; yesTone?: Tone; noTone?: Tone; id: string }) {
  if (p.value === null) return <span data-testid={p.id}>–</span>;
  return (
    <span data-testid={p.id}>
      <Pill tone={p.value ? (p.yesTone ?? "ok") : (p.noTone ?? "neutral")}>{t(p.value ? p.yes : p.no)}</Pill>
    </span>
  );
}

// A lock that stops the water path, in the watchdog's words; it is released under Status. One that
// only reports holds nothing: a softer note (SD-133)
function LockNote(p: { id: string }) {
  const st = state.value!;
  if (!st.latches[p.id]) return null;
  const item = st.watchdog.items.find((i) => i.id === `latch.${p.id}`);
  return (
    <Banner tone={holds(st.latches[p.id]) ? "bad" : "warn"}>
      <span data-testid={`water-latch-${p.id}`}>{item ? msg(item.text) : p.id}</span> <a href="#/">{t(holds(st.latches[p.id]) ? "water.releaseOnStatus" : "water.seenOnStatus")}</a>
    </Banner>
  );
}

function FlowCard() {
  const st = state.value!;
  const s = st.cycle.settings;
  const flow = st.readings.flow;
  const method = info.value?.profile?.flow;
  return (
    <Card title={<Term topic="circulation" />} icon={<RefreshCw size={18} />}>
      <div class="stack">
        <LockNote id="flow.dry" />
        <p class="muted small" data-testid="flow-method">
          {method === "turbine" ? t("water.byTurbine") : method === "outlet_level" ? t("water.byOutlet") : t("tank.pumpText")}
        </p>
        <div class="kv" data-testid="flow-info">
          <span>{t("water.feedPump")}</span>
          {st.hardware.release.noFlow ? (
            <span data-testid="feed-pump">
              <Pill tone="bad">{t("water.cutNoFlow")}</Pill>
            </span>
          ) : (
            <YesNo id="feed-pump" value={st.feedPump} yes="water.running" no="water.standing" />
          )}
          {st.pump.amps != null && st.feedPump && (
            <>
              <span>{t("water.pumpA")}</span>
              <span data-testid="pump-amps">
                {st.pump.normalA != null
                  ? t("water.pumpANormal", { a: num(st.pump.amps, 2), n: num(st.pump.normalA, 2) })
                  : t("water.pumpALearning", { a: num(st.pump.amps, 2) })}
              </span>
            </>
          )}
          <span>{t("water.flowProven")}</span>
          <YesNo id="flow-proven" value={st.feedPump ? st.flowOk : null} yes="water.proven" no="water.notProven" noTone="warn" />
          <span>{t("tank.flowNow")}</span>
          <span>{flow.usable ? `${num(flow.value, 1)} L/min` : "–"}</span>
          <span>{t("tank.flowMeasured")}</span>
          <span>{s.flowLpm != null ? `${num(s.flowLpm, 1)} L/min` : t("tank.notYet")}</span>
          <span>
            <Term topic="turnover" />
          </span>
          <span data-testid="turnover">{s.turnoverMin != null ? t("common.minutes", { n: num(s.turnoverMin, 1) }) : "–"}</span>
        </div>
        <p class="muted small">{t("tank.pumpText")}</p>
        {s.flowLpm == null && <p class="muted small">{t("tank.flowUnknown")}</p>}
      </div>
    </Card>
  );
}

function ReadingValue(p: { r: Reading; decimals: number }) {
  if (p.r.quality !== "ok") return <Pill tone={p.r.quality === "uncalibrated" ? "warn" : "bad"} title={msg(p.r.reason)}>{msg(p.r.reason)}</Pill>;
  return (
    <span>
      {num(p.r.value, p.decimals)} {p.r.unit} <span class="muted small">{ago(p.r.ageS)}</span>
    </span>
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

// The measuring chamber: the probes sit under water behind a weir, what runs
// over it goes to the outlet; calibrating a probe stops the check (SD-109)
function ChamberCard() {
  const cfg = config.value!;
  const st = state.value!;
  const r = st.readings;
  const [kind, setKind] = useState<"ph" | "ec" | null>(st.probe?.kind ?? null);
  const probe = (k: "ph" | "ec", label: string) => {
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
    <Card title={t("water.chamber")} icon={<Beaker size={18} />}>
      <div class="stack">
        <p class="muted small">{t("water.chamberIntro")}</p>
        <div class="kv" data-testid="chamber">
          <span>pH</span>
          <ReadingValue r={r.ph} decimals={2} />
          <span>EC</span>
          <ReadingValue r={r.ec} decimals={2} />
          <span>{t("status.temp")}</span>
          <ReadingValue r={r.temp} decimals={1} />
          <span>{t("water.outlet")}</span>
          <YesNo id="outlet-wet" value={st.hardware.outletWet} yes="water.wet" no="water.dry" />
          <span>{t("water.highLevel")}</span>
          <YesNo id="high-level" value={st.hardware.release.highLevel} yes="water.highLevelYes" no="water.highLevelNo" yesTone="bad" />
        </div>
        <div class="section-title">
          <Term topic="calibrateProbe" />
        </div>
        <div class="list">
          {probe("ph", t("channels.phProbe"))}
          {probe("ec", t("channels.ecProbe"))}
        </div>
        <p class="muted small">{t("channels.probeNote")}</p>
      </div>
      {kind && <ProbeDialog kind={kind} onClose={() => setKind(null)} />}
    </Card>
  );
}

// Back to the tank: by gravity, or by a pump that is locked after running a
// minute without a break (SD-109); and the release that stops every pump
// below the software (SD-110)
function ReturnCard() {
  const st = state.value!;
  const hw = st.hardware;
  const by = info.value?.profile?.return;
  const rel = hw.release;
  const why = [rel.leak && t("water.leak"), rel.highLevel && t("water.highLevelYes"), rel.stopButton && t("water.stopButton")].filter(Boolean).join(", ");
  return (
    <Card title={t("water.return")} icon={<ShieldCheck size={18} />}>
      <div class="stack">
        <LockNote id="hw.return" />
        <p class="muted small" data-testid="return-by">
          {by === "pump" ? t("water.byPump") : by === "gravity" ? t("water.byGravity") : ""}
        </p>
        <div class="kv">
          {by === "pump" && (
            <>
              <span>{t("water.returnPump")}</span>
              <YesNo id="return-pump" value={hw.returnPump} yes="water.running" no="water.standing" />
            </>
          )}
          <span>{t("water.release")}</span>
          <span data-testid="release">
            <Pill tone={rel.on ? "ok" : "bad"}>{rel.on ? t("water.releaseOn") : why ? t("water.releaseOffWhy", { why }) : t("water.releaseOff")}</Pill>
          </span>
        </div>
        <p class="muted small">{t("water.releaseNote")}</p>
      </div>
    </Card>
  );
}

export function WaterPage() {
  return (
    <div class="stack">
      <div class="grid-2">
        <FlowCard />
        <ReturnCard />
      </div>
      <ChamberCard />
    </div>
  );
}
