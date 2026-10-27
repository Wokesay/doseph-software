// SPDX-License-Identifier: AGPL-3.0-or-later
// The app's state: info, session, live state (SSE), configuration.
import { computed, signal } from "@preact/signals";
import { ApiError, NetworkError, get, setUnauthorizedHandler, type Config, type Info, type RobotState } from "./api";
import { hasOwnLang, setLang, t } from "./i18n";

export const info = signal<Info | null>(null);
export const authed = signal<boolean | null>(null);
export const state = signal<RobotState | null>(null);
export const config = signal<Config | null>(null);
export const lastUpdate = signal<number>(0);
/** The session ran out while the app was open: whatever was clicked last was not carried out. */
export const sessionExpired = signal(false);

// The robot sends its state every second, by the live stream or the polls that
// stand in for it. Nothing new for longer than this: what the page shows is not
// current. Not the stream's own state, so working polls or a quick reconnect show nothing.
const STALE_MS = 10_000;
const now = signal(Date.now());
export const stale = computed(() => lastUpdate.value > 0 && now.value - lastUpdate.value > STALE_MS);
let clockTimer: number | undefined;

export const simulated = computed(() => !!info.value?.platform?.simulated);
/** A SemVer pre-release (a hyphen after X.Y.Z, before any "+") or a version that cannot be read is for testing only. */
export function forTestingOnly(version: string | null | undefined): boolean {
  const m = /^\d+\.\d+\.\d+(-[^+]+)?(\+.*)?$/.exec(version ?? "");
  return !m || m[1] !== undefined;
}
export const testingOnly = computed(() => forTestingOnly(info.value?.version));

type Toast = { id: number; kind: "ok" | "error" | "info"; text: string };
export const toasts = signal<Toast[]>([]);
let toastId = 0;
export function toast(text: string, kind: Toast["kind"] = "ok") {
  const id = ++toastId;
  toasts.value = [...toasts.value, { id, kind, text }];
  setTimeout(() => (toasts.value = toasts.value.filter((t) => t.id !== id)), kind === "error" ? 7000 : 3500);
}
/** What a failed action shows: the robot's reason for a refusal, that it got no answer, or
 *  that it did not work; never a raw "TypeError: Failed to fetch". */
export function errorText(e: unknown): string {
  if (e instanceof ApiError) return e.message;
  if (e instanceof NetworkError) return t("shell.noAnswer");
  return t("shell.failed");
}
export function toastError(e: unknown) {
  toast(errorText(e), "error");
}

let source: EventSource | null = null;
let pollTimer: number | undefined;
let renewTimer: number | undefined;

// The browser keeps the session cookie as long as the robot keeps an unused session. A page left
// open, such as a phone on the wall, asks now and then, so a Stop still has its session;
// one asleep longer finds out as soon as it is woken.
const RENEW_MS = 10 * 60_000;
async function renewSession() {
  try {
    const s = await get<{ authenticated: boolean }>("/auth/session");
    if (!s.authenticated && authed.value) {
      sessionExpired.value = true;
      authed.value = false;
      stopLive();
    }
  } catch {
    /* no answer: the next time, or the polls say so */
  }
}
document.addEventListener("visibilitychange", () => {
  if (document.visibilityState === "visible" && authed.value) void renewSession();
});

function startLive() {
  stopLive();
  renewTimer = window.setInterval(renewSession, RENEW_MS);
  clockTimer = window.setInterval(() => {
    now.value = Date.now();
    // A stream that is open but brings nothing (a proxy holding it back): ask instead
    if (stale.value && !pollTimer) pollTimer = window.setInterval(refreshState, 3000);
  }, 1000);
  try {
    source = new EventSource("/api/v1/events/stream", { withCredentials: true });
    source.addEventListener("state", (ev) => {
      state.value = JSON.parse((ev as MessageEvent).data);
      lastUpdate.value = Date.now();
      // The stream brings values again: polls started while it was silent are not needed
      if (pollTimer) {
        clearInterval(pollTimer);
        pollTimer = undefined;
      }
    });
    source.onerror = () => {
      // EventSource reconnects by itself; ask as well meanwhile
      if (!pollTimer) pollTimer = window.setInterval(refreshState, 3000);
    };
    source.onopen = () => {
      if (pollTimer) {
        clearInterval(pollTimer);
        pollTimer = undefined;
      }
    };
  } catch {
    pollTimer = window.setInterval(refreshState, 2000);
  }
}

function stopLive() {
  source?.close();
  source = null;
  if (pollTimer) clearInterval(pollTimer);
  pollTimer = undefined;
  if (clockTimer) clearInterval(clockTimer);
  clockTimer = undefined;
  if (renewTimer) clearInterval(renewTimer);
  renewTimer = undefined;
}

export async function refreshState() {
  try {
    state.value = await get<RobotState>("/state");
    lastUpdate.value = Date.now();
  } catch {
    /* the connection is tried again later */
  }
}

export async function refreshConfig() {
  config.value = await get<Config>("/config");
}

export async function boot() {
  setUnauthorizedHandler(() => {
    if (authed.value) sessionExpired.value = true;
    authed.value = false;
    stopLive();
  });
  info.value = await get<Info>("/info");
  // Without a choice in this browser, the sign-in page speaks the robot's language
  if (!hasOwnLang() && (info.value.language === "de" || info.value.language === "en")) setLang(info.value.language, false);
  const s = await get<{ authenticated: boolean }>("/auth/session");
  authed.value = s.authenticated;
  if (s.authenticated) await afterLogin();
}

export async function afterLogin() {
  authed.value = true;
  sessionExpired.value = false;
  info.value = await get<Info>("/info");
  const [cfg] = await Promise.all([get<Config>("/config"), refreshState()]);
  config.value = cfg;
  // Without a choice of its own the browser takes the language set on the robot
  if (!hasOwnLang() && (cfg.system.language === "de" || cfg.system.language === "en")) setLang(cfg.system.language, false);
  startLive();
}

export function logoutLocal() {
  stopLive();
  authed.value = false;
  state.value = null;
  config.value = null;
}

// What several pages need
export const canisters = computed(() => config.value?.canisters ?? []);
export const recipes = computed(() => config.value?.recipes ?? []);
export const activeRecipe = computed(() => recipes.value.find((r) => r.id === config.value?.active.recipe));
/** The middle of a recipe's pH range, its pH target (RAT-066); null without a range. */
export const phMiddle = (r: { ph?: [number, number] | null } | undefined) => (r?.ph ? Math.round(((r.ph[0] + r.ph[1]) / 2) * 100) / 100 : null);
// The bounds the robot accepts (config.hpp); the page checks them before it sends
export const LIMITS = {
  volumeL: [1, 100000] as [number, number],
  waitMin: [2, 600] as [number, number],
  holdMin: [10, 1440] as [number, number],
  days: [1, 366] as [number, number],
};
