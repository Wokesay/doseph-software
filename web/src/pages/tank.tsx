// SPDX-License-Identifier: AGPL-3.0-or-later
// Tank: its volume and a new fill, the flow through the device and the
// turnover, how long a dose is mixed in, the pause between checks and the
// control's settings.
import { useState } from "preact/hooks";
import { Clock, RefreshCw, SlidersHorizontal, Waves, Wrench } from "lucide-preact";
import { post, put } from "../api";
import { num, time } from "../format";
import { t } from "../i18n";
import { LIMITS, config, refreshConfig, refreshState, state, toast } from "../store";
import { Banner, Button, Card, Field, Modal, NumberInput, Term } from "../ui";

async function saveTank(body: Record<string, unknown>) {
  await put("/tank", body);
  await Promise.all([refreshConfig(), refreshState()]);
  toast(t("tank.saved"));
}

const inRange = (v: number | null, [lo, hi]: [number, number]) => v !== null && v >= lo && v <= hi;

function VolumeCard() {
  const cfg = config.value!;
  const st = state.value!;
  const [vol, setVol] = useState<number | null>(cfg.tank.volumeL);
  const [ask, setAsk] = useState(false);
  const bad = vol !== null && !inRange(vol, LIMITS.volumeL);
  const latch = st.latches["tank.volume"];
  return (
    <Card title={<Term topic="volume" />} icon={<Waves size={18} />}>
      <div class="stack">
        {latch && <Banner tone="bad">{t("tank.volumeLatch", { implied: num(latch.impliedL as number, 0), entered: num(latch.enteredL as number, 0) })}</Banner>}
        <div class="form-grid">
          <Field label={t("tank.volume")} error={bad ? t("tank.volumeRange", { min: LIMITS.volumeL[0], max: LIMITS.volumeL[1] }) : null}>
            <NumberInput value={vol} onValue={setVol} unit="L" name="volumeL" />
          </Field>
        </div>
        <div class="row">
          <Button variant="primary" onClick={() => saveTank({ volumeL: vol })} disabled={vol === null || bad || vol === cfg.tank.volumeL} data-testid="save-volume">
            {t("common.save")}
          </Button>
          <button class="btn" onClick={() => setAsk(true)} data-testid="new-tank">
            <RefreshCw size={16} /> {t("tank.newTank")}
          </button>
        </div>
        <p class="muted small">{st.newTank ? t("tank.testPending") : t("tank.volumeNote")}</p>
      </div>
      {ask && (
        <Modal
          title={t("tank.newTankAsk")}
          onClose={() => setAsk(false)}
          message={t("tank.newTankText")}
          footer={
            <>
              <button class="btn" onClick={() => setAsk(false)}>
                {t("common.cancel")}
              </button>
              <Button
                variant="primary"
                data-testid="new-tank-confirm"
                onClick={async () => {
                  await post("/tank/new");
                  await refreshState();
                  setAsk(false);
                  toast(t("tank.newTankDone"));
                }}
              >
                {t("tank.newTankConfirm")}
              </Button>
            </>
          }
        />
      )}
    </Card>
  );
}

function FlowCard() {
  const st = state.value!;
  const s = st.cycle.settings;
  const flow = st.readings.flow;
  return (
    <Card title={<Term topic="circulation" />} icon={<RefreshCw size={18} />}>
      <div class="stack">
        <p class="muted small">{t("tank.suctionText")}</p>
        <div class="kv" data-testid="flow-info">
          <span>{t("tank.flowNow")}</span>
          <span>{flow.usable ? `${num(flow.value, 1)} L/min` : "–"}</span>
          <span>{t("tank.flowMeasured")}</span>
          <span>{s.flowLpm != null ? `${num(s.flowLpm, 1)} L/min` : t("tank.notYet")}</span>
          <span>
            <Term topic="turnover" />
          </span>
          <span data-testid="turnover">{s.turnoverMin != null ? t("common.minutes", { n: num(s.turnoverMin, 1) }) : "–"}</span>
        </div>
        {s.flowLpm == null && <p class="muted small">{t("tank.flowUnknown")}</p>}
      </div>
    </Card>
  );
}

function WaitCard() {
  const cfg = config.value!;
  const s = state.value!.cycle.settings;
  const [least, setLeast] = useState<number | null>(cfg.tank.mixMinMin);
  const [hold, setHold] = useState<number | null>(cfg.tank.holdMin);
  const badLeast = least !== null && !inRange(least, LIMITS.mixMinMin);
  const badHold = hold === null || !inRange(hold, LIMITS.holdMin);
  // The wait in force and why (SD-112): one turnover, the user's minimum, or the shortest while the flow is not known
  const why = s.mixWhy === "user" ? t("tank.mixWhyUser") : s.mixWhy === "turnover" ? t("tank.mixWhyTurnover") : t("tank.mixWhyMinimum");
  return (
    <Card title={<Term topic="waitTime" />} icon={<Clock size={18} />}>
      <div class="stack">
        <p class="muted small">{t("tank.mixIntro")}</p>
        <p data-testid="mix-in-force">{s.mixMin !== null ? t("tank.mixInForce", { n: num(s.mixMin, 0), why }) : "–"}</p>
        <div class="form-grid">
          <Field
            label={t("tank.mixLeast")}
            error={badLeast ? t("tank.waitRange", { min: LIMITS.mixMinMin[0], max: LIMITS.mixMinMin[1] }) : null}
            hint={t("tank.mixLeastHint")}
          >
            <NumberInput value={least} onValue={setLeast} unit="min" name="mixMinMin" />
          </Field>
          <Field label={<Term topic="holdTime" />} error={badHold ? t("tank.holdRange", { min: LIMITS.holdMin[0], max: LIMITS.holdMin[1] }) : null}>
            <NumberInput value={hold} onValue={setHold} unit="min" name="holdMin" />
          </Field>
        </div>
        <div class="row">
          <Button variant="primary" onClick={() => saveTank({ mixMinMin: least, holdMin: hold })} disabled={badLeast || badHold} data-testid="save-waits">
            {t("common.save")}
          </Button>
        </div>
      </div>
    </Card>
  );
}

function ControlCard() {
  const c = config.value!.control;
  const [v, setV] = useState({ ...c });
  const set = (k: keyof typeof c) => (x: number | null) => setV({ ...v, [k]: x ?? NaN });
  const save = async () => {
    await put("/control", v);
    await refreshConfig();
    toast(t("tank.saved"));
  };
  const row = (k: keyof typeof c, unit?: string) => (
    <Field label={t(`tank.ctl.${k}`)}>
      <NumberInput value={Number.isNaN(v[k]) ? null : v[k]} onValue={set(k)} unit={unit} name={k} />
    </Field>
  );
  return (
    <Card title={t("tank.control")} icon={<SlidersHorizontal size={18} />}>
      <details>
        <summary>{t("tank.controlShow")}</summary>
        <div class="stack" style="margin-top:12px">
          <p class="muted small">{t("tank.controlIntro")}</p>
          <div class="form-grid">
            {row("ecTolerance", "mS/cm")}
            {row("phTolerance")}
            {row("maxEcStep", "mS/cm")}
            {row("ecFloor", "mS/cm")}
            {row("phMaxMlPerDose", "ml")}
            {row("maxEcRounds")}
            {row("maxPhDoses")}
          </div>
          <div>
            <Button onClick={save} disabled={Object.values(v).some((x) => Number.isNaN(x))}>
              {t("common.save")}
            </Button>
          </div>
        </div>
      </details>
    </Card>
  );
}

// Maintenance: the robot holds while someone works at tank or device (RAT-042)
function MaintenanceCard() {
  const until = state.value!.maintenanceUntil;
  const set = async (minutes: number) => {
    await post("/maintenance", { minutes });
    await refreshState();
    toast(t(minutes > 0 ? "tank.maint.started" : "tank.maint.ended"));
  };
  return (
    <Card title={t("tank.maint")} icon={<Wrench size={18} />}>
      <div class="stack-sm" data-testid="maintenance">
        <p class="muted small">{t("tank.maint.intro")}</p>
        {until ? (
          <div class="row">
            <span data-testid="maintenance-until">{t("tank.maint.until", { time: time(until) })}</span>
            <Button size="sm" onClick={() => set(0)}>
              {t("tank.maint.end")}
            </Button>
          </div>
        ) : (
          <div class="row">
            {[30, 60, 120].map((m) => (
              <Button size="sm" onClick={() => set(m)}>
                {t("tank.maint.start", { min: m })}
              </Button>
            ))}
          </div>
        )}
      </div>
    </Card>
  );
}

export function TankPage() {
  const rev = config.value!.revision;
  return (
    <div class="stack">
      <div class="grid-2">
        <VolumeCard key={`v${rev}`} />
        <FlowCard />
      </div>
      <WaitCard key={`w${rev}`} />
      <MaintenanceCard />
      <ControlCard key={`c${rev}`} />
    </div>
  );
}
