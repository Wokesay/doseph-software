// SPDX-License-Identifier: AGPL-3.0-or-later
// Watering from the tank (SD-134): the tank's water for watering, each circuit with its state, its
// schedule, its next shot, its output, today's run time and its last shot, and at the end the
// reserve. A circuit waters now by hand; where only the water stands in the way, a person may water
// anyway after a confirmation that names why (RAT-086). An empty tank, Stop or a dose stop even that.
import { useEffect, useState } from "preact/hooks";
import { Droplets, Plus, Trash2 } from "lucide-preact";
import { del, get, post, put, type Devices, type WaterCircuit } from "../api";
import { ago, duration, num, time } from "../format";
import { msg, t } from "../i18n";
import { config, refreshConfig, refreshState, state, toast, toastError } from "../store";
import { Banner, Button, Card, Field, Modal, NumberInput, Pill, Seg, Toggle } from "../ui";
import { RoleRow } from "./settings";

type Mode = WaterCircuit["mode"];
// The form keeps an emptied field empty, never 0 (R5); Save waits until what the schedule needs is there
type Form = Omit<WaterCircuit, "paused" | "longestS" | "shotS" | "shotsPerDay" | "everyMin" | "times"> & {
  longestS: number | null; shotS: number | null; shotsPerDay: number | null; everyMin: number | null; times: { atMin: number; s: number | null }[];
};

const nb = " ";
const hhmm = (m: number) => `${String(Math.floor(m / 60)).padStart(2, "0")}:${String(m % 60).padStart(2, "0")}`;
// A time field's minutes of the day; nothing while it is empty or incomplete
const minutes = (v: string): number | null => {
  const [h, m] = v.split(":").map(Number);
  return v && Number.isFinite(h) && Number.isFinite(m) ? h * 60 + m : null;
};
const whole = (v: number | null) => (v == null ? null : Math.round(v));
const every = (min: number) => (min < 60 ? `${min}${nb}min` : min % 60 === 0 ? `${min / 60}${nb}h` : duration(min * 60000));

// The shots of a window, as the robot spreads them: from its start to its end, over the day without an end
function windowShots(fromMin: number, toMin: number, n: number): number[] {
  const span = (((toMin - fromMin) % 1440) + 1440) % 1440;
  const at: number[] = [];
  for (let k = 0; k < n; ++k) at.push((fromMin + (span === 0 ? Math.floor((k * 1440) / n) : n === 1 ? 0 : Math.floor((k * span) / (n - 1)))) % 1440);
  return [...new Set(at)].sort((a, b) => a - b);
}

function schedule(c: WaterCircuit): string {
  if (c.mode === "window") return t("watering.sched.window", { n: c.shotsPerDay, s: c.shotS, from: hhmm(c.fromMin), to: hhmm(c.toMin) });
  if (c.mode === "times")
    return t("watering.sched.times", {
      list: [...c.times]
        .sort((a, b) => a.atMin - b.atMin)
        .map((x) => `${hhmm(x.atMin)} (${num(x.s, 0)}${nb}s)`)
        .join(", "),
    });
  if (c.mode === "interval") return t("watering.sched.interval", { every: every(c.everyMin), s: c.shotS });
  return t("watering.sched.manual");
}

const ToDevices = () => <a href="#/settings">{t("watering.toDevices")}</a>;

function TankCard(p: { devices: Devices | null }) {
  const w = state.value!.watering;
  const refill = !!p.devices?.roles.refill_valve?.binding;
  const fullFloat = !!p.devices?.roles.tank_full?.binding;
  return (
    <Card title={t("watering.tank")} icon={<Droplets size={18} />}>
      <div class="stack" data-testid="watering-tank">
        {w.low && <Banner tone="warn">{t(refill ? "watering.lowRefill" : "watering.low")}</Banner>}
        <div data-testid="watering-fit">
          {w.fit ? (
            <Banner tone="warn">{msg(w.fit)}</Banner>
          ) : w.mixing ? (
            <Pill tone="info" wrap>
              {t("watering.mixing")}
            </Pill>
          ) : (
            <Pill tone="ok">{t("watering.fit")}</Pill>
          )}
        </div>
        {w.warning && <Banner tone="warn">{msg(w.warning)}</Banner>}
        <div class="kv">
          <span>{t("watering.available")}</span>
          <span data-testid="watering-available">{w.availableL != null ? `${num(Math.max(0, w.availableL), 1)}${nb}L` : "–"}</span>
          <span>{t("watering.given")}</span>
          <span data-testid="watering-given">{w.givenL != null ? `${num(w.givenL, 1)}${nb}L` : t("watering.noCount")}</span>
          <span>{t("watering.reserve")}</span>
          <span>{w.reserveL != null ? `${num(w.reserveL, 1)}${nb}L` : "–"}</span>
        </div>
        {w.reserveL == null && (
          <p class="muted small">
            <a href="#/tank">{t("watering.noVolume")}</a>
          </p>
        )}
        {w.givenL == null && (
          <p class="muted small">
            {t("watering.noCountHint")}
            {fullFloat && ` ${t("watering.noCountFloat")}`}
          </p>
        )}
      </div>
    </Card>
  );
}

function ReserveCard() {
  const pct = config.value!.watering.reservePct;
  const [reserve, setReserve] = useState<number | null>(pct);
  useEffect(() => setReserve(pct), [pct]);
  const save = async () => {
    try {
      await put("/watering", { reservePct: reserve });
      await refreshConfig();
      await refreshState();
      toast(t("settings.saved"));
    } catch (e) {
      toastError(e);
    }
  };
  return (
    <Card title={t("watering.reserve")}>
      <div class="stack-sm">
        <p class="muted small">{t("watering.countIntro")}</p>
        <div class="row" style="align-items:flex-end;flex-wrap:wrap">
          <Field label={t("watering.reservePct")} hint={t("watering.reserveHint")}>
            <NumberInput value={reserve} onValue={setReserve} unit="%" min={10} max={90} name="reserve-pct" />
          </Field>
          <Button size="sm" onClick={save} disabled={reserve == null || reserve === pct} data-testid="save-reserve">
            {t("common.save")}
          </Button>
        </div>
      </div>
    </Card>
  );
}

// A circuit's settings, for a new one or a change; its output is assigned once it is saved. A pause
// is not part of it: only the switch on the card sets one, so a change never ends one made elsewhere.
function CircuitForm(p: { circuit: WaterCircuit | null; devices: Devices | null; reload: () => unknown; onClose: () => void }) {
  const blank: Form = { id: "", name: "", mode: "manual", fromMin: 480, toMin: 1200, shotsPerDay: 6, shotS: 60, times: [], everyMin: 240, longestS: 300, rateLpm: null, dailyMaxS: null };
  const [c, setC] = useState<Form>(p.circuit ? { ...p.circuit } : blank);
  const [id, setId] = useState(p.circuit?.id ?? "");
  const set = (patch: Partial<Form>) => setC({ ...c, ...patch });
  const running = !!id && state.value?.watering.running === id;
  const shellies = (p.devices?.devices ?? []).filter((d) => d.kind === "shelly");
  const missing =
    !c.name.trim() ||
    c.longestS == null ||
    ((c.mode === "window" || c.mode === "interval") && c.shotS == null) ||
    (c.mode === "window" && c.shotsPerDay == null) ||
    (c.mode === "interval" && c.everyMin == null) ||
    (c.mode === "times" && (c.times.length === 0 || c.times.some((x) => x.s == null)));
  const save = async () => {
    try {
      // What the schedule does not use and is empty stays as it was
      const body: Record<string, unknown> = { ...c, name: c.name.trim() };
      for (const k of ["longestS", "shotS", "shotsPerDay", "everyMin"] as const) if (c[k] == null) delete body[k];
      if (id) body.id = id;
      else delete body.id;
      const r = await put<{ id: string }>("/watering/circuits", body);
      const fresh = !id;
      if (fresh) setId(r.id);  // saved: a second Save changes it, whatever follows fails
      await refreshConfig();
      await refreshState();
      await p.reload();
      if (!fresh) {
        toast(t("settings.saved"));
        p.onClose();
        return;
      }
      // A new circuit stays open: its output is assigned next
      toast(t("watering.savedNext"));
      window.setTimeout(() => document.querySelector<HTMLSelectElement>(`select[name='role-device-water.${r.id}']`)?.focus(), 50);
    } catch (e) {
      toastError(e);
    }
  };
  const remove = async () => {
    if (!confirm(t("watering.removeAsk", { name: p.circuit?.name ?? c.name }))) return;
    try {
      await del(`/watering/circuits/${id}`);
      await refreshConfig();
      await refreshState();
      await p.reload();
      toast(t("watering.removed"));
      p.onClose();
    } catch (e) {
      toastError(e);
    }
  };
  const role = id ? `water.${id}` : "";
  const nextTime = () => ((c.times[c.times.length - 1]?.atMin ?? 420) + 60) % 1440;
  const preview = c.mode === "window" && c.shotsPerDay != null && c.shotsPerDay >= 1 && c.shotsPerDay <= 48 ? windowShots(c.fromMin, c.toMin, c.shotsPerDay) : [];
  return (
    <Modal
      title={t(id ? "watering.edit" : "watering.new")}
      onClose={p.onClose}
      wide
      footer={
        <>
          {id && (
            <Button variant="danger-soft" onClick={remove} data-testid="remove-circuit">
              {t("watering.delete")}
            </Button>
          )}
          <span style="flex:1" />
          <Button onClick={p.onClose}>{t("common.close")}</Button>
          <Button variant="primary" onClick={save} disabled={missing} data-testid="save-circuit">
            {t("common.save")}
          </Button>
        </>
      }
    >
      <div class="stack" data-testid="circuit-form">
        {running && <Banner tone="info">{t("watering.editRunning")}</Banner>}
        <Field label={t("watering.name")}>
          <input class="input" name="circuit-name" maxLength={40} value={c.name} onInput={(e) => set({ name: (e.target as HTMLInputElement).value })} />
        </Field>
        <div class="section-title" id="circuit-mode">
          {t("watering.mode")}
        </div>
        <Seg<Mode>
          value={c.mode}
          onChange={(mode) => set({ mode, times: mode === "times" && c.times.length === 0 ? [{ atMin: 480, s: Math.min(60, c.longestS ?? 60) }] : c.times })}
          labelledBy="circuit-mode"
          options={[
            ["window", t("watering.mode.window")],
            ["times", t("watering.mode.times")],
            ["interval", t("watering.mode.interval")],
            ["manual", t("watering.mode.manual")],
          ]}
        />
        {c.mode === "window" && (
          <>
            <div class="form-grid">
              <Field label={t("watering.from")}>
                <input class="input" type="time" name="circuit-from" value={hhmm(c.fromMin)} onInput={(e) => set({ fromMin: minutes((e.target as HTMLInputElement).value) ?? c.fromMin })} />
              </Field>
              <Field label={t("watering.to")}>
                <input class="input" type="time" name="circuit-to" value={hhmm(c.toMin)} onInput={(e) => set({ toMin: minutes((e.target as HTMLInputElement).value) ?? c.toMin })} />
              </Field>
              <Field label={t("watering.shotsPerDay")}>
                <NumberInput value={c.shotsPerDay} onValue={(v) => set({ shotsPerDay: whole(v) })} min={1} max={1440} name="circuit-shots" />
              </Field>
              <Field label={t("watering.shotS")}>
                <NumberInput value={c.shotS} onValue={(v) => set({ shotS: v })} unit="s" min={1} name="circuit-shot" />
              </Field>
            </div>
            {preview.length > 0 && (
              <p class="muted small" data-testid="window-shots">
                {t("watering.windowShots", { list: preview.map(hhmm).join(", ") })}
              </p>
            )}
          </>
        )}
        {c.mode === "interval" && (
          <>
            <div class="form-grid">
              <Field label={t("watering.everyMin")}>
                <NumberInput value={c.everyMin} onValue={(v) => set({ everyMin: whole(v) })} unit="min" min={1} max={1440} name="circuit-every" />
              </Field>
              <Field label={t("watering.shotS")}>
                <NumberInput value={c.shotS} onValue={(v) => set({ shotS: v })} unit="s" min={1} name="circuit-shot" />
              </Field>
            </div>
            <p class="muted small">{t("watering.intervalNote")}</p>
          </>
        )}
        {c.mode === "times" && (
          <div class="stack-sm">
            {c.times.map((x, i) => (
              <div key={i} class="row" style="align-items:flex-end;flex-wrap:wrap" data-testid={`circuit-time-${i}`}>
                <Field label={t("watering.time")}>
                  <input
                    class="input"
                    type="time"
                    name={`circuit-time-${i}`}
                    value={hhmm(x.atMin)}
                    onInput={(e) => set({ times: c.times.map((y, j) => (j === i ? { ...y, atMin: minutes((e.target as HTMLInputElement).value) ?? y.atMin } : y)) })}
                  />
                </Field>
                <Field label={t("watering.timeS")}>
                  <NumberInput value={x.s} onValue={(v) => set({ times: c.times.map((y, j) => (j === i ? { ...y, s: v } : y)) })} unit="s" min={1} name={`circuit-time-s-${i}`} />
                </Field>
                <Button size="sm" variant="ghost" aria-label={t("watering.removeTime")} onClick={() => set({ times: c.times.filter((_, j) => j !== i) })}>
                  <Trash2 size={16} />
                </Button>
              </div>
            ))}
            <div>
              <Button size="sm" onClick={() => set({ times: [...c.times, { atMin: nextTime(), s: Math.min(60, c.longestS ?? 60) }] })} data-testid="add-time">
                <Plus size={16} />
                {t("watering.addTime")}
              </Button>
            </div>
          </div>
        )}
        <div class="form-grid">
          <Field label={t("watering.longestS")} hint={t("watering.longestHint")}>
            <NumberInput value={c.longestS} onValue={(v) => set({ longestS: v })} unit="s" min={1} max={86400} name="circuit-longest" />
          </Field>
          <Field label={t("watering.dailyMaxS")} hint={t("watering.dailyMaxHint")}>
            <NumberInput value={c.dailyMaxS} onValue={(v) => set({ dailyMaxS: v })} unit="s" min={1} max={86400} name="circuit-daily" />
          </Field>
          <Field label={t("watering.rateLpm")} hint={t("watering.rateHint")}>
            <NumberInput value={c.rateLpm} onValue={(v) => set({ rateLpm: v })} unit="L/min" min={0.01} max={1000} step={0.1} name="circuit-rate" />
          </Field>
        </div>
        {shellies.length === 0 ? (
          <p class="muted small">
            {t("watering.outputNoShelly")} <ToDevices />
          </p>
        ) : role && p.devices?.roles[role] ? (
          <RoleRow key={`${role}${JSON.stringify(p.devices.roles[role]?.binding)}`} role={role} label={t("watering.output")} devices={p.devices} reload={p.reload} />
        ) : (
          <p class="muted small">{t("watering.outputHint")}</p>
        )}
      </div>
    </Modal>
  );
}

function CircuitCard(p: { c: WaterCircuit; devices: Devices | null; onEdit: () => void }) {
  const st = state.value!;
  const s = st.watering.circuits.find((x) => x.id === p.c.id);
  const running = st.watering.running === p.c.id;
  const paused = s?.paused ?? p.c.paused;
  const [seconds, setSeconds] = useState<number | null>(Math.min(p.c.mode === "manual" || p.c.mode === "times" ? 60 : p.c.shotS, p.c.longestS));
  const [asking, setAsking] = useState(false);
  // A shorter longest shot: the length for "Water now" follows it
  useEffect(() => {
    if (seconds != null && seconds > p.c.longestS) setSeconds(p.c.longestS);
  }, [p.c.longestS]);
  // The question closes once the water is no longer what stands in the way
  useEffect(() => {
    if (asking && !s?.anyway) setAsking(false);
  }, [asking, s?.anyway]);
  const role = p.devices?.roles[`water.${p.c.id}`];
  const device = role?.binding ? p.devices?.devices.find((d) => d.id === role.binding!.device) : null;
  const hand = s?.hand ?? null;
  const tooLong = seconds != null && seconds > p.c.longestS;
  const name = p.c.name;
  const now = async (override: boolean) => {
    try {
      await post(`/watering/circuits/${p.c.id}/now`, { seconds, override });
      setAsking(false);
      await refreshState();
      toast(t("watering.started", { name }));
    } catch (e) {
      toastError(e);
    }
  };
  const stop = async () => {
    try {
      await post(`/watering/circuits/${p.c.id}/stop`);
      await refreshState();
      toast(t("watering.stopped", { name }));
    } catch (e) {
      toastError(e);
    }
  };
  const pause = async (on: boolean) => {
    try {
      await post(`/watering/circuits/${p.c.id}/pause`, { paused: on });
      await refreshConfig();
      await refreshState();
    } catch (e) {
      toastError(e);
    }
  };
  const last = s?.last;
  // Why it does not water: the schedule's next shot for a schedule, any shot by hand otherwise
  const why = p.c.mode === "manual" || paused ? hand : (s?.blocked ?? null);
  // The water's own reason, where the tank is also still being mixed in (RAT-086)
  const fit = st.watering.fit;
  const reasons = hand ? [hand, ...(fit && fit.key !== hand.key ? [fit] : [])] : [];
  return (
    <div class="item stacked" data-testid={`circuit-${p.c.id}`}>
      <div class="stack-sm" style="gap:4px">
        <h3 style="margin:0;font-size:1rem">{name}</h3>
        <span class="row small" style="flex-wrap:wrap" data-testid={`circuit-state-${p.c.id}`}>
          {running ? (
            <Pill tone={st.watering.override ? "warn" : "info"} dot pulse>
              {t(st.watering.override ? "watering.state.anyway" : "watering.state.running")}
              {st.watering.leftS != null && ` · ${t("watering.state.left", { left: duration(st.watering.leftS * 1000) })}`}
            </Pill>
          ) : (
            <>
              {paused && <Pill tone="neutral">{t("watering.state.paused")}</Pill>}
              {why ? <span class="muted">{msg(why)}</span> : !paused && <Pill tone="ok">{t("watering.state.ready")}</Pill>}
            </>
          )}
          {s?.due && !running && <Pill tone="accent">{t("watering.state.due")}</Pill>}
        </span>
      </div>
      <div class="stack-sm small" style="gap:4px">
        <div>
          <span class="muted">{t("watering.mode")}: </span>
          <span data-testid={`circuit-schedule-${p.c.id}`}>{schedule(p.c)}</span>
        </div>
        {s?.nextAt != null && s.nextS != null && (
          <div>
            <span class="muted">{t("watering.next")}: </span>
            <span data-testid={`circuit-next-${p.c.id}`}>{t("watering.nextAt", { at: time(s.nextAt), in: duration((s.nextAt - st.now) * 1000), s: s.nextS })}</span>
          </div>
        )}
        <div>
          <span class="muted">{t("watering.output")}: </span>
          {role?.binding && device ? t("watering.outputAt", { device: device.name, channel: role.binding.channel }) : t("watering.outputNone")}
        </div>
        <div>
          <span class="muted">{t("watering.today")}: </span>
          <span data-testid={`circuit-today-${p.c.id}`}>
            {!s ? "–" : p.c.dailyMaxS != null ? t("watering.todayOf", { s: s.dayS, max: p.c.dailyMaxS }) : t("watering.todayS", { s: s.dayS })}
          </span>
        </div>
        <div>
          <span class="muted">{t("watering.last")}: </span>
          <span data-testid={`circuit-last-${p.c.id}`}>
            {last
              ? `${t(last.upTo ? (last.l != null ? "watering.lastUpTo" : "watering.lastUpToNoL") : last.l != null ? "watering.lastShot" : "watering.lastShotNoL", {
                  ago: ago(st.now - last.at),
                  s: last.s,
                  l: last.l,
                  end: msg(last.end),
                })}${last.override ? ` ${t("watering.lastAnyway")}` : ""}`
              : t("watering.never")}
          </span>
        </div>
      </div>
      <div class="row" style="align-items:flex-end;flex-wrap:wrap">
        {running ? (
          <Button size="sm" onClick={stop} aria-label={t("watering.stopAria", { name })} data-testid={`circuit-stop-${p.c.id}`}>
            {t("watering.stop")}
          </Button>
        ) : (
          <>
            <div style="width:9rem">
              <Field label={t("watering.length")} error={tooLong ? t("watering.tooLong", { max: p.c.longestS }) : null}>
                <NumberInput value={seconds} onValue={setSeconds} unit="s" min={1} max={p.c.longestS} name={`circuit-seconds-${p.c.id}`} ariaLabel={t("watering.length")} />
              </Field>
            </div>
            {s?.anyway ? (
              <Button size="sm" variant="danger-soft" onClick={() => setAsking(true)} disabled={!seconds || tooLong} aria-label={t("watering.anywayAria", { name })} data-testid={`circuit-anyway-${p.c.id}`}>
                {t("watering.anyway")}
              </Button>
            ) : (
              <Button size="sm" variant="primary" onClick={() => now(false)} disabled={!!hand || !seconds || tooLong} aria-label={t("watering.nowAria", { name })} data-testid={`circuit-now-${p.c.id}`}>
                {t("watering.now")}
              </Button>
            )}
          </>
        )}
      </div>
      <div class="row" style="flex-wrap:wrap">
        {(p.c.mode !== "manual" || paused) && (
          <div class="row small" style="gap:6px" data-testid={`circuit-pause-${p.c.id}`}>
            <Toggle checked={paused} onChange={pause} label={t("watering.pauseAria", { name })} />
            <span aria-hidden="true">{t("watering.pause")}</span>
          </div>
        )}
        <span class="grow" />
        <Button size="sm" variant="ghost" onClick={p.onEdit} aria-label={t("watering.editAria", { name })} data-testid={`circuit-edit-${p.c.id}`}>
          {t("common.edit")}
        </Button>
      </div>
      {asking && hand && (
        <Modal
          title={t("watering.anywayTitle")}
          onClose={() => setAsking(false)}
          message={
            <>
              {t("watering.anywayText")}{" "}
              <strong data-testid="anyway-reason">{reasons.map((r) => msg(r)).join("; ")}</strong>
            </>
          }
          footer={
            <>
              <Button onClick={() => setAsking(false)}>{t("common.cancel")}</Button>
              <Button variant="danger" onClick={() => now(true)} data-testid="confirm-anyway">
                {t("watering.anywayYes")}
              </Button>
            </>
          }
        >
          <p class="muted small">{t("watering.anywayNote", { s: seconds })}</p>
        </Modal>
      )}
    </div>
  );
}

export function WateringPage() {
  const cfg = config.value!;
  const [devices, setDevices] = useState<Devices | null>(null);
  const [editing, setEditing] = useState<WaterCircuit | "new" | null>(null);
  const load = () => get<Devices>("/devices").then(setDevices).catch(() => {});
  useEffect(() => {
    // Circuits changed elsewhere, by another browser or the app, show too
    const poll = () => {
      load();
      void refreshConfig().catch(() => {});
    };
    poll();
    const id = window.setInterval(poll, 5000);
    return () => window.clearInterval(id);
  }, []);
  if (!state.value) return null;
  const circuits = cfg.watering.circuits;
  const noShelly = devices != null && !devices.devices.some((d) => d.kind === "shelly");
  return (
    <div class="stack">
      <TankCard devices={devices} />
      <Card
        title={t("watering.circuits")}
        actions={
          <Button size="sm" variant="primary" onClick={() => setEditing("new")} data-testid="add-circuit">
            <Plus size={16} />
            {t("watering.add")}
          </Button>
        }
      >
        <p class="muted small">{t("watering.intro")}</p>
        <div class="list" data-testid="circuits">
          {circuits.length === 0 &&
            (noShelly ? (
              <p class="muted">
                {t("watering.noneNoShelly")} <ToDevices />
              </p>
            ) : (
              <p class="muted">{t("watering.none")}</p>
            ))}
          {circuits.map((c) => (
            <CircuitCard key={c.id} c={c} devices={devices} onEdit={() => setEditing(c)} />
          ))}
        </div>
      </Card>
      <ReserveCard />
      {editing && <CircuitForm circuit={editing === "new" ? null : editing} devices={devices} reload={load} onClose={() => setEditing(null)} />}
    </div>
  );
}
