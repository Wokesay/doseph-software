// SPDX-License-Identifier: AGPL-3.0-or-later
// Formatting in the page language. A missing value shows as "–", never as 0.
import { locale, t } from "./i18n";

const nf = new Map<string, Intl.NumberFormat>();
/** A number as another program shows it: up to `decimals` places, no trailing zeros. */
export function numUpTo(v: number | null | undefined, decimals = 2): string {
  if (v === null || v === undefined || !Number.isFinite(v)) return "–";
  return new Intl.NumberFormat(locale(), { minimumFractionDigits: 0, maximumFractionDigits: decimals }).format(v);
}

export function num(v: number | null | undefined, decimals = 1): string {
  if (v === null || v === undefined || !Number.isFinite(v)) return "–";
  const key = `${locale()}:${decimals}`;
  let f = nf.get(key);
  if (!f) {
    f = new Intl.NumberFormat(locale(), { minimumFractionDigits: decimals, maximumFractionDigits: decimals });
    nf.set(key, f);
  }
  // -0.3 rounds to "-0", which is no value of its own; drop the sign then
  return f.format(v).replace(/^[-\u2212](0(?:[.,]0+)?)$/, "$1");
}

// A plain number in the language of the page (5,8 / 5.8), with as many
// decimals as it has (gc PD-035).
export function figure(v: number): string {
  // No thousands separator: a field would read "1.000" or "1,000" as 1.
  return v.toLocaleString(locale(), { maximumFractionDigits: 6, useGrouping: false });
}

export function ago(seconds: number | null | undefined): string {
  if (seconds === null || seconds === undefined) return "";
  if (seconds < 5) return t("common.justNow");
  if (seconds < 60) return t("common.secondsAgo", { n: Math.round(seconds) });
  if (seconds < 3600) return t("common.minutesAgo", { n: Math.round(seconds / 60) });
  if (seconds < 86400) return t("common.hoursAgo", { n: Math.round(seconds / 3600) });
  return t("common.daysAgo", { n: Math.round(seconds / 86400) });
}

// Number and unit are joined by a no-break space, so a line never breaks between them (gc#106)
export function duration(ms: number): string {
  const s = Math.max(0, Math.round(ms / 1000));
  const nb = "\u00a0";
  if (s < 60) return `${s}${nb}s`;
  const m = Math.floor(s / 60);
  const r = s % 60;
  if (m < 60) return r ? `${m}${nb}min ${r}${nb}s` : `${m}${nb}min`;
  return `${Math.floor(m / 60)}${nb}h ${m % 60}${nb}min`;
}

const dtfs = new Map<string, Intl.DateTimeFormat>();
function fmt(kind: "dt" | "t" | "d") {
  const key = `${locale()}:${kind}`;
  let f = dtfs.get(key);
  if (!f) {
    const opts: Intl.DateTimeFormatOptions =
      kind === "dt" ? { day: "2-digit", month: "2-digit", hour: "2-digit", minute: "2-digit" } : kind === "t" ? { hour: "2-digit", minute: "2-digit" } : { weekday: "long", day: "numeric", month: "long" };
    f = new Intl.DateTimeFormat(locale(), opts);
    dtfs.set(key, f);
  }
  return f;
}
export const dateTime = (epoch: number) => fmt("dt").format(new Date(epoch * 1000));
export const time = (epoch: number) => fmt("t").format(new Date(epoch * 1000));
export const day = (epoch: number) => fmt("d").format(new Date(epoch * 1000));

export function plural(n: number, one: string, many: string) {
  return `${n} ${n === 1 ? one : many}`;
}

// The form the robot accepts (isTimeZoneName in core/src/config.cpp): newer browsers also know
// offsets such as "+01:00", which the robot refuses
const ZONE_FORM = /^[A-Za-z](?:[A-Za-z0-9_+-]|\/(?=[A-Za-z0-9_+-]))*$/;

/** A time zone the browser knows, in its usual spelling ("europe/berlin" → "Europe/Berlin"), in the
 *  form the robot accepts; null if there is none of that name. */
export function timeZone(name: string): string | null {
  const z = name.trim();
  if (!z) return null;
  try {
    const known = new Intl.DateTimeFormat("en", { timeZone: z }).resolvedOptions().timeZone;
    return known.length <= 64 && ZONE_FORM.test(known) ? known : null;
  } catch {
    return null;
  }
}
