// SPDX-License-Identifier: AGPL-3.0-or-later
// Charts: uPlot for histories (small, fast, MIT), an SVG sparkline for tiles.
import { useEffect, useRef } from "preact/hooks";
import uPlot from "uplot";
import "uplot/dist/uPlot.min.css";
import type { SeriesData } from "./api";
import { locale, t } from "./i18n";

function cssVar(name: string) {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim() || "#888";
}

export type Marker = { t: number; label: string; color?: string };

export function TimeChart(p: {
  data: SeriesData | null;
  color: string; // a CSS variable, e.g. "--ph"
  label: string;
  unit?: string;
  decimals?: number;
  band?: [number, number] | null; // the target band
  markers?: Marker[];
  height?: number;
}) {
  const ref = useRef<HTMLDivElement>(null);
  const plot = useRef<uPlot | null>(null);

  useEffect(() => {
    const el = ref.current;
    if (!el || !p.data || p.data.t.length === 0) return;
    const color = cssVar(p.color);
    const grid = cssVar("--border");
    const text = cssVar("--faint");
    const showMinMax = p.data.stepS >= 60;
    const xs = p.data.t;
    const avg = p.data.avg.map((v) => (v === null ? null : v));
    const mn = p.data.min.map((v) => (v === null ? null : v));
    const mx = p.data.max.map((v) => (v === null ? null : v));
    const dec = p.decimals ?? 2;
    const band = p.band;
    const markers = p.markers ?? [];
    const opts: uPlot.Options = {
      width: el.clientWidth,
      height: p.height ?? 220,
      padding: [10, 8, 0, 0],
      cursor: { y: false, points: { size: 6 } },
      legend: { show: true, live: true },
      scales: { x: { time: true } },
      axes: [
        {
          stroke: text,
          grid: { stroke: grid, width: 1 },
          ticks: { stroke: grid },
          // In the page's language and 24-hour clock, not uPlot's US default. Ticks a day or
          // more apart show the date; closer ones the time, and the date at midnight.
          values: (_u, vals, _axis, _space, incr) =>
            vals.map((v) => {
              const d = new Date(v * 1000);
              return incr >= 86400 || (d.getHours() === 0 && d.getMinutes() === 0)
                ? d.toLocaleDateString(locale(), { day: "2-digit", month: "2-digit" })
                : d.toLocaleTimeString(locale(), { hour: "2-digit", minute: "2-digit" });
            }),
        },
        {
          stroke: text,
          grid: { stroke: grid, width: 1 },
          ticks: { stroke: grid },
          size: 52,
          values: (_u, vals) => vals.map((v) => v.toLocaleString(locale(), { maximumFractionDigits: dec })),
        },
      ],
      series: [
        { label: t("widgets.chartTime"), value: (_u, v) => (v ? new Date(v * 1000).toLocaleString(locale(), { day: "2-digit", month: "2-digit", hour: "2-digit", minute: "2-digit" }) : "–") },
        {
          label: p.label,
          stroke: color,
          width: 2,
          spanGaps: false,
          value: (_u, v) => (v === null || v === undefined ? "–" : `${v.toLocaleString(locale(), { minimumFractionDigits: dec, maximumFractionDigits: dec })} ${p.unit ?? ""}`),
        },
        ...(showMinMax
          ? [
              { label: t("widgets.chartMin"), stroke: color, width: 0, show: true, points: { show: false }, value: () => "" },
              { label: t("widgets.chartMax"), stroke: color, width: 0, show: true, points: { show: false }, value: () => "" },
            ]
          : []),
      ],
      bands: showMinMax ? [{ series: [3, 2], fill: `${color}22` }] : [],
      hooks: {
        drawClear: [
          (u) => {
            if (!band) return;
            const ctx = u.ctx;
            const y0 = u.valToPos(band[1], "y", true);
            const y1 = u.valToPos(band[0], "y", true);
            ctx.save();
            ctx.fillStyle = `${cssVar("--ok")}18`;
            ctx.fillRect(u.bbox.left, y0, u.bbox.width, y1 - y0);
            ctx.restore();
          },
        ],
        draw: [
          (u) => {
            const ctx = u.ctx;
            ctx.save();
            for (const m of markers) {
              if (m.t < (u.scales.x.min ?? 0) || m.t > (u.scales.x.max ?? 0)) continue;
              const x = u.valToPos(m.t, "x", true);
              ctx.strokeStyle = m.color ? cssVar(m.color) : cssVar("--faint");
              ctx.globalAlpha = 0.55;
              ctx.setLineDash([3, 3]);
              ctx.beginPath();
              ctx.moveTo(x, u.bbox.top);
              ctx.lineTo(x, u.bbox.top + u.bbox.height);
              ctx.stroke();
            }
            ctx.restore();
          },
        ],
      },
    };
    const data: uPlot.AlignedData = showMinMax ? [xs, avg, mn, mx] : [xs, avg];
    plot.current?.destroy();
    plot.current = new uPlot(opts, data, el);
    const ro = new ResizeObserver(() => plot.current?.setSize({ width: el.clientWidth, height: p.height ?? 220 }));
    ro.observe(el);
    return () => {
      ro.disconnect();
      plot.current?.destroy();
      plot.current = null;
    };
  }, [p.data, p.band?.[0], p.band?.[1], p.markers?.length]);

  if (!p.data || p.data.t.length === 0 || p.data.avg.every((v) => v === null))
    return <div class="chart-empty">{t("widgets.chartEmpty")}</div>;
  return <div class="chart" ref={ref} />;
}

export function Sparkline(p: { values: (number | null)[]; color: string; band?: [number, number] | null }) {
  const vals = p.values.filter((v): v is number => v !== null);
  if (vals.length < 2) return <svg class="spark" />;
  let lo = Math.min(...vals);
  let hi = Math.max(...vals);
  if (p.band) {
    lo = Math.min(lo, p.band[0]);
    hi = Math.max(hi, p.band[1]);
  }
  const pad = (hi - lo) * 0.1 || 0.1;
  lo -= pad;
  hi += pad;
  const w = 200;
  const h = 34;
  const n = p.values.length;
  const y = (v: number) => h - ((v - lo) / (hi - lo)) * h;
  let d = "";
  p.values.forEach((v, i) => {
    if (v === null) return;
    const x = (i / (n - 1)) * w;
    d += `${d && p.values[i - 1] !== null ? "L" : "M"}${x.toFixed(1)},${y(v).toFixed(1)}`;
  });
  return (
    <svg class="spark" viewBox={`0 0 ${w} ${h}`} preserveAspectRatio="none" aria-hidden="true">
      {p.band && <rect x="0" width={w} y={y(p.band[1])} height={Math.max(0, y(p.band[0]) - y(p.band[1]))} fill={`var(--ok)`} opacity="0.12" />}
      <path d={d} fill="none" stroke={`var(${p.color})`} stroke-width="2" vector-effect="non-scaling-stroke" stroke-linejoin="round" />
    </svg>
  );
}
