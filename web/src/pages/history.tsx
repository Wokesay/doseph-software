// SPDX-License-Identifier: AGPL-3.0-or-later
// History: pH, EC, water temperature and flow with the target band and the
// doses marked, and the events.
import { useEffect, useState } from "preact/hooks";
import { Download, History, LineChart } from "lucide-preact";
import { get, type RobotEvent, type SeriesData } from "../api";
import { TimeChart, type Marker } from "../chart";
import { lang, msg, t, type TextKey } from "../i18n";
import { state } from "../store";
import { Card, Seg } from "../ui";
import { EventList } from "../widgets";

const RANGES: [string, TextKey, number][] = [
  ["6h", "history.range6h", 6 * 3600],
  ["24h", "history.range24h", 24 * 3600],
  ["7d", "history.range7d", 7 * 86400],
  ["30d", "history.range30d", 30 * 86400],
];

const FILTERS: [string, TextKey][] = [
  ["", "common.all"],
  ["dose", "history.filter.dose"],
  ["control", "history.filter.control"],
  ["alarm", "history.filter.alarm"],
  ["calibration", "history.filter.calibration"],
  ["config", "history.filter.config"],
  ["system", "history.filter.system"],
  ["auth", "history.filter.auth"],
];

export function HistoryPage() {
  const st = state.value!;
  const [range, setRange] = useState("24h");
  const [data, setData] = useState<Record<string, SeriesData>>({});
  const [doses, setDoses] = useState<RobotEvent[]>([]);
  const [events, setEvents] = useState<RobotEvent[]>([]);
  const [filter, setFilter] = useState("");
  const span = RANGES.find((r) => r[0] === range)![2];

  useEffect(() => {
    let alive = true;
    const load = async () => {
      const now = state.value?.now ?? Math.floor(Date.now() / 1000);
      const from = now - span;
      const [h, d] = await Promise.all([
        get<{ series: SeriesData[] }>(`/history?series=ph,ec,temp,flow&from=${from}&to=${now}&points=700`),
        get<RobotEvent[]>(`/events?type=dose&from=${from}&limit=1000`),
      ]);
      if (!alive) return;
      setData(Object.fromEntries(h.series.map((s) => [s.series, s])));
      setDoses(d);
    };
    load().catch(() => {});
    const timer = setInterval(() => load().catch(() => {}), 30000);
    return () => {
      alive = false;
      clearInterval(timer);
    };
  }, [range]);

  useEffect(() => {
    get<RobotEvent[]>(`/events?limit=150${filter ? `&type=${filter}` : ""}`).then(setEvents).catch(() => {});
  }, [filter, st.eventId]);

  const s = st.cycle.settings;
  const band = (target: number | null, tol: number | null): [number, number] | null => (target != null && tol != null ? [target - tol, target + tol] : null);
  const markers = (purpose: string): Marker[] =>
    doses.filter((e) => e.data?.purpose === purpose).map((e) => ({ t: e.ts, label: msg(e.title), color: purpose === "ph" ? "--ph" : "--ec" }));

  const now = st.now;
  const csv = `/api/v1/export.csv?series=ph,ec,temp,flow&from=${now - span}&to=${now}&lang=${lang.value}`;  // the page's format (RAT-016)

  return (
    <div class="stack">
      <div class="page-head">
        <p class="muted">{t("history.intro")}</p>
        <div class="row">
          <Seg value={range} onChange={setRange} options={RANGES.map(([v, l]) => [v, t(l)] as [string, string])} />
          <a class="btn sm" href={csv} download title={t("history.csvFormat")} data-testid="history-csv">
            <Download size={15} /> CSV
          </a>
        </div>
      </div>
      <div class="charts">
        <Card title="pH" icon={<LineChart size={18} />}>
          <TimeChart data={data.ph ?? null} color="--ph" label="pH" decimals={2} band={band(s.phTarget, s.phTolerance)} markers={markers("ph")} />
        </Card>
        <Card title="EC" icon={<LineChart size={18} />}>
          <TimeChart data={data.ec ?? null} color="--ec" label="EC" unit="mS/cm" decimals={2} band={band(s.ecTarget, s.ecTolerance)} markers={markers("ec")} />
        </Card>
        <Card title={t("status.temp")} icon={<LineChart size={18} />}>
          <TimeChart data={data.temp ?? null} color="--temp" label={t("status.temp")} unit="°C" decimals={1} />
        </Card>
        <Card title={t("status.flow")} icon={<LineChart size={18} />}>
          <TimeChart data={data.flow ?? null} color="--flow" label={t("status.flow")} unit="L/min" decimals={1} />
        </Card>
      </div>
      <p class="muted small">{t("history.onlyWithFlow")}</p>
      <Card title={t("history.events")} icon={<History size={18} />}>
        <div class="chips" style="margin-bottom:8px">
          {FILTERS.map(([v, l]) => (
            <button class={`chip ${filter === v ? "on" : ""}`} aria-pressed={filter === v} onClick={() => setFilter(v)}>
              {t(l)}
            </button>
          ))}
        </div>
        <EventList events={events} groupByDay />
      </Card>
    </div>
  );
}
