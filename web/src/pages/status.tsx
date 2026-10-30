// SPDX-License-Identifier: AGPL-3.0-or-later
// Status: where the mixing cycle stands, the readings with their trend,
// locks to release, the monitoring and the latest events.
import { useEffect, useState } from "preact/hooks";
import { Activity, BellRing, CheckCircle2, FlaskConical, Lock, ShieldCheck } from "lucide-preact";
import { del, get, holds, post, type Alarm, type Check, type Phase, type RobotEvent, type SeriesData } from "../api";
import { dateTime, day, duration, num, time } from "../format";
import { msg, t, type TextKey } from "../i18n";
import { activeRecipe, config, refreshState, state, toast, toastError } from "../store";
import { Banner, Button, Card, CheckRow, Pill, Term, type Tone } from "../ui";
import { EventList, MetricTile } from "../widgets";

// The steps of one check, in order (SD-105)
const STEPS: [Phase, TextKey][] = [
  ["pump_on", "status.step.pumpOn"],
  ["prove_flow", "status.step.proveFlow"],
  ["flush", "status.step.flush"],
  ["measure", "status.step.measure"],
  ["dose", "status.step.dose"],
  ["mix", "status.step.mix"],
  ["remeasure", "status.step.remeasure"],
  ["hold", "status.step.hold"],
];

const PHASE: Record<Phase, [TextKey, Tone]> = {
  off: ["status.phase.off", "neutral"],
  stopped: ["status.phase.stopped", "bad"],
  pump_on: ["status.phase.checking", "info"],
  prove_flow: ["status.phase.checking", "info"],
  flush: ["status.phase.checking", "info"],
  measure: ["status.phase.checking", "info"],
  dose: ["status.phase.dosing", "info"],
  mix: ["status.phase.mixing", "info"],
  remeasure: ["status.phase.checking", "info"],
  hold: ["status.phase.hold", "ok"],
  wait: ["status.phase.wait", "warn"],
};

function CycleCard() {
  const st = state.value!;
  const c = st.cycle;
  const [label, tone] = PHASE[c.phase] ?? PHASE.off;
  const at = STEPS.findIndex(([p]) => p === c.phase);
  const pr = c.progress;
  const busy = tone === "info";
  return (
    <Card title={t("status.cycle")} icon={<Activity size={18} />} actions={<Pill tone={tone} dot pulse={busy}>{t(label)}</Pill>}>
      <div class="stack-sm">
        <p class="title" data-testid="cycle-line" style="font-weight:600">
          {msg(c.line)}
        </p>
        {at >= 0 && (
          <div class="phases" aria-label={t("status.steps")}>
            {STEPS.map(([, k], i) => (
              <span class={`ph ${i === at ? "cur" : i < at ? "done" : ""}`} aria-current={i === at ? "step" : undefined}>
                {t(k)}
              </span>
            ))}
          </div>
        )}
        {pr && pr.totalS != null && pr.totalS > 0 && (
          <div class="stack-sm">
            <div class="progress" role="progressbar" aria-valuemin={0} aria-valuemax={pr.totalS} aria-valuenow={pr.doneS ?? 0}>
              <span style={`width:${Math.min(100, ((pr.doneS ?? 0) / pr.totalS) * 100)}%`} />
            </div>
            <span class="small muted">
              {t("status.progress", { done: duration((pr.doneS ?? 0) * 1000), total: duration(pr.totalS * 1000) })}
              {pr.paused && <span class="warn-text"> · {t("status.paused")}</span>}
            </span>
          </div>
        )}
        {pr?.nextAt != null && (c.phase === "hold" || c.phase === "wait") && (
          <p class="muted small" data-testid="next-check">
            {t("status.nextCheck", { time: time(pr.nextAt) })}
          </p>
        )}
        {st.dosing && (
          <p class="small" data-testid="dosing">
            {t("status.dosing", { name: st.dosing.name, run: st.dosing.run, runs: st.dosing.runs, done: num(st.dosing.mlDone, 1), ml: num(st.dosing.ml, 1) })}
          </p>
        )}
        {c.round?.test && (
          <p class="small">
            <Term topic="testDose" />
          </p>
        )}
        {st.newTank && c.phase !== "off" && c.phase !== "stopped" && !c.round?.test && <p class="muted small">{t("status.newTank")}</p>}
        {c.mixPending && <p class="muted small">{t("status.mixPending")}</p>}
        {st.catchUp && (
          <Banner tone="info">
            <div class="stack" data-testid="catch-up">
              <span>
                {t("status.catchUp", { parts: st.catchUp.parts.map((p) => t("status.catchUpPart", { name: p.name, ml: num(p.ml, 1) })).join(", ") })}
              </span>
              <div>
                <Button
                  size="sm"
                  onClick={async () => {
                    await del("/catchup");
                    await refreshState();
                    toast(t("status.catchUpEnded"), "info");
                  }}
                >
                  {t("status.catchUpEnd")}
                </Button>
              </div>
            </div>
          </Banner>
        )}
        {c.notes.map((n) => (
          <Banner tone="warn">
            <span data-testid="cycle-note">{msg(n)}</span>
          </Banner>
        ))}
        {c.last && (
          <p class="faint small">
            {t("status.lastMeasured", { ph: num(c.last.ph, 2), ec: num(c.last.ec, 2), time: dateTime(c.last.at) })}
          </p>
        )}
        <WhyChecks />
      </div>
    </Card>
  );
}

// Why the robot doses or not: what each kind of dose needs, and whether it holds now (RAT-037)
function WhyChecks() {
  const ch = state.value!.cycle.checks;
  const groups: [TextKey, Check[]][] = [
    ["status.why.all", ch.all],
    ["status.why.ec", ch.ec],
    ["status.why.ph", ch.ph],
  ];
  const open = groups.reduce((n, [, items]) => n + items.filter((i) => !i.ok).length, 0);
  return (
    <details data-testid="checks">
      <summary>{t(open ? "status.why.open" : "status.why", { n: open })}</summary>
      {groups.map(([label, items]) => (
        <div class="checks" data-testid={`checks-${label.split(".").pop()}`}>
          <span class="small muted">{t(label)}</span>
          {items.map((i) => (
            <CheckRow ok={i.ok} text={msg(i)} />
          ))}
        </div>
      ))}
    </details>
  );
}

function Readings() {
  const st = state.value!;
  const s = st.cycle.settings;
  const [spark, setSpark] = useState<Record<string, (number | null)[]>>({});
  useEffect(() => {
    let alive = true;
    const load = async () => {
      const now = state.value?.now ?? Math.floor(Date.now() / 1000);
      const h = await get<{ series: SeriesData[] }>(`/history?series=ph,ec,temp,flow&from=${now - 6 * 3600}&to=${now}&points=120`);
      if (alive) setSpark(Object.fromEntries(h.series.map((x) => [x.series, x.avg])));
    };
    load().catch(() => {});
    const timer = setInterval(() => load().catch(() => {}), 60_000);
    return () => {
      alive = false;
      clearInterval(timer);
    };
  }, []);
  const band = (target: number | null, tol: number | null): [number, number] | null => (target != null && tol != null ? [target - tol, target + tol] : null);
  const lock = (ch: "ph" | "ec" | "temp") => {
    const l = st.jumpLocks[ch];
    return l && l.until > st.now ? t("status.jumpUntil", { time: time(l.until) }) : undefined;
  };
  const r = st.readings;
  const flowNote =
    s.flowLpm != null && s.turnoverMin != null ? t("status.turnover", { min: num(s.turnoverMin, 1) }) : st.feedPump === false ? t("status.pumpOff") : undefined;
  return (
    <div class="grid-3" style="grid-template-columns:repeat(auto-fit,minmax(min(200px,100%),1fr))">
      <MetricTile id="ph" label="pH" reading={r.ph} color="--ph" band={band(s.phTarget, s.phTolerance)} spark={spark.ph} note={lock("ph")} />
      <MetricTile id="ec" label="EC" reading={r.ec} color="--ec" band={band(s.ecTarget, s.ecTolerance)} spark={spark.ec} note={lock("ec")} />
      <MetricTile id="temp" label={t("status.temp")} reading={r.temp} color="--temp" spark={spark.temp} note={lock("temp")} />
      <MetricTile id="flow" label={t("status.flow")} reading={r.flow} color="--flow" spark={spark.flow} note={flowNote} />
    </div>
  );
}

function RecipeCard() {
  const st = state.value!;
  const s = st.cycle.settings;
  const r = activeRecipe.value;
  const plant = st.plan.plant;
  return (
    <Card title={t("nav.recipe")} icon={<FlaskConical size={18} />} actions={<a href={plant ? "#/plants" : "#/recipe"}>{t("status.change")}</a>}>
      {r ? (
        <div class="kv">
          {plant && (
            <>
              <span>{t("status.plant")}</span>
              <span data-testid="plan-plant">{t("status.plantPhase", { name: plant.name, phase: plant.phase })}</span>
            </>
          )}
          <span>{t("status.recipe")}</span>
          <span data-testid="recipe-name">{r.name}</span>
          <span>{t("status.ecTarget")}</span>
          <span>{s.ecTarget != null ? `${num(s.ecTarget, 2)} ± ${num(s.ecTolerance, 2)} mS/cm` : "–"}</span>
          <span>{t("status.phTarget")}</span>
          <span>{s.phTarget != null ? `${num(s.phTarget, 2)} ± ${num(s.phTolerance, 2)}` : "–"}</span>
          <span>{t("status.until")}</span>
          <span>
            {plant ? t(plant.confirm ? "status.phaseConfirm" : "status.phaseEnd", { date: day(plant.phaseEnd) }) : <ActiveUntil />}
          </span>
        </div>
      ) : (
        <p class="muted">{t("status.noRecipe")}</p>
      )}
    </Card>
  );
}

/** When the recipe in force ends, or that it has no end. */
export function ActiveUntil() {
  const a = config.value?.active;
  if (!a?.recipe) return <>–</>;
  return <>{a.until > 0 ? dateTime(a.until) : t("status.noEnd")}</>;
}

function Locks() {
  const st = state.value!;
  const ids = Object.keys(st.latches);
  if (!ids.length) return null;
  const text = (id: string) => {
    const item = st.watchdog.items.find((i) => i.id === `latch.${id}`);
    return item ? msg(item.text) : id;
  };
  return (
    <Card title={ids.some((id) => holds(st.latches[id])) ? <Term topic="latched" /> : t("status.reportsTitle")} icon={<Lock size={18} />} class="locks">
      <div class="list">
        {ids.map((id) => (
          <div class="item wrap-row" data-testid={`latch-${id}`}>
            <div class="grow">{text(id)}</div>
            {!holds(st.latches[id]) ? (
              // Its lock only reports (SD-133): nothing is held, and clearing it says it was seen
              <div class="row-actions">
                <Pill tone="warn">{t("status.reportedOnly")}</Pill>
                <Button size="sm" onClick={() => release(id, {}, "status.cleared")}>
                  {t("status.clear")}
                </Button>
              </div>
            ) : /^channel\.\d+\.head_check$/.test(id) ? (
              // A head of the same size fitted again: the person says whether it is new (SD-144)
              <div class="row-actions">
                <Button size="sm" onClick={() => release(id, { newHead: true })} data-testid={`new-head-${id}`}>
                  {t("status.newHead")}
                </Button>
                <Button size="sm" onClick={() => release(id, { newHead: false })} data-testid={`same-head-${id}`}>
                  {t("status.sameHead")}
                </Button>
              </div>
            ) : id === "tank.volume" ? (
              <div class="row-actions">
                <a class="btn sm" href="#/tank">
                  {t("status.correctVolume")}
                </a>
                <Button size="sm" onClick={() => release(id, { confirm: true })}>
                  {t("status.volumeRight")}
                </Button>
              </div>
            ) : (
              <Button size="sm" onClick={() => release(id, {})}>
                {t("status.release")}
              </Button>
            )}
          </div>
        ))}
      </div>
    </Card>
  );
}

async function release(id: string, body: object, done: "status.released" | "status.cleared" = "status.released") {
  try {
    await post(`/latches/${encodeURIComponent(id)}/ack`, body);
    await refreshState();
    toast(t(done));
  } catch (e) {
    toastError(e);  // e.g. the device still reports the cause (SD-110)
  }
}

const LEVEL_TONE: Record<Alarm["level"], Tone> = { critical: "bad", warning: "warn", info: "info" };

// The alarms raised (SD-123): acknowledging pauses a critical alarm's reminders, the alarm stays (RAT-022)
function AlarmsCard() {
  const st = state.value!;
  if (!st.alarms.length) return null;
  const ack = async (id: string) => {
    try {
      await post(`/alarms/${encodeURIComponent(id)}/ack`);
      await refreshState();
      toast(t("status.alarmAcked"));
    } catch (e) {
      toastError(e);
    }
  };
  return (
    <Card title={t("status.alarms")} icon={<BellRing size={18} />}>
      <div class="list">
        {st.alarms.map((a) => (
          <div class="item wrap-row" data-testid={`alarm-${a.id}`}>
            <div class="grow">
              <div class="title">
                <Pill tone={LEVEL_TONE[a.level]}>{t(`status.level.${a.level}`)}</Pill> {msg(a.label)}
              </div>
              <div class="small">{msg(a.text)}</div>
              <div class="muted small">
                {t("status.alarmSince", { time: dateTime(a.since) })}
                {a.ackUntil > st.now && ` · ${t("status.alarmPaused", { time: time(a.ackUntil) })}`}
              </div>
            </div>
            {a.level === "critical" && a.ackUntil <= st.now && (
              <div class="row-actions">
                <Button size="sm" onClick={() => ack(a.id)} data-testid={`ack-${a.id}`}>
                  {t("status.alarmAck")}
                </Button>
              </div>
            )}
          </div>
        ))}
      </div>
      <p class="muted small" style="margin-top:8px">
        {t("status.alarmAckNote")}
      </p>
    </Card>
  );
}

function WatchCard() {
  const wd = state.value!.watchdog;
  return (
    <Card title={t("status.watch")} icon={<ShieldCheck size={18} />} actions={wd.stale ? <Pill tone="bad">{t("shell.watchdogStale")}</Pill> : wd.overall === "ok" ? <CheckCircle2 size={18} color="var(--ok)" /> : null}>
      <div class="checks" data-testid="watchdog">
        {wd.items.map((i) => (
          <CheckRow ok={i.status === "ok"} neutral={i.status === "neutral"} soft={false} text={`${msg(i.label)}: ${msg(i.text)}`} />
        ))}
      </div>
    </Card>
  );
}

function RecentEvents() {
  const st = state.value!;
  const [events, setEvents] = useState<RobotEvent[]>([]);
  useEffect(() => {
    get<RobotEvent[]>("/events?limit=8").then(setEvents).catch(() => {});
  }, [st.eventId]);
  return (
    <Card title={t("status.events")} actions={<a href="#/history">{t("status.allEvents")}</a>}>
      <EventList events={events} />
    </Card>
  );
}

export function StatusPage() {
  return (
    <div class="stack">
      <CycleCard />
      <AlarmsCard />
      <Locks />
      <Readings />
      <div class="grid-2">
        <RecipeCard />
        <WatchCard />
      </div>
      <RecentEvents />
    </div>
  );
}
