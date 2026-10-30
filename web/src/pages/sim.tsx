// SPDX-License-Identifier: AGPL-3.0-or-later
// The simulator's panel: time-lapse, the tank as it truly is, faults and
// scenarios, to try the cycle and its texts without hardware. Simulator only.
import { useEffect, useState } from "preact/hooks";
import { FlaskConical } from "lucide-preact";
import { get, sim } from "../api";
import { num } from "../format";
import { t, type TextKey } from "../i18n";
import { logoutLocal, refreshState, state, toast, toastError } from "../store";
import { Button, Field, NumberInput, Seg } from "../ui";

type World = {
  tank: { volumeL: number; ec: number; ph: number; temp: number; pendingEc: number; pendingPh: number };
  channels: {
    channel: number; liquid: string; head: string; outletBlocked: boolean; siphon: boolean; drops: number; lineMl: number; trueFlow: number; blocked: boolean; empty: boolean; state: string; cupMl: number;
    intake: "canister" | "water"; light: boolean; lineHolds: "air" | "concentrate" | "water";
  }[];
  calWater: { ml: number; cylinderMl: number; scaleG: number };
  output: boolean; pumpLpm: number; flowing: boolean; flowFault: string; probeFault: string;
  primed: boolean; outletWet: boolean; chamberL: number; highLevel: boolean; returnByPump: boolean; returnPump: boolean;
  returnFault: string; leak: boolean; stopButton: boolean; released: boolean; cableFault: boolean; faults: string[];
  phBuffer: number | null; ecBuffer: number | null; liquids: Record<string, string>;
};

type Channel = World["channels"][number];
// What the device's protection can report (SD-141, SD-146), by the IDs the firmware uses
const HW_FAULTS = [
  "self_test.switch_off", "board_description", "power_trip.dosing", "power_trip.pump", "power_trip.return", "safety_cycle",
  "supply_overvoltage_latched", "supply", "current_limit.dosing", "pump_protection.pump", "pump_protection.return", "stop_mismatch",
  "not_off", "leak_sensor_open", "moisture", "sensor_supply", "flow_implausible", "board_hot", "safety_restart",
];
// A channel's faults as they are, so a button changes only its own
const faults = (p: Channel) => ({ channel: p.channel, blocked: p.blocked, empty: p.empty, outletBlocked: p.outletBlocked, siphon: p.siphon });

/** The person's hands in a pump calibration (SD-143): an intake into the water or back, and what the
 *  cylinder and the scale read */
function PumpCalTwin(p: { w: World; act: (action: string, body?: object) => Promise<void> }) {
  const pc = state.value?.pumpCalibration;
  const n = pc && !["done", "failed", "cancelled"].includes(pc.step) ? pc.channel : null;
  const ch = n ? p.w.channels[n - 1] : null;
  return (
    <div class="stack-sm">
      <div class="section-title">{t("sim.pumpCal")}</div>
      <p class="small muted">
        {t("sim.calWater", { cylinder: num(p.w.calWater.cylinderMl, 1), scale: num(p.w.calWater.scaleG, 1) })}
        {ch && ` · ${t("sim.calLine", { channel: n!, holds: t(`sim.line.${ch.lineHolds}` as TextKey) })}`}
      </p>
      <div class="row">
        {ch && (
          <Button size="sm" onClick={() => p.act("intake", { channel: n, where: ch.intake === "water" ? "canister" : "water" })} data-testid="sim-intake">
            {t(ch.intake === "water" ? "sim.intakeToCanister" : "sim.intakeToWater", { channel: n! })}
          </Button>
        )}
        <Button size="sm" variant="ghost" onClick={() => p.act("calWater", { ml: 100 })}>
          {t("sim.calRefill")}
        </Button>
      </div>
    </div>
  );
}

export function SimPanel() {
  const [open, setOpen] = useState(false);
  const [world, setWorld] = useState<World | null>(null);
  const [vol, setVol] = useState<number | null>(100);
  const [ec, setEc] = useState<number | null>(0.45);
  const [ph, setPh] = useState<number | null>(7.6);
  const [lpm, setLpm] = useState<number | null>(12);
  const speed = String(state.value?.sim?.speed ?? 1);

  const load = () =>
    get<{ world: World }>("/sim")
      .then((s) => setWorld(s.world))
      .catch(() => {});
  useEffect(() => {
    if (!open) return;
    load();
    const timer = setInterval(load, 2000);
    return () => clearInterval(timer);
  }, [open]);

  const act = async (action: string, body: unknown = {}, done?: string) => {
    try {
      await sim(action, body);
      await load();
      await refreshState();
      if (done) toast(done, "info");
    } catch (e) {
      toastError(e);
    }
  };

  if (!open)
    return (
      <button class="btn sim-fab" onClick={() => setOpen(true)} data-testid="sim-open">
        <FlaskConical size={16} /> {t("common.simulator")}
      </button>
    );
  const w = world;
  return (
    <aside class="drawer" aria-label={t("common.simulator")}>
      <div class="row-between">
        <h2 class="row">
          <FlaskConical size={18} /> {t("common.simulator")}
        </h2>
        <button class="btn sm ghost" onClick={() => setOpen(false)}>
          {t("common.close")}
        </button>
      </div>
      <p class="muted small">{t("sim.intro")}</p>
      <Field label={t("sim.speed")} group>
        <Seg value={speed} onChange={(v) => act("speed", { speed: Number(v) })} options={[["1", "1×"], ["10", "10×"], ["60", "60×"], ["300", "300×"]]} />
      </Field>
      {w && (
        <div class="card flat stack-sm" data-testid="sim-world">
          <div class="section-title">{t("sim.tankTrue")}</div>
          <div class="row small">
            <span>{num(w.tank.volumeL, 1)} L</span>·<span>EC {num(w.tank.ec + w.tank.pendingEc, 2)}</span>·<span>pH {num(w.tank.ph + w.tank.pendingPh, 2)}</span>·
            <span>{num(w.tank.temp, 1)} °C</span>
          </div>
          <div class="small muted">
            {t("sim.feedPump", { state: w.output ? t("common.on") : t("common.off") })}
            {w.output && !w.primed ? ` (${t("sim.priming")})` : ""} · {w.flowing ? t("sim.flowing", { lpm: num(w.pumpLpm, 1) }) : t("sim.notFlowing")}
          </div>
          <div class="small muted" data-testid="sim-water-path">
            {t("sim.chamber", { l: num(w.chamberL, 2) })} · {w.outletWet ? t("sim.outletWet") : t("sim.outletDry")} ·{" "}
            {w.returnByPump ? t("sim.returnPump", { state: w.returnPump ? t("common.on") : t("common.off") }) : t("sim.returnGravity")}
          </div>
          <div class={`small ${w.released ? "muted" : "warn-text"}`} data-testid="sim-release">
            {w.released
              ? t("sim.releaseOn")
              : t("sim.releaseOff", {
                  why: [w.leak && t("sim.why.leak"), w.highLevel && t("sim.why.highLevel"), w.stopButton && t("sim.why.stopButton")].filter(Boolean).join(", "),
                })}
          </div>
        </div>
      )}
      <div class="stack-sm">
        <div class="section-title">{t("sim.fill")}</div>
        <div class="form-grid" style="grid-template-columns:repeat(3,minmax(0,1fr))">
          <NumberInput value={vol} onValue={setVol} unit="L" ariaLabel={t("sim.litres")} />
          <NumberInput value={ec} onValue={setEc} unit="EC" ariaLabel="EC" />
          <NumberInput value={ph} onValue={setPh} unit="pH" ariaLabel="pH" />
        </div>
        <div class="row">
          <Button size="sm" onClick={() => act("fill", { volumeL: vol, ec, ph }, t("sim.filled"))}>
            {t("sim.freshFill")}
          </Button>
          <Button size="sm" variant="ghost" onClick={() => act("topup", { litres: 10 }, t("sim.toppedUp"))}>
            {t("sim.topUp")}
          </Button>
        </div>
      </div>
      <div class="stack-sm">
        <div class="section-title">{t("sim.flow")}</div>
        <div class="row">
          <NumberInput value={lpm} onValue={setLpm} unit="L/min" ariaLabel={t("sim.pumpLpm")} />
          <Button size="sm" onClick={() => act("pumpLpm", { lpm })}>
            {t("sim.set")}
          </Button>
        </div>
        <Seg
          value={w?.flowFault || "none"}
          onChange={(f) => act("flowFault", { fault: f === "none" ? "" : f })}
          label={t("sim.flowFault")}
          options={[
            ["none", t("sim.fault.none")],
            ["blocked", t("sim.fault.blocked")],
            ["dry", t("sim.fault.dry")],
            ["hose", t("sim.fault.hose")],
            ["sensor", t("sim.fault.sensor")],
            ["air", t("sim.fault.air")],
          ]}
        />
      </div>
      <div class="stack-sm">
        <div class="section-title">{t("sim.return")}</div>
        <Seg
          value={w?.returnFault || "none"}
          onChange={(f) => act("returnFault", { fault: f === "none" ? "" : f })}
          label={t("sim.returnFault")}
          options={[
            ["none", t("sim.fault.none")],
            w?.returnByPump ? ["failed", t("sim.fault.returnFailed")] : ["blocked", t("sim.fault.returnBlocked")],
          ]}
        />
        <label class="row small">
          <input type="checkbox" checked={!!w?.leak} onChange={(e) => act("leak", { on: (e.target as HTMLInputElement).checked })} data-testid="sim-leak" /> {t("sim.leak")}
        </label>
        <label class="row small">
          <input type="checkbox" checked={!!w?.cableFault} onChange={(e) => act("cableFault", { on: (e.target as HTMLInputElement).checked })} data-testid="sim-cable" /> {t("sim.cableFault")}
        </label>
        <label class="row small">
          <input type="checkbox" checked={!!w?.stopButton} onChange={(e) => act("stopButton", { pressed: (e.target as HTMLInputElement).checked })} /> {t("sim.stopButton")}
        </label>
        <Field label={t("sim.hwFault")}>
          <select
            value={w?.faults?.[0] ?? ""}
            onChange={(e) => {
              const v = (e.target as HTMLSelectElement).value;
              act("faults", { ids: v ? [v] : [] });
            }}
            data-testid="sim-hw-fault"
          >
            <option value="">{t("sim.fault.none")}</option>
            {HW_FAULTS.map((f) => (
              <option value={f} key={f}>
                {f}
              </option>
            ))}
          </select>
        </Field>
      </div>
      <div class="stack-sm">
        <div class="section-title">{t("sim.probes")}</div>
        <Seg
          value={w?.probeFault || "none"}
          onChange={(f) => act("probeFault", { fault: f === "none" ? "" : f })}
          label={t("sim.probes")}
          options={([["none", "sim.fault.none"], ["offline", "sim.fault.offline"], ["frozen", "sim.fault.frozen"], ["jump", "sim.fault.jump"], ["ec_zero", "sim.fault.ecZero"]] as [string, TextKey][]).map(
            ([v, k]) => [v, t(k)] as [string, string],
          )}
        />
        <div class="row">
          <span class="small muted">{t("sim.buffer")}</span>
          {[7, 4].map((v) => (
            <Button size="sm" onClick={() => act("buffer", { kind: "ph", value: v })}>
              pH {v}
            </Button>
          ))}
          <Button size="sm" onClick={() => act("buffer", { kind: "ec", value: 1.413 })}>
            {num(1.413, 3)} mS/cm
          </Button>
          <Button
            size="sm"
            variant="ghost"
            onClick={async () => {
              await act("buffer", { kind: "ph" });
              await act("buffer", { kind: "ec" });
            }}
          >
            {t("sim.backInTank")}
          </Button>
        </div>
        {w && (w.phBuffer !== null || w.ecBuffer !== null) && <p class="small warn-text">{t("sim.inBuffer")}</p>}
      </div>
      {w && <PumpCalTwin w={w} act={act} />}
      {w && (
        <div class="stack-sm">
          <div class="section-title">{t("sim.channels")}</div>
          <div class="list">
            {w.channels.map((p) => (
              <div class="item small">
                <span class="grow">
                  {t("sim.channelLiquid", { n: p.channel, liquid: w.liquids[p.liquid] ?? p.liquid ?? "–" })} <span class="faint">{t("sim.trueFlow", { flow: num(p.trueFlow, 1) })}</span>
                  {p.cupMl > 0 && <span class="faint"> · {t("sim.cup", { ml: num(p.cupMl, 1) })}</span>}
                  <span class="faint"> · {t("sim.drops", { n: p.drops })}</span>
                </span>
                <select
                  class="select input"
                  style="width:auto"
                  aria-label={t("sim.head", { n: p.channel })}
                  value={p.head}
                  onChange={(e) => act("channelHead", { channel: p.channel, head: (e.target as HTMLSelectElement).value })}
                  data-testid={`sim-head-${p.channel}`}
                >
                  {["S", "M", "L"].map((h) => (
                    <option value={h}>{t("sim.headSize", { head: h })}</option>
                  ))}
                  <option value="">{t("sim.noHead")}</option>
                </select>
                <Button size="sm" onClick={() => act("channelFault", { ...faults(p), blocked: !p.blocked })}>
                  {p.blocked ? t("sim.unblock") : t("sim.block")}
                </Button>
                <Button size="sm" variant="ghost" onClick={() => act("channelFault", { ...faults(p), empty: !p.empty })}>
                  {p.empty ? t("sim.refill") : t("sim.empty")}
                </Button>
                <Button size="sm" variant="ghost" onClick={() => act("channelFault", { ...faults(p), outletBlocked: !p.outletBlocked })}>
                  {p.outletBlocked ? t("sim.outletClear") : t("sim.outletBlock")}
                </Button>
                <Button size="sm" variant="ghost" onClick={() => act("channelFault", { ...faults(p), siphon: !p.siphon })}>
                  {p.siphon ? t("sim.siphonOff") : t("sim.siphonOn")}
                </Button>
                <Button size="sm" variant="ghost" onClick={() => act("pumpWear", { channel: p.channel, factor: 0.8 })} title={t("sim.wearHint")}>
                  {t("sim.wear")}
                </Button>
              </div>
            ))}
          </div>
        </div>
      )}
      <div class="stack-sm">
        <div class="section-title">{t("sim.time")}</div>
        <div class="row">
          <Button size="sm" onClick={() => act("fastForward", { hours: 1 }, t("sim.forwarded", { h: 1 }))}>
            {t("sim.forward", { h: 1 })}
          </Button>
          <Button size="sm" onClick={() => act("fastForward", { hours: 6 }, t("sim.forwarded", { h: 6 }))}>
            {t("sim.forward", { h: 6 })}
          </Button>
          <Button size="sm" variant="danger-soft" onClick={() => act("reboot", { outageS: 60 }, t("sim.powerCut"))}>
            {t("sim.reboot")}
          </Button>
        </div>
      </div>
      <div class="stack-sm">
        <div class="section-title">{t("sim.atDevice")}</div>
        <p class="faint small">{t("sim.atDeviceNote")}</p>
        <div class="row">
          {(["access", "factory"] as const).map((r) => (
            <Button
              size="sm"
              variant="danger-soft"
              data-testid={`sim-reset-${r}`}
              onClick={async () => {
                if (!confirm(t(r === "access" ? "sim.accessResetAsk" : "sim.factoryResetAsk"))) return;
                await act("atDevice", { reset: r });
                logoutLocal();
                location.reload();
              }}
            >
              {t(r === "access" ? "sim.accessReset" : "sim.factoryReset")}
            </Button>
          ))}
        </div>
      </div>
      <div class="stack-sm">
        <div class="section-title">{t("sim.scenario")}</div>
        <p class="faint small">{t("sim.scenarioNote")}</p>
        <div class="row">
          {([["new", "sim.scenarioNew"], ["demo", "sim.scenarioDemo"]] as [string, TextKey][]).map(([n, l]) => (
            <Button
              size="sm"
              onClick={async () => {
                await act("scenario", { name: n }, t("sim.scenarioLoaded", { name: t(l) }));
                logoutLocal();
                location.reload();
              }}
            >
              {t(l)}
            </Button>
          ))}
        </div>
      </div>
    </aside>
  );
}
