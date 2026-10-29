// SPDX-License-Identifier: AGPL-3.0-or-later
// REST client and types of the robot's API (/api/v1). The web app uses only this API.
import { msg, t } from "./i18n";

export type Msg = { key: string; text: string; args?: Record<string, unknown> };

export type Quality = "offline" | "no_data" | "stale" | "frozen" | "implausible" | "jump" | "uncalibrated" | "ok";
export type Signal = "ph" | "ec" | "temp" | "flow";

export type Reading = {
  value: number | null;
  quality: Quality;
  reason: Msg;
  ageS: number | null;
  unit: string;
  decimals: number;
  usable: boolean;
};

/** The steps of one check, in order (SD-105); off, stopped and wait stand outside it. */
export type Phase = "off" | "stopped" | "pump_on" | "prove_flow" | "flush" | "measure" | "dose" | "mix" | "remeasure" | "hold" | "wait";

export type CycleSettings = {
  recipe: string | null;
  ecTarget: number | null; ecTolerance: number | null; phTarget: number | null; phTolerance: number | null;
  volumeL: number | null; flowLpm: number | null; turnoverMin: number | null;
  // The waits in force (the user's, else the suggestion) and the suggestions from volume and flow
  // The mixing wait after a dose (SD-112): in force, the robot's own, the user's least; why; the stability window
  mixMin: number | null; mixOwnMin: number | null; mixUserMin: number | null; mixWhy: "turnover" | "user" | "minimum" | "settling" | "settling_default"; stableS: number;
  holdMin: number;
};

// One thing a kind of dose needs, and whether it holds now (MixCycle::checks)
export type Check = Msg & { ok: boolean };

export type Cycle = {
  phase: Phase;
  since: number;
  line: Msg;
  notes: Msg[];
  // Flush and mix: seconds done of total (mix counts only with flow); hold and wait: the next check
  progress: { doneS?: number; totalS?: number; paused?: boolean; first?: boolean; nextAt?: number } | null;
  round: { kind: "ec" | "ph"; n: number; test: boolean; ml: number; from: number | null } | null;
  last: { ph: number | null; ec: number | null; temp: number | null; at: number } | null;
  mixPending: boolean;
  checks: { all: Check[]; ec: Check[]; ph: Check[] };
  settings: CycleSettings;
};

/** An alarm raised from a watchdog problem (SD-123); `ackUntil`: its reminders are paused until then (0: not). */
export type Alarm = { id: string; level: "critical" | "warning" | "info"; since: number; sent: number; ackUntil: number; label: Msg; text: Msg };
/** What the robot pushes, or would push without a channel: an alarm, a reminder, the morning report (SD-123). */
export type Notice = { id: number; at: number; kind: "alarm" | "repeat" | "report"; level: "critical" | "warning" | "info"; title: Msg; lines: Msg[]; items: string[] };

/** The push channels without their secrets (SD-124): of a token or password only whether it is set; how each fared last. */
export type PushSettings = {
  ntfy: { enabled: boolean; server: string; topic: string; tokenSet: boolean };
  telegram: { enabled: boolean; chatId: string; tokenSet: boolean };
  email: { enabled: boolean; server: string; from: string; to: string; user: string; passwordSet: boolean };
  status: Record<string, { at: number; ok: boolean; error: string }>;
};

/** A maintenance item (SD-125): its interval, when it was last done, when it is due and the date it is bundled into. */
export type ServiceItem = { id: string; kind: string; channel: number; days: number; defaultDays: number; last: number; due: number; date: number; dueNow: boolean; label: Msg };
export type ServicePlan = { dates: number[]; items: ServiceItem[] };

// Plants and their phases (SD-128)
export type PlantPhase = { name: string; days: number; confirm: boolean; recipe: string; ecTarget: number | null; ph: [number, number] | null };
export type PlantRecipe = { id: string; name: string; note: string; phases: PlantPhase[] };
export type Plant = {
  id: string; name: string; from: string; phases: PlantPhase[]; start: number; phase: number; phaseStart: number;
  harvestAt: number; completedAt: number; running: boolean; leading: boolean; day: number;
  phaseDay?: number; phaseEnd?: number; daysOver?: number; ecTarget?: number | null; phTarget?: number | null;
};
export type Plants = { system: GrowSystem | ""; systems: Record<GrowSystem, [number, number]>; plantRecipes: PlantRecipe[]; plants: Plant[]; lead: string | null; plan: { source: string; recipe: string | null; plant: string | null } };
export type WaterPlan = {
  source: "plant" | "hand" | ""; recipe: string | null; ecTarget: number | null; phTarget: number | null; until: number; ended: boolean;
  plant: { id: string; name: string; phase: string; phaseEnd: number; confirm: boolean } | null;
};
export type CalendarEntry = {
  date: number; kind: "phase" | "confirm" | "harvest" | "maintenance"; overdue: boolean;
  plant?: string; name?: string; phase?: string; items?: Msg[];
};
export type Calendar = { today: number; until: number; entries: CalendarEntry[] };

export type Assessment = { id: string; label: Msg; status: "ok" | "problem" | "neutral"; text: Msg };
export type Watchdog = {
  evaluatedAt: number; overall: "ok" | "problem" | "neutral"; ok: number; problems: number; neutral: number;
  headline: Msg; items: Assessment[]; stale: boolean;
};

export type ChannelJob = {
  // "prime": a line into a cup, by time; "fill": a line into the tank until it drips (SD-111)
  id: string; type: "calibration" | "prime" | "fill"; channel: number;
  state: "running" | "waiting_result" | "done" | "failed";
  seconds: number; actualS: number; ml: number | null; message: Msg; at: number;
};

export type ProbeSession = { kind: "ph" | "ec"; points: [number, number][]; rawPh: number | null; rawEc: number | null; temp: number | null };

export type RobotState = {
  now: number; uptimeS: number; clock: string; stopped: boolean; online: boolean;
  feedPump: boolean | null; flowOk: boolean;
  pump: { amps: number | null; normalA: number | null };
  // The water plan: the leading plant's phase, or the recipe chosen by hand (SD-128)
  plan: WaterPlan;
  // Commissioning (SD-129): whether every step is done or skipped, and the next open one
  setup: { complete: boolean; next: SetupStepId | null };
  // The hardware release and the water path as the device reports them (SD-109, SD-110)
  hardware: { release: { on: boolean; leak: boolean; highLevel: boolean; stopButton: boolean; noFlow: boolean }; outletWet: boolean | null; returnPump: boolean | null };
  readings: Record<Signal, Reading>;
  cycle: Cycle;
  watchdog: Watchdog;
  alarms: Alarm[];
  // reportOnly: set while its lock reported only; it holds nothing (SD-133)
  latches: Record<string, Record<string, unknown> & { reportOnly?: boolean }>;
  jumpLocks: Record<string, { until: number; from: number; to: number; at: number }>;
  newTank: boolean; topUp: boolean;
  // The refill valve (SD-132): a fill under way and since when, why it was started, whether the float
  // says full, why a fill cannot start now, and the longest fill
  refill: { filling: boolean; asked: boolean; opened: boolean; since: number | null; why: "top_up" | "button" | null; full: boolean | null; blocked: Msg | null; maxFillMin: number };
  // The circulation pump (SD-132): its mode, and whether the robot wants it on now
  circulation: { mode: CirculationMode; wanted: boolean };
  effects: { ph: number | null; ecByRecipe: Record<string, number | null> };
  dosing: { order: string; purpose: string; canister: string; name: string; ml: number | null; mlDone: number; run: number; runs: number } | null;
  // `head`: the pump head fitted, null without one (SD-108)
  // `day`: its run time into the tank in the last 24 h and its cap (SD-113)
  channels: { channel: number; canister: string | null; head: string | null; flowMlPerMin: number | null; at: number | null; drops: { count: number; lastAgoS: number | null } | null; day: { usedS: number; capS: number } }[];
  stock: Record<string, Stock>;
  // What a pair still misses after a round cut short; the next nutrient round doses it first (SD-114)
  catchUp: { recipe: string; parts: { canister: string; name: string; ml: number }[] } | null;
  channelJob: ChannelJob | null;
  probe: ProbeSession | null;
  // Maintenance mode until then (epoch seconds); null when off
  maintenanceUntil: number | null;
  eventId: number;
  sim?: { speed: number; scenario: string };
};

export type Canister = {
  id: string; name: string; role: "nutrient" | "ph_down"; channel: number; pair: string; color: string; capacityMl: number | null; maxMlPerDay: number | null;
  // "" on this device; the coupled device's ID for a canister on its channel (SD-131)
  device: string;
};
/** A canister's level counted from use (SD-119): unknown until entered; may fall below zero. */
export type Stock = { ml: number | null; status: "unknown" | "ok" | "low" | "empty" };
/** A recipe: ml per litre of each part, its EC target and pH range; a fixed part is never scaled (RAT-085). */
export type Recipe = { id: string; name: string; note: string; steps: { canister: string; mlPerL: number | null; fixed: boolean }[]; ecTarget: number | null; ph: [number, number] | null };

export type Config = {
  schemaVersion: number; revision: number;
  system: { name: string; language: string; timezone: string; passwordSet: boolean };
  // Quiet hours in local minutes of the day; equal: none (SD-123)
  notify: { quietFromMin: number; quietToMin: number };
  // `system`: the cultivation system (SD-128), "" while none is chosen
  tank: { volumeL: number | null; mixMinMin: number | null; holdMin: number | null; system: GrowSystem | "" };
  control: { ecTolerance: number; phTolerance: number; maxEcStep: number; ecFloor: number; phMaxMlPerDose: number; maxEcRounds: number; maxPhDoses: number };
  // The locks a person releases that only report (SD-133)
  locks: { reportOnly: string[] };
  // The refill valve's longest fill, 1 to 25 min (RAT-079)
  refill: { maxFillMin: number };
  circulation: { mode: CirculationMode };
  limits: { minRunS: number; maxRunS: number; maxPartialRuns: number };
  canisters: Canister[];
  recipes: Recipe[];
  active: { recipe: string; from: number; until: number };
  channels: Record<string, { flowMlPerMin: number | null; at: number; head: string }>;
  probes: { ph: { points?: [number, number][]; at?: number } | null; ec: { factor?: number; reference?: number; tempC?: number; at?: number } | null };
};

export type Info = {
  product: string; version: string; api: number; schemaVersion: number;
  platform: { kind: string; simulated?: boolean; scenario?: string };
  // The device's own identity (SD-129): random, made at the first start, never in a backup
  deviceId: string;
  hasPassword: boolean; setupCode?: boolean; name: string; language?: "de" | "en"; channelCount: number;
  // Why neither sign-in nor a first password works: the password file is unreadable or gone
  passwordProblem?: Msg | null;
  // What the device has (SD-108); `example`: the simulator's profile, its numbers are assumptions
  profile?: Profile;
};

export type Profile = {
  line: string; variant: string;
  channels: { pump: string; head: string | null; dropSensor: boolean; reverse: boolean }[];
  flow: string; pumpIn: "tank" | "device"; return: string; network: string[]; pumpLpmLow: number | null; example: boolean;
};

// Coupling two devices (PD-457, SD-131): the peer without its token, a pairing code shown here,
// and how the last pairing from here went
export type Peer = { role: "leader" | "follower"; id: string; name: string; address: string; since: number };
export type Coupling = {
  deviceId: string; peer: Peer | null; code: { until: number } | null;
  pairing: { state: "pairing" | "failed"; address: string; error: Msg | null; at: number } | null;
  // While coupled: whether the other device was heard within 10 s, and when (SD-131)
  link: { ok: boolean; heard: number | null } | null;
};

// MQTT with Home Assistant discovery (PD-461, SD-130): the password is written, never read back
export type MqttSettings = {
  enabled: boolean; host: string; port: number; tls: boolean; user: string; passwordSet: boolean;
  commands: boolean; discovery: string; commandsOn: boolean; topic: string;
  status: { connected: boolean; error: Msg | null; at: number };
};

// Commissioning (SD-129): the steps of #10 in their order. "device": done while the robot sees it;
// "confirm": done once the user confirms it; "later": not there yet
export type SetupStepId = "password" | "network" | "placement" | "flow" | "probes" | "channels" | "lines" | "test" | "tank" | "push" | "account";
export type SetupStep = { id: SetupStepId; how: "device" | "confirm" | "later"; optional: boolean; state: "open" | "done" | "skipped" | "later"; at: number };
export type Setup = { steps: SetupStep[]; next: SetupStepId | null; complete: boolean };

// Access tokens (SD-129): the owner sees each with its role and last use since the start, never the token
export type TokenRole = "app" | "account" | "peer";
export const TOKEN_ROLES: TokenRole[] = ["app", "account", "peer"];
export type AccessToken = { id: string; name: string; role: TokenRole; created: number; used: number | null };
export type AccessTokens = { tokens: AccessToken[]; max: number };
export type NewToken = { id: string; name: string; role: TokenRole; created: number; token: string };

export type RobotEvent = { id: number; ts: number; type: string; severity: "info" | "notice" | "warn" | "alarm"; title: Msg; text: Msg; data: Record<string, any> };

export type SeriesData = { series: string; stepS: number; t: number[]; avg: (number | null)[]; min: (number | null)[]; max: (number | null)[] };

export type TemplateStep = { name: string; nameDe?: string; pair?: string; mlPerL: number };
export type RecipeTemplate = { id: string; name: string; nameDe?: string; note?: string; noteDe?: string; source?: string; systems?: GrowSystem[]; ecTarget?: number; ph?: [number, number]; steps: TemplateStep[] };
export type GrowSystem = "hydro" | "coco" | "rockwool" | "soil";
export const GROW_SYSTEMS: GrowSystem[] = ["hydro", "coco", "rockwool", "soil"];

// The message is shown in the page language.
export class ApiError extends Error {
  constructor(public status: number, public key: string, text: string, public body: any) {
    super(text);
  }
}

let onUnauthorized: () => void = () => {};
export function setUnauthorizedHandler(f: () => void) {
  onUnauthorized = f;
}

// A request that got no answer: no connection, or none within its time limit
export class NetworkError extends Error {}

export async function api<T = any>(method: string, path: string, body?: unknown, timeoutMs?: number): Promise<T> {
  let res: Response;
  let text: string;
  // AbortController, not AbortSignal.timeout: that is missing before iOS 16, and a
  // Stop must still be sent from an old phone on the wall
  const abort = timeoutMs ? new AbortController() : undefined;
  const timer = abort ? setTimeout(() => abort.abort(), timeoutMs) : undefined;
  try {
    res = await fetch(`/api/v1${path}`, {
      method,
      headers: body !== undefined ? { "Content-Type": "application/json" } : undefined,
      body: body !== undefined ? JSON.stringify(body) : undefined,
      credentials: "same-origin",
      signal: abort?.signal,
    });
    text = await res.text();
  } catch (e) {
    throw new NetworkError(e instanceof Error ? e.message : String(e));
  } finally {
    clearTimeout(timer);
  }
  let data: any = null;
  try {
    data = text ? JSON.parse(text) : null;
  } catch {
    data = text;
  }
  if (!res.ok) {
    if (res.status === 401 && !path.startsWith("/auth/")) onUnauthorized();
    const err = data?.error ?? {};
    throw new ApiError(res.status, err.key ?? "http", err.text ? msg(err) : t("shell.httpError", { status: res.status }), data);
  }
  return data as T;
}

export const get = <T = any>(p: string) => api<T>("GET", p);
export const post = <T = any>(p: string, b: unknown = {}, timeoutMs?: number) => api<T>("POST", p, b, timeoutMs);
export const put = <T = any>(p: string, b: unknown) => api<T>("PUT", p, b);
export const del = <T = any>(p: string) => api<T>("DELETE", p);

// The simulator's controls (simulator only)
export const sim = (action: string, body: unknown = {}) => api("POST", `/sim/${action}`, body);

/** The circulation pump runs during a dose, its mixing wait and a fill, or always (SD-132). */
export type CirculationMode = "mixing" | "always";

// The locks a person releases that may be set to report only, by their kind (SD-133); as the core's
export const REPORTABLE_LOCKS = ["flow.dry", "tank.volume", "ec.no_effect", "ph.no_effect", "channel.no_drops", "channel.siphon", "hw.return", "refill"] as const;
// Whether a latch holds: one set while its lock reported only holds nothing (SD-133)
export const holds = (latch: { reportOnly?: boolean } | undefined) => !!latch && !latch.reportOnly;

// Network devices and the roles bound to them (SD-132)
export type NetDevice = {
  id: string;
  kind: "shelly" | "esphome";
  address: string;
  name: string;
  online: boolean;
  polled: boolean;
  hasPassword: boolean;
  ownId: string | null;
  problem: Msg | null;
  switches: { channel: number; on: boolean }[];
  inputs: { channel: number; state: boolean }[];
};
export type RoleSource = "shelly" | "esphome" | "mqtt";
export type RoleBinding = { source: RoleSource; device: string; channel: number; entity: string; field: string; invert: boolean; availability: string };
export type DeviceRole = {
  kind: "output" | "binary";
  pulse: boolean;
  autoOffS: number;
  binding: RoleBinding | null;
  output?: { state: string; verified: boolean; want: boolean; problem: Msg | null };
  value?: unknown;
  state?: string;
};
export type Devices = { devices: NetDevice[]; roles: Record<string, DeviceRole>; mqttTopics: string[] };
export const DEVICE_ROLES = ["refill_valve", "circulation_pump", "tank_full"] as const;
