// SPDX-License-Identifier: AGPL-3.0-or-later
// Watering from the tank (SD-134): the tank's water for watering and its reserve, then each circuit
// with its schedule, its output, today's run time and its last shot. A circuit waters now by hand;
// where the water is not fit, a person may water anyway after a confirmation that names why
// (RAT-086). An empty tank, Stop or a dose stop even that.
import { useEffect, useState } from "preact/hooks";
import { Droplets, Plus, Trash2 } from "lucide-preact";
import { del, get, post, put, type Devices, type Msg, type WaterCircuit } from "../api";
import { ago, num } from "../format";
import { msg, t } from "../i18n";
import { config, refreshConfig, refreshState, state, toast, toastError } from "../store";
import { Banner, Button, Card, Field, Modal, NumberInput, Pill, Seg, Toggle } from "../ui";
import { RoleRow } from "./settings";

type Mode = WaterCircuit["mode"];

// Why a circuit cannot water that a person may water over: the water, not the tank or the robot
const OVERRIDABLE = new Set(["water.no.mixing", "water.no.reading", "water.no.calibrating", "water.no.ph", "water.no.ec", "water.no.ec_max"]);

const hhmm = (m: number) => `${String(Math.floor(m / 60)).padStart(2, "0")}:${String(m % 60).padStart(2, "0")}`;
const minutes = (v: string) => {
  const [h, m] = v.split(":").map(Number);
  return Number.isFinite(h) && Number.isFinite(m) ? h * 60 + m : 0;
};

function schedule(c: WaterCircuit): string {
  if (c.mode === "window") return t("watering.sched.window", { n: c.shotsPerDay, s: c.shotS, from: hhmm(c.fromMin), to: hhmm(c.toMin) });
  if (c.mode === "times") return t("watering.sched.times", { list: c.times.map((x) => `${hhmm(x.atMin)} (${num(x.s, 0)} s)`).join(", ") });
  if (c.mode === "interval") return t("watering.sched.interval", { min: c.everyMin, s: c.shotS });
  return t("watering.sched.manual");
}

function TankCard() {
  const w = state.value!.watering;
  const cfg = config.value!;
  const [reserve, setReserve] = useState<number | null>(cfg.watering.reservePct);
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
    <Card title={t("watering.tank")} icon={<Droplets size={18} />}>
      <div class="stack" data-testid="watering-tank">
        {w.low && <Banner tone="warn">{t("watering.low")}</Banner>}
        <div class="kv">
          <span>{t("watering.water")}</span>
          <span data-testid="watering-fit">
            {w.fit ? (
              <Pill tone="warn" wrap>
                {msg(w.fit)}
              </Pill>
            ) : w.mixing ? (
              <Pill tone="info" wrap>
                {t("watering.mixing")}
              </Pill>
            ) : (
              <Pill tone="ok">{t("watering.fit")}</Pill>
            )}
          </span>
          <span>{t("watering.given")}</span>
          <span data-testid="watering-given">{w.givenL != null ? `${num(w.givenL, 1)} L` : t("watering.noCount")}</span>
          <span>{t("watering.reserve")}</span>
          <span>{w.reserveL != null ? `${num(w.reserveL, 1)} L` : "–"}</span>
          <span>{t("watering.available")}</span>
          <span data-testid="watering-available">{w.availableL != null ? `${num(w.availableL, 1)} L` : "–"}</span>
        </div>
        {w.warning && <p class="small">{msg(w.warning)}</p>}
        {w.givenL == null && <p class="muted small">{t("watering.noCountHint")}</p>}
        <p class="muted small">{t("watering.countIntro")}</p>
        <div class="row" style="align-items:flex-end">
          <Field label={t("watering.reservePct")} hint={t("watering.reserveHint")}>
            <NumberInput value={reserve} onValue={setReserve} unit="%" min={10} max={90} name="reserve-pct" />
          </Field>
          <Button size="sm" onClick={save} data-testid="save-reserve">
            {t("common.save")}
          </Button>
        </div>
      </div>
    </Card>
  );
}

// A circuit's settings, for a new one or a change; its output is assigned once it is saved
function CircuitForm(p: { circuit: WaterCircuit | null; devices: Devices | null; reload: () => unknown; onClose: () => void }) {
  const blank: WaterCircuit = { id: "", name: "", mode: "manual", fromMin: 480, toMin: 1200, shotsPerDay: 6, shotS: 60, times: [], everyMin: 240, longestS: 300, rateLpm: null, dailyMaxS: null, paused: false };
  const [c, setC] = useState<WaterCircuit>(p.circuit ?? blank);
  const [id, setId] = useState(p.circuit?.id ?? "");
  const set = (patch: Partial<WaterCircuit>) => setC({ ...c, ...patch });
  const save = async () => {
    try {
      const body: Partial<WaterCircuit> = { ...c };
      if (!id) delete body.id;
      else body.id = id;
      const r = await put<{ id: string }>("/watering/circuits", body);
      await refreshConfig();
      await refreshState();
      await p.reload();
      toast(t("settings.saved"));
      if (!id) setId(r.id);  // a new circuit: its output is assigned next
      else p.onClose();
    } catch (e) {
      toastError(e);
    }
  };
  const role = id ? `water.${id}` : "";
  return (
    <Modal
      title={t(p.circuit ? "watering.edit" : "watering.new")}
      onClose={p.onClose}
      wide
      footer={
        <>
          <Button onClick={p.onClose}>{t("common.close")}</Button>
          <Button variant="primary" onClick={save} data-testid="save-circuit">
            {t("common.save")}
          </Button>
        </>
      }
    >
      <div class="stack" data-testid="circuit-form">
        <div class="form-grid">
          <Field label={t("watering.name")}>
            <input class="input" name="circuit-name" maxLength={40} value={c.name} onInput={(e) => set({ name: (e.target as HTMLInputElement).value })} />
          </Field>
          <Field label={t("watering.longestS")} hint={t("watering.longestHint")}>
            <NumberInput value={c.longestS} onValue={(v) => set({ longestS: v ?? 0 })} unit="s" min={1} max={86400} name="circuit-longest" />
          </Field>
          <Field label={t("watering.rateLpm")} hint={t("watering.rateHint")}>
            <NumberInput value={c.rateLpm} onValue={(v) => set({ rateLpm: v })} unit="L/min" min={0.01} max={1000} step={0.1} name="circuit-rate" />
          </Field>
          <Field label={t("watering.dailyMaxS")} hint={t("watering.dailyMaxHint")}>
            <NumberInput value={c.dailyMaxS} onValue={(v) => set({ dailyMaxS: v })} unit="s" min={1} max={86400} name="circuit-daily" />
          </Field>
        </div>
        <div class="section-title" id="circuit-mode">
          {t("watering.mode")}
        </div>
        <Seg<Mode>
          value={c.mode}
          onChange={(mode) => set({ mode, times: mode === "times" && c.times.length === 0 ? [{ atMin: 480, s: Math.min(60, c.longestS) }] : c.times })}
          labelledBy="circuit-mode"
          options={[
            ["window", t("watering.mode.window")],
            ["times", t("watering.mode.times")],
            ["interval", t("watering.mode.interval")],
            ["manual", t("watering.mode.manual")],
          ]}
        />
        {c.mode === "window" && (
          <div class="form-grid">
            <Field label={t("watering.from")}>
              <input class="input" type="time" name="circuit-from" value={hhmm(c.fromMin)} onInput={(e) => set({ fromMin: minutes((e.target as HTMLInputElement).value) })} />
            </Field>
            <Field label={t("watering.to")}>
              <input class="input" type="time" name="circuit-to" value={hhmm(c.toMin)} onInput={(e) => set({ toMin: minutes((e.target as HTMLInputElement).value) })} />
            </Field>
            <Field label={t("watering.shotsPerDay")}>
              <NumberInput value={c.shotsPerDay} onValue={(v) => set({ shotsPerDay: v ?? 0 })} min={1} max={1440} name="circuit-shots" />
            </Field>
            <Field label={t("watering.shotS")}>
              <NumberInput value={c.shotS} onValue={(v) => set({ shotS: v ?? 0 })} unit="s" min={1} name="circuit-shot" />
            </Field>
          </div>
        )}
        {c.mode === "interval" && (
          <div class="form-grid">
            <Field label={t("watering.everyMin")}>
              <NumberInput value={c.everyMin} onValue={(v) => set({ everyMin: v ?? 0 })} unit="min" min={1} max={1440} name="circuit-every" />
            </Field>
            <Field label={t("watering.shotS")}>
              <NumberInput value={c.shotS} onValue={(v) => set({ shotS: v ?? 0 })} unit="s" min={1} name="circuit-shot" />
            </Field>
          </div>
        )}
        {c.mode === "times" && (
          <div class="stack-sm">
            {c.times.map((x, i) => (
              <div class="row" style="align-items:flex-end" data-testid={`circuit-time-${i}`}>
                <Field label={t("watering.time")}>
                  <input
                    class="input"
                    type="time"
                    name={`circuit-time-${i}`}
                    value={hhmm(x.atMin)}
                    onInput={(e) => set({ times: c.times.map((y, j) => (j === i ? { ...y, atMin: minutes((e.target as HTMLInputElement).value) } : y)) })}
                  />
                </Field>
                <Field label={t("watering.timeS")}>
                  <NumberInput value={x.s} onValue={(v) => set({ times: c.times.map((y, j) => (j === i ? { ...y, s: v ?? 0 } : y)) })} unit="s" min={1} name={`circuit-time-s-${i}`} />
                </Field>
                <Button size="sm" variant="ghost" aria-label={t("watering.removeTime")} onClick={() => set({ times: c.times.filter((_, j) => j !== i) })}>
                  <Trash2 size={16} />
                </Button>
              </div>
            ))}
            <div>
              <Button size="sm" onClick={() => set({ times: [...c.times, { atMin: 720, s: Math.min(60, c.longestS) }] })} data-testid="add-time">
                <Plus size={16} />
                {t("watering.addTime")}
              </Button>
            </div>
          </div>
        )}
        <div class="section-title">{t("watering.output")}</div>
        {role && p.devices?.roles[role] ? (
          <RoleRow key={`${role}${JSON.stringify(p.devices.roles[role]?.binding)}`} role={role} label={t("watering.output")} devices={p.devices} reload={p.reload} />
        ) : (
          <p class="muted small">{t("watering.outputHint")}</p>
        )}
      </div>
    </Modal>
  );
}

function CircuitCard(p: { c: WaterCircuit; devices: Devices | null; onEdit: () => void; reload: () => unknown }) {
  const st = state.value!;
  const w = st.watering;
  const s = w.circuits.find((x) => x.id === p.c.id);
  const running = w.running === p.c.id;
  const [seconds, setSeconds] = useState<number | null>(Math.min(p.c.mode === "manual" || p.c.mode === "times" ? 60 : p.c.shotS, p.c.longestS));
  const [anyway, setAnyway] = useState<Msg | null>(null);
  const role = p.devices?.roles[`water.${p.c.id}`];
  const device = role?.binding ? p.devices?.devices.find((d) => d.id === role.binding!.device) : null;
  const blocked = s?.blocked ?? null;
  const canAnyway = !!blocked && OVERRIDABLE.has(blocked.key ?? "");
  const now = async (override: boolean) => {
    try {
      await post(`/watering/circuits/${p.c.id}/now`, { seconds, override });
      setAnyway(null);
      await refreshState();
      toast(t("watering.started"));
    } catch (e) {
      toastError(e);
    }
  };
  const stop = async () => {
    try {
      await post(`/watering/circuits/${p.c.id}/stop`);
      await refreshState();
      toast(t("watering.stopped"));
    } catch (e) {
      toastError(e);
    }
  };
  const pause = async (paused: boolean) => {
    try {
      await post(`/watering/circuits/${p.c.id}/pause`, { paused });
      await refreshConfig();
      await refreshState();
    } catch (e) {
      toastError(e);
    }
  };
  const remove = async () => {
    if (!confirm(t("watering.removeAsk", { name: p.c.name }))) return;
    try {
      await del(`/watering/circuits/${p.c.id}`);
      await refreshConfig();
      await refreshState();
      await p.reload();
      toast(t("watering.removed"));
    } catch (e) {
      toastError(e);
    }
  };
  const last = s?.last;
  return (
    <div class="item stacked" data-testid={`circuit-${p.c.id}`}>
      <div class="row" style="justify-content:space-between">
        <strong>{p.c.name}</strong>
        <span class="row small" data-testid={`circuit-state-${p.c.id}`}>
          {running ? (
            <Pill tone="info" pulse>
              {t("watering.state.running")}
            </Pill>
          ) : p.c.paused ? (
            <Pill tone="neutral">{t("watering.state.paused")}</Pill>
          ) : blocked ? (
            <span class="muted">{msg(blocked)}</span>
          ) : (
            <Pill tone="ok">{t("watering.state.ready")}</Pill>
          )}
          {s?.due && !running && <Pill tone="accent">{t("watering.state.due")}</Pill>}
        </span>
      </div>
      <div class="stack-sm small" style="gap:4px">
        <div>
          <span class="muted">{t("watering.mode")}: </span>
          <span data-testid={`circuit-schedule-${p.c.id}`}>{schedule(p.c)}</span>
        </div>
        <div>
          <span class="muted">{t("watering.output")}: </span>
          {role?.binding && device ? t("watering.outputAt", { device: device.name, channel: role.binding.channel }) : t("watering.outputNone")}
        </div>
        <div>
          <span class="muted">{t("watering.today")}: </span>
          <span data-testid={`circuit-today-${p.c.id}`}>
            {p.c.dailyMaxS != null ? t("watering.todayOf", { s: s?.dayS ?? 0, max: p.c.dailyMaxS }) : t("watering.todayS", { s: s?.dayS ?? 0 })}
          </span>
        </div>
        <div>
          <span class="muted">{t("watering.last")}: </span>
          <span data-testid={`circuit-last-${p.c.id}`}>
            {last
              ? t(last.l != null ? "watering.lastShot" : "watering.lastShotNoL", { ago: ago(st.now - last.at), s: last.s, l: last.l, end: msg(last.end) })
              : t("watering.never")}
          </span>
        </div>
      </div>
      <div class="row" style="align-items:flex-end;flex-wrap:wrap">
        {running ? (
          <Button size="sm" onClick={stop} data-testid={`circuit-stop-${p.c.id}`}>
            {t("watering.stop")}
          </Button>
        ) : (
          <>
            <Field label={t("watering.seconds")}>
              <NumberInput value={seconds} onValue={setSeconds} unit="s" min={1} max={p.c.longestS} name={`circuit-seconds-${p.c.id}`} ariaLabel={t("watering.seconds")} />
            </Field>
            {canAnyway ? (
              <Button size="sm" variant="danger-soft" onClick={() => setAnyway(blocked)} disabled={!seconds} data-testid={`circuit-anyway-${p.c.id}`}>
                {t("watering.anyway")}
              </Button>
            ) : (
              <Button size="sm" variant="primary" onClick={() => now(false)} disabled={!!blocked || !seconds} data-testid={`circuit-now-${p.c.id}`}>
                {t("watering.now")}
              </Button>
            )}
          </>
        )}
        {p.c.mode !== "manual" && (
          <label class="row small" style="gap:6px">
            <Toggle checked={p.c.paused} onChange={pause} label={t("watering.pause")} />
            {t("watering.pause")}
          </label>
        )}
        <span class="grow" />
        <Button size="sm" variant="ghost" onClick={p.onEdit} data-testid={`circuit-edit-${p.c.id}`}>
          {t("common.edit")}
        </Button>
        <Button size="sm" variant="ghost" onClick={remove} data-testid={`circuit-remove-${p.c.id}`}>
          {t("common.delete")}
        </Button>
      </div>
      {anyway && (
        <Modal
          title={t("watering.anywayTitle")}
          onClose={() => setAnyway(null)}
          footer={
            <>
              <Button onClick={() => setAnyway(null)}>{t("common.cancel")}</Button>
              <Button variant="danger" onClick={() => now(true)} data-testid="confirm-anyway">
                {t("watering.anywayYes")}
              </Button>
            </>
          }
        >
          <div class="stack-sm">
            <p>{t("watering.anywayText")}</p>
            <p>
              <strong data-testid="anyway-reason">{msg(anyway)}</strong>
            </p>
            <p class="muted small">{t("watering.anywayNote", { s: seconds })}</p>
          </div>
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
    load();
    void refreshConfig().catch(() => {});  // circuits changed elsewhere, by another browser or the app
    const id = window.setInterval(load, 5000);
    return () => window.clearInterval(id);
  }, []);
  if (!state.value) return null;
  const circuits = cfg.watering.circuits;
  return (
    <div class="stack">
      <TankCard />
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
          {circuits.length === 0 && <p class="muted">{t("watering.none")}</p>}
          {circuits.map((c) => (
            <CircuitCard key={c.id} c={c} devices={devices} onEdit={() => setEditing(c)} reload={load} />
          ))}
        </div>
      </Card>
      {editing && <CircuitForm circuit={editing === "new" ? null : editing} devices={devices} reload={load} onClose={() => setEditing(null)} />}
    </div>
  );
}
