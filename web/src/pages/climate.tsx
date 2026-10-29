// SPDX-License-Identifier: AGPL-3.0-or-later
// Climate (SD-136): each room with its day, its air against its targets, its devices, the plant it
// follows, and what holds: heat protection on its light, CO2 above 2,000 ppm, a CO2 valve locked.
// A room is added and changed in a dialog, where its devices and sensors are assigned.
import { useEffect, useState } from "preact/hooks";
import { Plus } from "lucide-preact";
import { CLIMATE_OUTPUTS, CLIMATE_SENSORS, del, get, post, put, type ClimateRoom, type Devices, type Plants, type RobotState } from "../api";
import { figure } from "../format";
import { t, type TextKey } from "../i18n";
import { config, refreshConfig, refreshState, state, toast, toastError } from "../store";
import { Banner, Button, Card, Field, Modal, NumberInput, Pill, Seg, Toggle } from "../ui";
import { RoleRow } from "./settings";

type Status = RobotState["climate"][number];
// The form keeps an emptied field empty, never 0 (R5); Save waits until what the room needs is there
type Form = Omit<ClimateRoom, "hours" | "dayC" | "nightC" | "rhPct" | "vpdKpa" | "heatC"> & {
  hours: number | null; dayC: number | null; nightC: number | null; rhPct: number | null; vpdKpa: number | null; heatC: number | null;
};

const nb = " ";
const hhmm = (m: number) => `${String(Math.floor(m / 60)).padStart(2, "0")}:${String(m % 60).padStart(2, "0")}`;
const minutes = (v: string): number | null => {
  const [h, m] = v.split(":").map(Number);
  return v && Number.isFinite(h) && Number.isFinite(m) ? h * 60 + m : null;
};
const unit = (v: number | null | undefined, u: string) => (v == null ? null : `${figure(v)}${nb}${u}`);

const ToDevices = () => <a href="#/settings">{t("climate.toDevices")}</a>;

function RoomForm(p: { room: ClimateRoom | null; devices: Devices | null; reload: () => unknown; onClose: () => void }) {
  const cfg = config.value!;
  const blank: Form = {
    id: "", name: "", lights: [], startMin: 360, hours: 18, dayC: 26, nightC: 22, byVpd: false, rhPct: 70, vpdKpa: 1.0,
    co2Ppm: null, heatC: 30, heaterW: null, circulateAlways: false, plant: "",
  };
  const [c, setC] = useState<Form>(p.room ? { ...p.room } : blank);
  const [plants, setPlants] = useState<Plants["plants"]>([]);
  useEffect(() => {
    get<Plants>("/plants")
      .then((d) => setPlants(d.plants.filter((x) => x.running || x.id === p.room?.plant)))
      .catch(() => {});
  }, []);
  const [id, setId] = useState(p.room?.id ?? "");
  const set = (patch: Partial<Form>) => setC({ ...c, ...patch });
  // A light circuit lies in one room only: those of the other rooms are not offered
  const taken = new Set(cfg.climate.rooms.filter((r) => r.id !== id).flatMap((r) => r.lights));
  const lights = cfg.lighting.circuits.filter((l) => !taken.has(l.id));
  const shellies = (p.devices?.devices ?? []).filter((d) => d.kind === "shelly");
  const missing =
    !c.name.trim() || c.dayC == null || c.nightC == null || c.heatC == null || (c.byVpd ? c.vpdKpa == null : c.rhPct == null) || (c.lights.length === 0 && c.hours == null);
  const save = async () => {
    try {
      // What is empty is left out: the robot keeps its default for it; CO2 and the heater's power
      // empty are none
      const body: Record<string, unknown> = { ...c, name: c.name.trim() };
      for (const k of ["hours", "dayC", "nightC", "rhPct", "vpdKpa", "heatC"] as const) if (c[k] == null) delete body[k];
      if (id) body.id = id;
      else delete body.id;
      const r = await put<{ id: string }>("/climate/rooms", body);
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
      // A new room stays open: its devices and sensors are assigned next
      toast(t("climate.savedNext"));
      window.setTimeout(() => document.querySelector<HTMLSelectElement>(`select[name='role-device-climate.${r.id}.exhaust']`)?.focus(), 50);
    } catch (e) {
      toastError(e);
    }
  };
  const remove = async () => {
    if (!confirm(t("climate.removeAsk", { name: p.room?.name ?? c.name }))) return;
    try {
      await del(`/climate/rooms/${id}`);
      await refreshConfig();
      await refreshState();
      await p.reload();
      toast(t("climate.removed"));
      p.onClose();
    } catch (e) {
      toastError(e);
    }
  };
  const toggleLight = (lid: string, on: boolean) => set({ lights: on ? [...c.lights, lid] : c.lights.filter((x) => x !== lid) });
  return (
    <Modal
      title={t(id ? "climate.edit" : "climate.new")}
      onClose={p.onClose}
      wide
      footer={
        <>
          {id && (
            <Button variant="danger-soft" onClick={remove} data-testid="remove-room">
              {t("climate.delete")}
            </Button>
          )}
          <span style="flex:1" />
          <Button onClick={p.onClose}>{t("common.close")}</Button>
          <Button variant="primary" onClick={save} disabled={missing} data-testid="save-room">
            {t("common.save")}
          </Button>
        </>
      }
    >
      <div class="stack" data-testid="room-form">
        <Field label={t("climate.name")}>
          <input class="input" name="room-name" maxLength={40} value={c.name} onInput={(e) => set({ name: (e.target as HTMLInputElement).value })} />
        </Field>
        <div class="section-title">{t("climate.lights")}</div>
        {lights.length > 0 ? (
          <div class="stack-sm" data-testid="room-lights">
            {lights.map((l) => (
              <label class="row small" style="gap:6px">
                <Toggle checked={c.lights.includes(l.id)} onChange={(v) => toggleLight(l.id, v)} label={l.name} />
                <span aria-hidden="true">{l.name}</span>
              </label>
            ))}
            <p class="muted small">{t("climate.lightsHint")}</p>
          </div>
        ) : null}
        {c.lights.length === 0 && (
          <>
            <p class="muted small">{t("climate.noLights")}</p>
            <div class="form-grid">
              <Field label={t("climate.start")}>
                <input class="input" type="time" name="room-start" value={hhmm(c.startMin)} onInput={(e) => set({ startMin: minutes((e.target as HTMLInputElement).value) ?? c.startMin })} />
              </Field>
              <Field label={t("climate.hours")}>
                <NumberInput value={c.hours} onValue={(v) => set({ hours: v })} unit="h" min={0} max={24} step={0.5} name="room-hours" />
              </Field>
            </div>
          </>
        )}
        <div class="section-title" id="room-lead">
          {t("climate.targets")}
        </div>
        <div class="form-grid">
          <Field label={t("climate.dayC")}>
            <NumberInput value={c.dayC} onValue={(v) => set({ dayC: v })} unit="°C" min={5} max={40} step={0.5} name="room-day-c" />
          </Field>
          <Field label={t("climate.nightC")}>
            <NumberInput value={c.nightC} onValue={(v) => set({ nightC: v })} unit="°C" min={5} max={40} step={0.5} name="room-night-c" />
          </Field>
        </div>
        <Field label={t("climate.lead")}>
          <Seg<"rh" | "vpd">
            value={c.byVpd ? "vpd" : "rh"}
            onChange={(v) => set({ byVpd: v === "vpd" })}
            labelledBy="room-lead"
            options={[
              ["rh", t("climate.lead.rh")],
              ["vpd", t("climate.lead.vpd")],
            ]}
          />
        </Field>
        <div class="form-grid">
          {c.byVpd ? (
            <Field label={t("climate.vpdKpa")} hint={t("climate.vpdHint")}>
              <NumberInput value={c.vpdKpa} onValue={(v) => set({ vpdKpa: v })} unit="kPa" min={0.2} max={2.5} step={0.05} name="room-vpd" />
            </Field>
          ) : (
            <Field label={t("climate.rhPct")}>
              <NumberInput value={c.rhPct} onValue={(v) => set({ rhPct: v })} unit="%" min={20} max={95} name="room-rh" />
            </Field>
          )}
          <Field label={t("climate.co2Ppm")} hint={t("climate.co2Hint")}>
            <NumberInput value={c.co2Ppm} onValue={(v) => set({ co2Ppm: v == null ? null : Math.round(v) })} unit="ppm" min={400} max={1500} step={50} name="room-co2" />
          </Field>
          <Field label={t("climate.heatC")} hint={t("climate.heatHint")}>
            <NumberInput value={c.heatC} onValue={(v) => set({ heatC: v })} unit="°C" min={20} max={45} step={0.5} name="room-heat" />
          </Field>
          <Field label={t("climate.heaterW")} hint={t("climate.heaterHint")}>
            <NumberInput value={c.heaterW} onValue={(v) => set({ heaterW: v })} unit="W" min={50} max={3500} step={10} name="room-heater-w" />
          </Field>
        </div>
        <label class="row small" style="gap:6px">
          <Toggle checked={c.circulateAlways} onChange={(v) => set({ circulateAlways: v })} label={t("climate.circulateAlways")} />
          <span aria-hidden="true">{t("climate.circulateAlways")}</span>
        </label>
        {plants.length > 0 && (
          <Field label={t("climate.plant")} hint={t("climate.plantHint")}>
            <select class="select input" name="room-plant" value={c.plant} onChange={(e) => set({ plant: (e.target as HTMLSelectElement).value })}>
              <option value="">{t("climate.plantNone")}</option>
              {plants.map((x) => (
                <option value={x.id}>{x.running ? x.name : t("climate.plantDone", { name: x.name })}</option>
              ))}
            </select>
          </Field>
        )}
        {!id ? (
          <p class="muted small">{t("climate.rolesHint")}</p>
        ) : !p.devices ? null : (
          <>
            <div class="section-title">{t("climate.devices")}</div>
            {shellies.length === 0 && (
              <p class="muted small">
                {t("climate.noShelly")} <ToDevices />
              </p>
            )}
            {shellies.length > 0 &&
              CLIMATE_OUTPUTS.map((kind) => {
                const role = `climate.${id}.${kind}`;
                return p.devices!.roles[role] ? (
                  <RoleRow key={`${role}${JSON.stringify(p.devices!.roles[role]?.binding)}`} role={role} label={t(`climate.out.${kind}` as TextKey)} devices={p.devices!} reload={p.reload} />
                ) : null;
              })}
            <div class="section-title">{t("climate.sensors")}</div>
            {CLIMATE_SENSORS.map((kind) => {
              const role = `climate.${id}.${kind}`;
              return p.devices!.roles[role] ? (
                <RoleRow key={`${role}${JSON.stringify(p.devices!.roles[role]?.binding)}`} role={role} label={t(`climate.sensor.${kind}` as TextKey)} devices={p.devices!} reload={p.reload} />
              ) : null;
            })}
          </>
        )}
      </div>
    </Modal>
  );
}

// A value against its target
function Air(p: { label: string; value: string | null; target: string | null; testid: string }) {
  return (
    <div class="stack-sm" style="gap:2px" data-testid={p.testid}>
      <span class="muted small">{p.label}</span>
      <strong>{p.value ?? t("climate.noValue")}</strong>
      <span class="muted small">{p.target ?? t("climate.noTarget")}</span>
    </div>
  );
}

function RoomCard(p: { r: ClimateRoom; s: Status | undefined; onEdit: () => void }) {
  const cfg = config.value!;
  const s = p.s;
  const name = p.r.name;
  const firstLight = p.r.lights.length > 0 ? cfg.lighting.circuits.find((l) => l.id === p.r.lights[0]) : undefined;
  const release = async () => {
    try {
      await post(`/latches/climate.${p.r.id}.co2/ack`);
      await refreshState();
      toast(t("climate.released"));
    } catch (e) {
      toastError(e);
    }
  };
  const tg = s?.targets;
  const sensorName = (k: string) => t(`climate.sensor.${k}` as TextKey);
  return (
    <div class="item stacked" data-testid={`room-${p.r.id}`}>
      <div class="stack-sm" style="gap:4px">
        <h3 style="margin:0;font-size:1rem">{name}</h3>
        <span class="row small" style="flex-wrap:wrap" data-testid={`room-state-${p.r.id}`}>
          {s && <Pill tone="neutral">{t(s.day ? "climate.day" : "climate.night")}</Pill>}
          {s?.plant && <span class="muted">{s.plant.phase != null ? t("climate.follows", { plant: s.plant.name, phase: s.plant.phase }) : t("climate.followsDone", { plant: s.plant.name })}</span>}
          <span class="muted">
            {firstLight ? t("climate.dayFrom", { light: firstLight.name }) : t("climate.dayOwn", { from: hhmm(p.r.startMin), to: hhmm((p.r.startMin + Math.round(p.r.hours * 60)) % 1440) })}
          </span>
        </span>
      </div>
      {s && s.missing.length > 0 && <Banner tone="warn">{t("climate.missing", { list: s.missing.map(sensorName).join(", ") })}</Banner>}
      {s?.heat && <Banner tone="warn">{s.heat.off ? t("climate.heatOff") : t("climate.heatDim", { pct: `${figure(s.heat.pct)}${nb}%` })}</Banner>}
      {s?.co2High && <Banner tone="bad">{t("climate.co2High")}</Banner>}
      {s?.co2Locked && (
        <Banner tone="bad">
          {t("climate.co2Locked")}{" "}
          <Button size="sm" onClick={release} data-testid={`room-release-${p.r.id}`}>
            {t("climate.release")}
          </Button>
        </Banner>
      )}
      <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(7.5rem,1fr));gap:12px" data-testid={`room-air-${p.r.id}`}>
        <Air label={t("climate.temp")} value={unit(s?.air.tempC, "°C")} target={tg?.tempC != null ? t("climate.target", { value: unit(tg.tempC, "°C") }) : null} testid={`room-temp-${p.r.id}`} />
        <Air
          label={t("climate.rh")}
          value={unit(s?.air.rhPct, "%")}
          target={tg?.rhPct != null ? (s?.byVpd && tg.vpdKpa != null ? t("climate.targetVpd", { value: unit(tg.rhPct, "%"), vpd: unit(tg.vpdKpa, "kPa") }) : t("climate.target", { value: unit(tg.rhPct, "%") })) : null}
          testid={`room-rh-${p.r.id}`}
        />
        <Air label={t("climate.vpd")} value={unit(s?.air.vpdKpa, "kPa")} target={tg?.vpdKpa != null ? t("climate.target", { value: unit(tg.vpdKpa, "kPa") }) : null} testid={`room-vpd-${p.r.id}`} />
        <Air label={t("climate.co2")} value={unit(s?.air.co2Ppm, "ppm")} target={tg?.co2Ppm != null ? t("climate.target", { value: unit(tg.co2Ppm, "ppm") }) : null} testid={`room-co2-${p.r.id}`} />
      </div>
      <div class="row small" style="flex-wrap:wrap" data-testid={`room-devices-${p.r.id}`}>
        {CLIMATE_OUTPUTS.filter((k) => s?.outputs[k]?.output && s.outputs[k].output!.state !== "not_assigned").map((k) => {
          const o = s!.outputs[k];
          const st = o.output!.state;
          const tone = st === "on" ? "ok" : st === "off" || st === "checking" || st === "waiting" ? "neutral" : "bad";
          return (
            <Pill tone={tone}>
              {t(`climate.out.${k}` as TextKey)}: {st === "on" || st === "off" ? t(st === "on" ? "climate.on" : "climate.off") : t(`settings.devices.state.${st}` as TextKey)}
            </Pill>
          );
        })}
      </div>
      <div class="row">
        <span class="grow" />
        <Button size="sm" variant="ghost" onClick={p.onEdit} aria-label={t("climate.editAria", { name })} data-testid={`room-edit-${p.r.id}`}>
          {t("common.edit")}
        </Button>
      </div>
    </div>
  );
}

export function ClimatePage() {
  const cfg = config.value!;
  const [devices, setDevices] = useState<Devices | null>(null);
  const [editing, setEditing] = useState<ClimateRoom | "new" | null>(null);
  const load = () => get<Devices>("/devices").then(setDevices).catch(() => {});
  useEffect(() => {
    // Rooms changed elsewhere, by another browser or the app, show too
    const poll = () => {
      load();
      void refreshConfig().catch(() => {});
    };
    poll();
    const id = window.setInterval(poll, 5000);
    return () => window.clearInterval(id);
  }, []);
  if (!state.value) return null;
  const rooms = cfg.climate.rooms;
  const status = state.value.climate ?? [];
  const noShelly = devices != null && !devices.devices.some((d) => d.kind === "shelly");
  return (
    <div class="stack">
      <Card
        title={t("climate.rooms")}
        actions={
          <Button size="sm" variant="primary" onClick={() => setEditing("new")} data-testid="add-room">
            <Plus size={16} />
            {t("climate.add")}
          </Button>
        }
      >
        <p class="muted small">{t("climate.intro")}</p>
        <div class="list" data-testid="rooms">
          {rooms.length === 0 &&
            (noShelly ? (
              <p class="muted">
                {t("climate.noneNoShelly")} <ToDevices />
              </p>
            ) : (
              <p class="muted">{t("climate.none")}</p>
            ))}
          {rooms.map((r) => (
            <RoomCard key={r.id} r={r} s={status.find((x) => x.id === r.id)} onEdit={() => setEditing(r)} />
          ))}
        </div>
      </Card>
      {editing && <RoomForm room={editing === "new" ? null : editing} devices={devices} reload={load} onClose={() => setEditing(null)} />}
    </div>
  );
}
