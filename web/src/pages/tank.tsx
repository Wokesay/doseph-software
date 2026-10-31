// SPDX-License-Identifier: AGPL-3.0-or-later
// Tank: its volume and a new fill, how long a dose is mixed in, the pause
// between checks, maintenance and its dates, and the control's settings.
// The flow through the device is on the water path page.
import { useEffect, useState } from "preact/hooks";
import { CalendarCheck, Clock, RefreshCw, SlidersHorizontal, Sprout, Timer, Waves, Wrench } from "lucide-preact";
import { GROW_SYSTEMS, get, holds, post, put, type GrowSystem, type ServicePlan } from "../api";
import { day, num, span, time } from "../format";
import { msg, t } from "../i18n";
import { LIMITS, config, refreshConfig, refreshState, state, toast, toastError } from "../store";
import { Banner, Button, Card, Field, Modal, NumberInput, Pill, Term } from "../ui";

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
  const latch = holds(st.latches["tank.volume"]) ? st.latches["tank.volume"] : undefined;
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

// The cultivation system (SD-128): one water for every plant on the tank. It sets the pH a new
// phase is offered and which templates come first; control never reads it.
function SystemCard() {
  const cfg = config.value!;
  const [system, setSystem] = useState<GrowSystem | "">(cfg.tank.system);
  return (
    <Card title={t("tank.system")} icon={<Sprout size={18} />}>
      <div class="stack">
        <p class="muted small">{t("tank.systemIntro")}</p>
        <div class="form-grid">
          <Field label={t("tank.system")}>
            <select class="select input" value={system} onChange={(e) => setSystem((e.target as HTMLSelectElement).value as GrowSystem | "")} name="system">
              <option value="">{t("tank.system.none")}</option>
              {GROW_SYSTEMS.map((g) => (
                <option value={g}>{t(`tank.system.${g}`)}</option>
              ))}
            </select>
          </Field>
        </div>
        <div>
          <Button variant="primary" onClick={() => saveTank({ system })} disabled={system === cfg.tank.system} data-testid="save-system">
            {t("common.save")}
          </Button>
        </div>
      </div>
    </Card>
  );
}

// Who prepares a fresh fill (SD-150): the robot, or the person by hand while the robot only holds.
// The estimate of a fresh fill, made at each check, helps to choose.
function PrepareCard() {
  const cfg = config.value!;
  const c = state.value!.cycle;
  const [prepare, setPrepare] = useState(cfg.tank.prepare);
  const f = c.fresh;
  const long = f?.ok === true && (f.more || f.totalMin > 24 * 60);
  return (
    <Card title={t("tank.prepare")} icon={<Timer size={18} />}>
      <div class="stack">
        <p class="muted small">{t("tank.prepareIntro")}</p>
        <p data-testid="fresh-estimate">
          {!f
            ? t("tank.freshNone")
            : !f.ok
              ? t("status.prepNone", { reason: msg(f.reason) })
              : f.more
                ? t("tank.freshMore", { time: span(f.totalMin * 60000) })
                : t("tank.fresh", { time: span(f.totalMin * 60000), rounds: f.rounds })}
        </p>
        {f?.ok && <p class="muted small">{t("tank.freshTap")}</p>}
        {f?.ok && f.limited && <p class="muted small">{t("status.prepLimited")}</p>}
        {f?.ok && f.daily && <p class="muted small">{t("status.prepDaily")}</p>}
        {long && cfg.tank.prepare === "robot" && <Banner tone="info">{t("tank.freshLong")}</Banner>}
        <Field label={t("tank.prepare")} hint={prepare === "hand" ? t("tank.prepareHandHint", { gap: num(c.settings.holdGapEc, 1) }) : undefined}>
          <select class="select input" value={prepare} onChange={(e) => setPrepare((e.target as HTMLSelectElement).value as "robot" | "hand")} name="prepare">
            <option value="robot">{t("tank.prepare.robot")}</option>
            <option value="hand">{t("tank.prepare.hand")}</option>
          </select>
        </Field>
        <div class="row">
          <Button variant="primary" onClick={() => saveTank({ prepare })} disabled={prepare === cfg.tank.prepare} data-testid="save-prepare">
            {t("common.save")}
          </Button>
        </div>
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
  const why =
    s.mixWhy === "user"
      ? t("tank.mixWhyUser")
      : s.mixWhy === "turnover"
        ? t("tank.mixWhyTurnover")
        : s.mixWhy === "settling"
          ? t("tank.mixWhySettling")
          : s.mixWhy === "settling_default"
            ? t("tank.mixWhySettlingDefault")
            : t("tank.mixWhyMinimum");
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

const SERVICE_KINDS = ["probe.ph.cal", "probe.ec.cal", "channel.cal", "chamber.clean", "trough.clean", "trough.rinse", "heads.check", "probe.ph.replace"] as const;

// Maintenance dates (SD-125): what is due within a week of the earliest joins its date; a
// calibration resets its item by itself, the rest is marked done here
function ServiceCard() {
  const [plan, setPlan] = useState<ServicePlan | null>(null);
  const [days, setDays] = useState<Record<string, number | null>>({});
  const load = () =>
    get<ServicePlan>("/service")
      .then(setPlan)
      .catch(() => {});
  const rev = config.value!.revision;
  useEffect(() => {
    load();
  }, [rev]);
  if (!plan) return null;
  const done = async (id: string) => {
    try {
      setPlan(await post<ServicePlan>(`/service/${encodeURIComponent(id)}/done`));
      await refreshState();
      toast(t("tank.service.noted"));
    } catch (e) {
      toastError(e);
    }
  };
  const saveDays = async () => {
    try {
      setPlan(await put<ServicePlan>("/service", { days }));
      await refreshConfig();
      setDays({});
      toast(t("tank.saved"));
    } catch (e) {
      toastError(e);
    }
  };
  const next = plan.dates[0];
  const at = (d: number) => plan.items.filter((i) => i.date === d);
  const interval = (kind: string) => plan.items.find((i) => i.kind === kind);
  return (
    <Card title={t("tank.service")} icon={<CalendarCheck size={18} />}>
      <div class="stack" data-testid="service">
        <p class="muted small">{t("tank.service.intro")}</p>
        {plan.dates.map((d, n) => (
          <div class="stack-sm">
            <div class="section-title">{n === 0 ? t("tank.service.next", { date: day(d) }) : day(d)}</div>
            <div class="list">
              {at(d).map((i) => (
                <div class="item wrap-row" data-testid={`service-${i.id}`}>
                  <div class="grow">
                    <div class="title">
                      {i.dueNow && <Pill tone="info">{t("tank.service.due")}</Pill>} {msg(i.label)}
                    </div>
                    <div class="muted small">{t("tank.service.every", { n: i.days, last: day(i.last) })}</div>
                  </div>
                  {n === 0 && (
                    <div class="row-actions">
                      {i.kind.endsWith(".cal") ? (
                        <a class="btn sm" href={i.kind === "channel.cal" ? "#/channels" : "#/water"}>
                          {t("tank.service.calibrate")}
                        </a>
                      ) : (
                        <Button size="sm" onClick={() => done(i.id)} data-testid={`service-done-${i.id}`}>
                          {t("tank.service.done")}
                        </Button>
                      )}
                    </div>
                  )}
                </div>
              ))}
            </div>
          </div>
        )).slice(0, next ? 3 : 0)}
        <details>
          <summary>{t("tank.service.intervals")}</summary>
          <div class="stack" style="margin-top:12px">
            <p class="muted small">{t("tank.service.intervalsHint")}</p>
            <div class="form-grid">
              {SERVICE_KINDS.filter((k) => interval(k)).map((k) => (
                <Field label={t(`tank.service.kind.${k}`)} hint={t("tank.service.default", { n: interval(k)!.defaultDays })}>
                  <NumberInput value={k in days ? days[k] : interval(k)!.days} onValue={(v) => setDays({ ...days, [k]: v })} unit={t("tank.service.dayUnit")} name={`service-${k}`} />
                </Field>
              ))}
            </div>
            <div>
              <Button onClick={saveDays} disabled={!Object.keys(days).length} data-testid="save-service">
                {t("common.save")}
              </Button>
            </div>
          </div>
        </details>
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
        <MaintenanceCard />
      </div>
      <SystemCard key={`s${rev}`} />
      <PrepareCard key={`p${rev}`} />
      <WaitCard key={`w${rev}`} />
      <ServiceCard />
      <ControlCard key={`c${rev}`} />
    </div>
  );
}
