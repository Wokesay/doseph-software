// SPDX-License-Identifier: AGPL-3.0-or-later
// Parts several pages show: a reading's tile and the list of events.
import { AlertOctagon, AlertTriangle, Bell, Info } from "lucide-preact";
import type { Quality, Reading, RobotEvent } from "./api";
import { Sparkline } from "./chart";
import { ago, day, num, time } from "./format";
import { msg, t, type TextKey } from "./i18n";
import { ErrorBoundary, Pill } from "./ui";

const QUALITY: Record<Quality, TextKey> = {
  offline: "widgets.quality.offline",
  no_data: "widgets.quality.noData",
  stale: "widgets.quality.stale",
  frozen: "widgets.quality.frozen",
  implausible: "widgets.quality.implausible",
  jump: "widgets.quality.jump",
  uncalibrated: "widgets.quality.uncalibrated",
  ok: "widgets.quality.ok",
};

export function MetricTile(p: { id: string; label: string; reading?: Reading; color: string; band?: [number, number] | null; spark?: (number | null)[]; note?: string }) {
  const r = p.reading;
  const bad = !!r && r.quality !== "ok";
  const outOfBand = r?.usable && p.band && r.value !== null && (r.value < p.band[0] || r.value > p.band[1]);
  return (
    <div class={`metric ${bad ? "bad" : ""}`} data-testid={`metric-${p.id}`}>
      <div class="metric-top">
        <span class="metric-label">
          <span class="dot" style={`color:var(${p.color})`} />
          {p.label}
        </span>
        {bad ? (
          <Pill tone={r!.quality === "uncalibrated" ? "warn" : "bad"} title={msg(r!.reason)}>
            {t(QUALITY[r!.quality] ?? "widgets.quality.offline")}
          </Pill>
        ) : outOfBand ? (
          <Pill tone="warn">{t("widgets.outOfBand")}</Pill>
        ) : null}
      </div>
      <div class="metric-value">
        {num(r?.value ?? null, r?.decimals ?? 2)}
        {r?.unit && <span class="unit">{r.unit}</span>}
      </div>
      {p.spark && !bad && <Sparkline values={p.spark} color={p.color} band={p.band} />}
      {bad && r?.reason.text && <div class="metric-reason">{msg(r.reason)}</div>}
      {p.note && <div class="metric-note">{p.note}</div>}
      <div class="metric-foot">
        <span>{p.band ? t("widgets.target", { lo: num(p.band[0], 2), hi: num(p.band[1], 2) }) : ""}</span>
        {!bad && r?.ageS != null && <span>{ago(r.ageS)}</span>}
      </div>
    </div>
  );
}

const sevIcon = (s: RobotEvent["severity"]) =>
  s === "alarm" ? <AlertOctagon size={16} /> : s === "warn" ? <AlertTriangle size={16} /> : s === "notice" ? <Bell size={16} /> : <Info size={16} />;

export function EventList(p: { events: RobotEvent[]; groupByDay?: boolean }) {
  return (
    <ErrorBoundary>
      <Events {...p} />
    </ErrorBoundary>
  );
}

function Events(p: { events: RobotEvent[]; groupByDay?: boolean }) {
  if (!p.events.length) return <p class="muted">{t("widgets.noEvents")}</p>;
  let lastDay = "";
  return (
    <div>
      {p.events.map((e) => {
        const d = day(e.ts);
        const head = p.groupByDay && d !== lastDay ? <div class="day-head">{d}</div> : null;
        lastDay = d;
        return (
          <>
            {head}
            <div class="event" data-testid="event">
              <span class="t">{time(e.ts)}</span>
              <span class={`sev ${e.severity}`}>{sevIcon(e.severity)}</span>
              <div>
                <div class="et">{msg(e.title)}</div>
                {msg(e.text) && <div class="ex">{msg(e.text)}</div>}
              </div>
            </div>
          </>
        );
      })}
    </div>
  );
}
