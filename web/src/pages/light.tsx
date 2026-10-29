// SPDX-License-Identifier: AGPL-3.0-or-later
// Light (SD-135): each circuit with its state, its day by plan, a dimmer's brightness and ramps, the
// plant it follows, its next change and its output. A person switches a light on or off until the
// plan's next change, or back to the plan; on by hand in the dark period is said. Light follows its
// plan through STOP, a hold and maintenance.
import { useEffect, useState } from "preact/hooks";
import { Plus } from "lucide-preact";
import { del, get, post, put, type Devices, type LightCircuit, type Plants, type RobotState } from "../api";
import { deviceTime, duration, figure } from "../format";
import { msg, t } from "../i18n";
import { config, refreshConfig, refreshState, state, toast, toastError } from "../store";
import { Banner, Button, Card, Field, Modal, NumberInput, Pill, Seg } from "../ui";
import { RoleRow } from "./settings";

type Kind = "switched" | "dimmed";
type Status = RobotState["lighting"][number];
// The form keeps an emptied field empty, never 0 (R5); Save waits until what the circuit needs is there
type Form = Omit<LightCircuit, "hours" | "pct" | "thresholdPct" | "sunriseMin" | "sunsetMin"> & {
  hours: number | null; pct: number | null; thresholdPct: number | null; sunriseMin: number | null; sunsetMin: number | null;
};

const nb = " ";
const hhmm = (m: number) => `${String(Math.floor(m / 60)).padStart(2, "0")}:${String(m % 60).padStart(2, "0")}`;
// A time field's minutes of the day; nothing while it is empty or incomplete
const minutes = (v: string): number | null => {
  const [h, m] = v.split(":").map(Number);
  return v && Number.isFinite(h) && Number.isFinite(m) ? h * 60 + m : null;
};
const hoursText = (h: number) => `${figure(h)}${nb}h`;
const pctText = (p: number) => `${figure(Math.round(p * 10) / 10)}${nb}%`;
const minText = (m: number) => `${m}${nb}min`;
// The day's end, as the robot counts it: its start and its hours
const endMin = (startMin: number, hours: number) => (startMin + Math.round(hours * 60)) % 1440;

// The plan as it lights: the hours a followed phase sets
function plan(startMin: number, hours: number): string {
  if (hours <= 0) return t("light.plan.never");
  if (hours >= 24) return t("light.plan.always");
  return t("light.plan.day", { from: hhmm(startMin), to: hhmm(endMin(startMin, hours)), hours: hoursText(hours) });
}

const ToDevices = () => <a href="#/settings">{t("light.toDevices")}</a>;

function LightForm(p: { circuit: LightCircuit | null; devices: Devices | null; reload: () => unknown; onClose: () => void }) {
  // Ramps of 15 min unless set (RAT-035)
  const blank: Form = { id: "", name: "", dimmed: false, startMin: 360, hours: 18, pct: 100, thresholdPct: 0, sunriseMin: 15, sunsetMin: 15, plant: "" };
  const [c, setC] = useState<Form>(p.circuit ? { ...p.circuit, plant: p.circuit.plant ?? "" } : blank);
  const [plants, setPlants] = useState<Plants["plants"]>([]);
  useEffect(() => {
    get<Plants>("/plants")
      .then((d) => setPlants(d.plants.filter((x) => x.running || x.id === p.circuit?.plant)))
      .catch(() => {});
  }, []);
  const [id, setId] = useState(p.circuit?.id ?? "");
  const set = (patch: Partial<Form>) => setC({ ...c, ...patch });
  const shellies = (p.devices?.devices ?? []).filter((d) => d.kind === "shelly");
  const missing = !c.name.trim() || c.hours == null || (c.dimmed && (c.pct == null || c.thresholdPct == null || c.sunriseMin == null || c.sunsetMin == null));
  const save = async () => {
    try {
      // What is empty is left out: the robot keeps its default for it
      const body: Record<string, unknown> = { ...c, name: c.name.trim() };
      for (const k of ["hours", "pct", "thresholdPct", "sunriseMin", "sunsetMin"] as const) if (c[k] == null) delete body[k];
      if (id) body.id = id;
      else delete body.id;
      const r = await put<{ id: string }>("/lighting/circuits", body);
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
      toast(t("light.savedNext"));
      window.setTimeout(() => document.querySelector<HTMLSelectElement>(`select[name='role-device-light.${r.id}']`)?.focus(), 50);
    } catch (e) {
      toastError(e);
    }
  };
  const remove = async () => {
    if (!confirm(t("light.removeAsk", { name: p.circuit?.name ?? c.name }))) return;
    try {
      await del(`/lighting/circuits/${id}`);
      await refreshConfig();
      await refreshState();
      await p.reload();
      toast(t("light.removed"));
      p.onClose();
    } catch (e) {
      toastError(e);
    }
  };
  const role = id ? `light.${id}` : "";
  const preview = c.hours == null ? null : c.hours <= 0 ? t("light.previewNever") : c.hours >= 24 ? t("light.previewAlways") : t("light.preview", { from: hhmm(c.startMin), to: hhmm(endMin(c.startMin, c.hours)) });
  return (
    <Modal
      title={t(id ? "light.edit" : "light.new")}
      onClose={p.onClose}
      wide
      footer={
        <>
          {id && (
            <Button variant="danger-soft" onClick={remove} data-testid="remove-light">
              {t("light.delete")}
            </Button>
          )}
          <span style="flex:1" />
          <Button onClick={p.onClose}>{t("common.close")}</Button>
          <Button variant="primary" onClick={save} disabled={missing} data-testid="save-light">
            {t("common.save")}
          </Button>
        </>
      }
    >
      <div class="stack" data-testid="light-form">
        <Field label={t("light.name")}>
          <input class="input" name="light-name" maxLength={40} value={c.name} onInput={(e) => set({ name: (e.target as HTMLInputElement).value })} />
        </Field>
        <div class="section-title" id="light-kind">
          {t("light.kind")}
        </div>
        <Seg<Kind>
          value={c.dimmed ? "dimmed" : "switched"}
          onChange={(k) => set({ dimmed: k === "dimmed" })}
          labelledBy="light-kind"
          options={[
            ["switched", t("light.kind.switched")],
            ["dimmed", t("light.kind.dimmed")],
          ]}
        />
        <p class="muted small">{t(c.dimmed ? "light.kindHint.dimmed" : "light.kindHint.switched")}</p>
        <div class="form-grid">
          <Field label={t("light.start")}>
            <input class="input" type="time" name="light-start" value={hhmm(c.startMin)} onInput={(e) => set({ startMin: minutes((e.target as HTMLInputElement).value) ?? c.startMin })} />
          </Field>
          <Field label={t("light.hours")} hint={t("light.hoursHint")}>
            <NumberInput value={c.hours} onValue={(v) => set({ hours: v })} unit="h" min={0} max={24} step={0.5} name="light-hours" />
          </Field>
          {c.dimmed && (
            <>
              <Field label={t("light.pct")}>
                <NumberInput value={c.pct} onValue={(v) => set({ pct: v })} unit="%" min={1} max={100} name="light-pct" />
              </Field>
              <Field label={t("light.threshold")} hint={t("light.thresholdHint")}>
                <NumberInput value={c.thresholdPct} onValue={(v) => set({ thresholdPct: v })} unit="%" min={0} max={50} name="light-threshold" />
              </Field>
              <Field label={t("light.sunrise")} hint={t("light.rampHint")}>
                <NumberInput value={c.sunriseMin} onValue={(v) => set({ sunriseMin: v == null ? null : Math.round(v) })} unit="min" min={0} max={240} name="light-sunrise" />
              </Field>
              <Field label={t("light.sunset")}>
                <NumberInput value={c.sunsetMin} onValue={(v) => set({ sunsetMin: v == null ? null : Math.round(v) })} unit="min" min={0} max={240} name="light-sunset" />
              </Field>
            </>
          )}
        </div>
        {preview && (
          <p class="muted small" data-testid="light-preview">
            {preview}
            {c.plant && ` ${t("light.previewPlant")}`}
          </p>
        )}
        {plants.length > 0 && (
          <Field label={t("light.plant")} hint={t("light.plantHint")}>
            <select class="select input" name="light-plant" value={c.plant} onChange={(e) => set({ plant: (e.target as HTMLSelectElement).value })}>
              <option value="">{t("light.plantNone")}</option>
              {plants.map((x) => (
                <option value={x.id}>{x.running ? x.name : t("light.plantDone", { name: x.name })}</option>
              ))}
            </select>
          </Field>
        )}
        {shellies.length === 0 ? (
          <p class="muted small">
            {t("light.outputNoShelly")} <ToDevices />
          </p>
        ) : role && p.devices?.roles[role] ? (
          <RoleRow key={`${role}${JSON.stringify(p.devices.roles[role]?.binding)}`} role={role} label={t("light.output")} devices={p.devices} reload={p.reload} />
        ) : (
          <p class="muted small">{t("light.outputHint")}</p>
        )}
      </div>
    </Modal>
  );
}

function LightCard(p: { c: LightCircuit; s: Status | undefined; devices: Devices | null; onEdit: () => void }) {
  const now = state.value!.now;
  // On the device's clock, as its plan
  const at = (epoch: number) => deviceTime(epoch, config.value?.system.timezone);
  const s = p.s;
  const name = p.c.name;
  const role = p.devices?.roles[`light.${p.c.id}`];
  const device = role?.binding ? p.devices?.devices.find((d) => d.id === role.binding!.device) : null;
  const problem = s?.output?.problem ?? null;
  const hours = s?.hours ?? p.c.hours;
  // What the lamp does, not only what its plan wants: without an output, or one that cannot switch, that is said
  const bound = !!role?.binding;
  const cannot = s?.output?.state === "offline" || s?.output?.state === "refused" ? s.output.state : null;
  const hand = async (on: boolean | null) => {
    try {
      await post(`/lighting/circuits/${p.c.id}/hand`, { on });
      await refreshState();
      toast(t(on == null ? "light.planAgain" : on ? "light.switchedOn" : "light.switchedOff", { name }));
    } catch (e) {
      toastError(e);
    }
  };
  const read = s?.output?.brightness;
  return (
    <div class="item stacked" data-testid={`light-${p.c.id}`}>
      <div class="stack-sm" style="gap:4px">
        <h3 style="margin:0;font-size:1rem">{name}</h3>
        <span class="row small" style="flex-wrap:wrap" data-testid={`light-state-${p.c.id}`}>
          {!bound ? (
            <Pill tone="neutral">{t("light.state.noOutput")}</Pill>
          ) : cannot ? (
            <Pill tone="bad">{t(`settings.devices.state.${cannot}`)}</Pill>
          ) : (
            s && (s.on ? <Pill tone="ok">{s.pct != null ? t("light.state.onPct", { pct: pctText(s.pct) }) : t("light.state.on")}</Pill> : <Pill tone="neutral">{t("light.state.off")}</Pill>)
          )}
          {s?.hand && <Pill tone="accent">{t(s.nextChangeAt != null ? "light.state.hand" : "light.state.handDay", { at: at(s.hand.until) })}</Pill>}
          {bound && problem && <span class="muted">{msg(problem)}</span>}
        </span>
      </div>
      {bound && s?.darkOn && <Banner tone="warn">{t("light.darkOnText")}</Banner>}
      <div class="stack-sm small" style="gap:4px">
        <div>
          <span class="muted">{t("light.plan")}: </span>
          <span data-testid={`light-plan-${p.c.id}`}>
            {plan(p.c.startMin, hours)}
            {p.c.dimmed && hours > 0 && ` · ${pctText(s?.plant?.phase != null ? s.plant.pct : p.c.pct)}`}
          </span>
        </div>
        {p.c.dimmed && (p.c.sunriseMin > 0 || p.c.sunsetMin > 0) && hours > 0 && hours < 24 && (
          <div class="muted">{t("light.ramps", { rise: minText(p.c.sunriseMin), set: minText(p.c.sunsetMin) })}</div>
        )}
        {s?.plant && (
          <div>
            <span class="muted">{t("light.plant")}: </span>
            <span data-testid={`light-plant-${p.c.id}`}>
              {s.plant.phase == null
                ? t("light.followsDone", { plant: s.plant.name })
                : p.c.dimmed
                  ? t("light.follows", { plant: s.plant.name, phase: s.plant.phase, hours: hoursText(s.plant.hours), pct: pctText(s.plant.pct) })
                  : t("light.followsHours", { plant: s.plant.name, phase: s.plant.phase, hours: hoursText(s.plant.hours) })}
            </span>
          </div>
        )}
        {s?.nextChangeAt != null && (
          <div>
            <span class="muted">{t("light.next")}: </span>
            <span data-testid={`light-next-${p.c.id}`}>{t(s.day ? "light.nextOff" : "light.nextOn", { at: at(s.nextChangeAt), in: duration((s.nextChangeAt - now) * 1000) })}</span>
          </div>
        )}
        <div>
          <span class="muted">{t("light.output")}: </span>
          <span data-testid={`light-output-${p.c.id}`}>
            {role?.binding && device
              ? p.c.dimmed && read != null && s?.on
                ? t("light.outputRead", { device: device.name, channel: role.binding.channel, pct: pctText(read) })
                : t("light.outputAt", { device: device.name, channel: role.binding.channel })
              : t("light.outputNone")}
          </span>
        </div>
      </div>
      <div class="row" style="flex-wrap:wrap">
        {!bound && !s?.hand ? null : s?.hand ? (
          <Button size="sm" onClick={() => hand(null)} aria-label={t("light.byPlanAria", { name })} data-testid={`light-plan-again-${p.c.id}`}>
            {t("light.byPlan")}
          </Button>
        ) : s?.on ? (
          <Button size="sm" onClick={() => hand(false)} aria-label={t("light.offAria", { name })} data-testid={`light-off-${p.c.id}`}>
            {t("light.off")}
          </Button>
        ) : (
          <Button size="sm" onClick={() => hand(true)} aria-label={t("light.onAria", { name })} data-testid={`light-on-${p.c.id}`}>
            {t("light.on")}
          </Button>
        )}
        <span class="grow" />
        <Button size="sm" variant="ghost" onClick={p.onEdit} aria-label={t("light.editAria", { name })} data-testid={`light-edit-${p.c.id}`}>
          {t("common.edit")}
        </Button>
      </div>
    </div>
  );
}

export function LightPage() {
  const cfg = config.value!;
  const [devices, setDevices] = useState<Devices | null>(null);
  const [editing, setEditing] = useState<LightCircuit | "new" | null>(null);
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
  const circuits = cfg.lighting.circuits;
  const status = state.value.lighting ?? [];
  const noShelly = devices != null && !devices.devices.some((d) => d.kind === "shelly");
  return (
    <div class="stack">
      <Card
        title={t("light.circuits")}
        actions={
          <Button size="sm" variant="primary" onClick={() => setEditing("new")} data-testid="add-light">
            <Plus size={16} />
            {t("light.add")}
          </Button>
        }
      >
        <p class="muted small">{t("light.intro")}</p>
        <div class="list" data-testid="lights">
          {circuits.length === 0 &&
            (noShelly ? (
              <p class="muted">
                {t("light.noneNoShelly")} <ToDevices />
              </p>
            ) : (
              <p class="muted">{t("light.none")}</p>
            ))}
          {circuits.map((c) => (
            <LightCard key={c.id} c={c} s={status.find((x) => x.id === c.id)} devices={devices} onEdit={() => setEditing(c)} />
          ))}
        </div>
        {circuits.length > 0 && <p class="muted small">{t("light.handNote")}</p>}
      </Card>
      {editing && <LightForm circuit={editing === "new" ? null : editing} devices={devices} reload={load} onClose={() => setEditing(null)} />}
    </div>
  );
}
