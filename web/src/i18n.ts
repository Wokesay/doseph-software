// SPDX-License-Identifier: AGPL-3.0-or-later
// Languages of the app: German and English. Texts live under fixed keys in
// lang/de.ts and lang/en.ts (base and one file per area); the English table
// must have every German key (checked by the compiler).
//
// Messages of the robot come as {key, text, args} with an English text:
// English shows that text, German fills the template from
// lang/msg.ts with the arguments and falls back to the robot's text.
import { signal } from "@preact/signals";
import { de } from "./lang/de";
import { en } from "./lang/en";
import { msgDe } from "./lang/msg";

export type Lang = "de" | "en";
export type TextKey = keyof typeof de;
type Vars = Record<string, unknown>;

const tables: Record<Lang, Record<string, string>> = { de, en };
const STORE = "dp.lang";

function initial(): Lang {
  try {
    const saved = localStorage.getItem(STORE);
    if (saved === "de" || saved === "en") return saved;
  } catch {
    // private mode: no saved choice
  }
  return "en";  // English first until a choice is made here or on the robot
}

export const lang = signal<Lang>(initial());

/** Has this browser a choice of its own? Otherwise the robot's language applies. */
export function hasOwnLang(): boolean {
  try {
    return localStorage.getItem(STORE) !== null;
  } catch {
    return false;
  }
}
document.documentElement.lang = lang.value;

/** Switches the language; `remember` keeps the choice in this browser. */
export function setLang(l: Lang, remember = true) {
  lang.value = l;
  document.documentElement.lang = l;
  if (!remember) return;
  try {
    localStorage.setItem(STORE, l);
  } catch {
    // without storage the choice lasts until the page reloads
  }
}

type Msg = { key?: string; text?: string; args?: Vars };

// {name} or {name:N}: a number in the page language, with N decimals if
// given, without a thousands separator; missing is "–", never 0 (R5); a
// message of the robot inside is shown in the page language too.
// Messages come from the robot's storage, so they are read defensively: only
// own keys of the tables, only strings as text, and nested messages and
// lists at most a few levels deep. A bad record shows "–", it never throws.
const own = (o: object, k: string) => Object.prototype.hasOwnProperty.call(o, k);
const maxDepth = 4;

function fill(text: string, vars?: Vars, depth = 0) {
  if (!vars) return text;
  return text.replace(/\{(\w+)(?::(\d))?\}/g, (_, k: string, d?: string) =>
    show(own(vars, k) ? vars[k] : undefined, d === undefined ? undefined : Number(d), depth),
  );
}

function show(v: unknown, decimals?: number, depth = 0): string {
  if (v === null || v === undefined || depth > maxDepth) return "–";
  if (typeof v === "number") {
    if (!Number.isFinite(v)) return "–";
    if (decimals === undefined) return v.toLocaleString(locale(), { maximumFractionDigits: 6, useGrouping: false });
    // toFixed rounds the exact value like the robot does, so both languages
    // show the same digits (20.65 → 20.6 / 20,6).
    const s = v.toFixed(decimals).replace(/^-(0\.?0*)$/, "$1");
    return lang.value === "de" ? s.replace(".", ",") : s;
  }
  if (Array.isArray(v)) return v.length ? v.map((x) => show(x, decimals, depth + 1)).join(", ") : "–";
  if (typeof v === "object") return msgAt(v, depth + 1) || "–";  // a nested message
  return typeof v === "string" ? v : "–";
}

/** The text of a key in the current language, with its {placeholders} filled. */
export function t(key: TextKey, vars?: Vars): string {
  return tIn(lang.value, key, vars);
}

function tIn(l: Lang, key: TextKey, vars?: Vars): string {
  const text = tables[l][key] ?? de[key] ?? key;
  return fill(text, vars);
}

/** A message of the robot ({key, text, args}) in the current language. */
export function msg(m: Msg | null | undefined): string {
  return msgAt(m, 0);
}

function msgAt(m: unknown, depth: number): string {
  if (!m || typeof m !== "object") return "";
  const { key, text, args } = m as Msg;
  if (lang.value === "de" && typeof key === "string" && own(msgDe, key)) {
    const vars = args && typeof args === "object" && !Array.isArray(args) ? args : {};
    return fill(msgDe[key], vars, depth);
  }
  return typeof text === "string" ? text : "";
}

/** The locale for numbers and dates. */
export const locale = () => (lang.value === "de" ? "de-DE" : "en-GB");
