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
  // Mixed by hand, the robot corrects EC at most this far below its target (SD-150)
  holdGapEc: number;
  holdMin: number;
};

/** How long a preparation takes (SD-150): its rounds, minutes in all and when it should be done;
 *  `more`: longer than the estimate follows; `limited`: the pumps are slow for the tank; `daily`: the
 *  daily run time per channel stretches it; `reachEc`: where its rounds end. */
export type PrepEstimate =
  | { ok: true; rounds: number; ml: number; totalMin: number; endAt: number; leftMin: number; more: boolean; limited: boolean; daily: boolean; reachEc: number | null }
  | { ok: false; reason: Msg };

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
  // A preparation under way and a fresh fill from water without salts, each with its estimate (SD-150)
  prep: { startedAt: number; from: number; target: number; rounds: number; estimate: PrepEstimate } | null;
  fresh: PrepEstimate | null;
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
// `shotsPerDay`, `shotS`: for a watering circuit that follows the plant (SD-134); `lightHours`,
// `lightPct`: for a light circuit that follows it (SD-135); null: the circuit's own
export type PlantPhase = {
  name: string; days: number; confirm: boolean; recipe: string; ecTarget: number | null; ph: [number, number] | null;
  shotsPerDay?: number | null; shotS?: number | null; lightHours?: number | null; lightPct?: number | null;
  // Climate (SD-136): a room's targets; a VPD leads the humidity in place of one
  dayC?: number | null; nightC?: number | null; rhPct?: number | null; vpdKpa?: number | null; co2Ppm?: number | null;
};
export type PlantRecipe = { id: string; name: string; note: string; phases: PlantPhase[] };
export type Plant = {
  id: string; name: string; from: string; phases: PlantPhase[]; start: number; phase: number; phaseStart: number;
  harvestAt: number; completedAt: number; running: boolean; leading: boolean; day: number;
  phaseDay?: number; phaseEnd?: number; daysOver?: number; ecTarget?: number | null; phTarget?: number | null;
  // The local midnight each phase it reached began, 0: not known; the yield by hand, g (RAT-064)
  began: number[]; yieldG: number | null;
  // Frozen when it is completed (RAT-064, SD-138); null while it runs
  record: PlantRecord | null;
};
// What a plant was given: the tank's amounts while it ran, which plants beside it shared (SD-138).
// `countedFrom` null: not counted; `countLost`: the count was lost; both leave the amounts unknown.
export type PlantRecord = {
  from: number; to: number; days: number; harvestAt: number | null;
  phases: { name: string; days: number | null }[];
  countedFrom: number | null; countLost: boolean;
  canisters: { id: string; name: string; role: string; ml: number; unknown: number }[];
  wateredL: number | null; wateredUnknown: number; with: string[];
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
  id: string; type: "prime" | "fill"; channel: number;
  state: "running" | "done" | "failed";
  seconds: number; actualS: number; message: Msg; at: number;
};

// A pump calibration by the user (SD-143): its step, and whether it is the person's turn
export type PumpCalibration = {
  channel: number; method: "cylinder" | "scale";
  step: "reverse" | "to_water" | "prime" | "reading" | "run" | "empty" | "to_canister" | "fill" | "done" | "failed" | "cancelled";
  waiting: boolean; readings: number[]; runs: { s: number; drops: number; flowMlPerMin: number | null }[];
  flowMlPerMin: number | null; nominalMlPerMin: number | null; message: Msg; at: number;
  waitingFor: Msg | null;  // why the step's run has not started yet: no flow, a hold
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
  // reportOnly: set while its lock reported only; it holds nothing (SD-133); note: a note that holds
  // nothing, such as a restart by the safety supervisor (SD-141)
  latches: Record<string, Record<string, unknown> & { reportOnly?: boolean; note?: boolean }>;
  jumpLocks: Record<string, { until: number; from: number; to: number; at: number }>;
  newTank: boolean; topUp: boolean;
  // The refill valve (SD-132): a fill under way and since when, why it was started, whether the float
  // says full, why a fill cannot start now, and the longest fill
  refill: { filling: boolean; asked: boolean; opened: boolean; since: number | null; why: "top_up" | "button" | null; full: boolean | null; blocked: Msg | null; maxFillMin: number };
  // The circulation pump (SD-132): its mode, and whether the robot wants it on now
  circulation: { mode: CirculationMode; wanted: boolean };
  // Watering (SD-134): the circuit that waters, the litres given since the tank was full, the
  // reserve and what the tank may still give, whether the water is fit, and each circuit's state
  watering: {
    running: string | null; leftS: number | null; override: boolean; givenL: number | null; reserveL: number | null; availableL: number | null; low: boolean;
    fit: Msg | null; warning: Msg | null; mixing: boolean;
    // `blocked`: why its schedule's next shot cannot start; `hand`: why no shot by hand can, and
    // `anyway`: only the water stands in the way, which a person may water over (RAT-086)
    circuits: {
      id: string; running: boolean; due: boolean; paused: boolean; nextAt: number | null; nextS: number | null; dayS: number;
      // The plant it follows, its current phase and the shots that gives; null: it follows none
      plant: { id: string; name: string; phase: string | null; shotsPerDay: number; shotS: number } | null;
      // `upTo`: cut by a restart, it ran this long at most
      last: { at: number; s: number; l: number | null; end: Msg; override?: boolean; upTo?: boolean } | null; blocked: Msg | null; hand: Msg | null; anyway: boolean;
    }[];
  };
  effects: { ph: number | null; ecByRecipe: Record<string, number | null> };
  dosing: { order: string; purpose: string; canister: string; name: string; ml: number | null; mlDone: number; run: number; runs: number } | null;
  // `head`: the pump head fitted, null without one (SD-108)
  // `day`: its run time into the tank in the last 24 h and its cap (SD-113)
  channels: {
    channel: number; canister: string | null; head: string | null; flowMlPerMin: number | null; at: number | null;
    drops: { count: number; lastAgoS: number | null } | null; day: { usedS: number; capS: number };
    // A rate the consumption check proposes, and a change of its drops per ml since its
    // calibration (SD-145)
    rateProposal: RateProposal | null;
    drift: { share: number; at: number } | null;
  }[];
  stock: Record<string, Stock>;
  // What a pair still misses after a round cut short; the next nutrient round doses it first (SD-114)
  catchUp: { recipe: string; parts: { canister: string; name: string; ml: number }[] } | null;
  channelJob: ChannelJob | null;
  pumpCalibration: PumpCalibration | null;
  probe: ProbeSession | null;
  // Light (SD-135): each circuit, whether it is day by its plan, whether it is on and at what
  // brightness, its next change (null: none within a day), a switch by hand, the plant it follows
  // and its output
  lighting: {
    id: string; day: boolean; on: boolean; pct: number | null; hours: number; nextChangeAt: number | null;
    hand: { on: boolean; until: number } | null; darkOn: boolean;
    plant: { id: string; name: string; phase: string | null; hours: number; pct: number } | null;
    output: DeviceRole["output"] | null;
    // Heat protection of its room (SD-136): its brightness at most so much, or off
    heat: { off: boolean; pct: number } | null;
  }[];
  // Climate (SD-136): each room's day, targets, air, outputs, what has no value, and its alarms
  climate: {
    id: string; day: boolean; byVpd: boolean;
    targets: { tempC: number | null; rhPct: number | null; vpdKpa: number | null; co2Ppm: number | null };
    air: { tempC: number | null; rhPct: number | null; vpdKpa: number | null; co2Ppm: number | null };
    missing: ("temp" | "rh" | "co2")[];
    co2High: boolean; co2Locked: boolean;
    heat: { off: boolean; pct: number } | null;
    outputs: Record<ClimateOutput, { want: boolean; output: DeviceRole["output"] | null }>;
    plant: { id: string; name: string; phase: string | null } | null;
  }[];
  // Maintenance mode until then (epoch seconds); null when off
  maintenanceUntil: number | null;
  eventId: number;
  sim?: { speed: number; scenario: string };
};

export type Canister = {
  id: string; name: string; role: "nutrient" | "ph_down" | "silicate"; channel: number; pair: string; color: string; capacityMl: number | null; maxMlPerDay: number | null;
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
  // `prepare`: who prepares a fresh fill, the robot or the person by hand while the robot holds (SD-150)
  tank: { volumeL: number | null; mixMinMin: number | null; holdMin: number | null; system: GrowSystem | ""; prepare: "robot" | "hand" };
  control: { ecTolerance: number; phTolerance: number; maxEcStep: number; ecFloor: number; phMaxMlPerDose: number; maxEcRounds: number; maxPhDoses: number };
  // The locks a person releases that only report (SD-133)
  locks: { reportOnly: string[] };
  // The refill valve's longest fill, 1 to 25 min (RAT-079)
  refill: { maxFillMin: number };
  circulation: { mode: CirculationMode };
  // Watering (SD-134): the reserve, and the circuits with their schedules
  watering: { reservePct: number; circuits: WaterCircuit[] };
  lighting: { circuits: LightCircuit[] };
  climate: { rooms: ClimateRoom[] };
  limits: { minRunS: number; maxRunS: number; maxPartialRuns: number };
  canisters: Canister[];
  recipes: Recipe[];
  active: { recipe: string; from: number; until: number };
  channels: Record<string, { flowMlPerMin: number | null; at: number; head: string }>;
  probes: { ph: { points?: [number, number][]; at?: number } | null; ec: { factor?: number; reference?: number; tempC?: number; at?: number } | null };
};

// What a level read and entered says of its channel's rate (SD-145)
export type RateProposal = { canister: string; countedMl: number; usedMl: number; from: number; to: number; at: number };
export type UseCheck = { countedMl: number; usedMl: number; share: number; proposal: RateProposal | null; calibrate: boolean } | null;

export type Info = {
  product: string; version: string; api: number; schemaVersion: number;
  platform: { kind: string; simulated?: boolean; scenario?: string };
  // The device's own identity (SD-129): random, made at the first start, never in a backup
  deviceId: string;
  hasPassword: boolean; setupCode?: boolean; name: string; language?: "de" | "en"; channelCount: number;
  // The setup code is the one on the device's label, taken in the function key's window (SD-149)
  labelCode?: boolean; keyWindow?: boolean;
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

/** A watering circuit (SD-134): a pump or valve on a Shelly output, bound as the role `water.<id>`. */
export type WaterCircuit = {
  id: string; name: string; mode: "window" | "times" | "interval" | "manual";
  fromMin: number; toMin: number; shotsPerDay: number; shotS: number; times: { atMin: number; s: number }[]; everyMin: number;
  longestS: number; rateLpm: number | null; dailyMaxS: number | null; paused: boolean;
  plant: string;  // window: the plant whose current phase sets shots a day and their length; "": none
};

/** A light circuit (SD-135): a Shelly switch, or a Shelly dimmer on a 0–10 V line, bound as the role `light.<id>`. */
/** A climate room (SD-136): its outputs and sensors are the roles `climate.<id>.<kind>`. */
export type ClimateRoom = {
  id: string; name: string;
  lights: string[];                 // its light circuits; the first one's day is the room's
  startMin: number; hours: number;  // its own day without a light circuit
  dayC: number; nightC: number; byVpd: boolean; rhPct: number; vpdKpa: number;
  co2Ppm: number | null;            // by day; null: no CO2
  heatC: number;                    // heat protection for light: its first limit
  heaterW: number | null;           // the heater's power: its plug's power limit is a fifth above it
  circulateAlways: boolean; plant: string;
};
export const CLIMATE_OUTPUTS = ["exhaust", "circulation", "heater", "humidifier", "dehumidifier", "co2_valve"] as const;
export type ClimateOutput = (typeof CLIMATE_OUTPUTS)[number];
export const CLIMATE_SENSORS = ["temp", "rh", "co2"] as const;

export type LightCircuit = {
  id: string; name: string; dimmed: boolean;
  startMin: number; hours: number;  // its day: from its start, local time, for its hours of light
  pct: number; thresholdPct: number; sunriseMin: number; sunsetMin: number;  // a dimmer's brightness, its driver's threshold, its ramps
  plant: string;  // the plant whose current phase sets the hours and the brightness; "": none
};

/** The circulation pump runs during a dose, its mixing wait and a fill, or always (SD-132). */
export type CirculationMode = "mixing" | "always";

// The locks a person releases that may be set to report only, by their kind (SD-133); as the core's
export const REPORTABLE_LOCKS = ["flow.dry", "tank.volume", "ec.no_effect", "ph.no_effect", "channel.no_drops", "channel.siphon", "hw.return", "refill"] as const;
// Whether a latch holds: one set while its lock reported only holds nothing (SD-133)
export const holds = (latch: { reportOnly?: boolean; note?: boolean } | undefined) => !!latch && !latch.reportOnly && !latch.note;

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
  kind: "output" | "binary" | "level" | "temp" | "rh" | "co2";
  pulse: boolean;
  autoOffS: number;
  binding: RoleBinding | null;
  // `level`, `brightness`: a dimmer's brightness wanted and read back (SD-135)
  output?: { state: string; verified: boolean; want: boolean; problem: Msg | null; level?: number; brightness?: number | null };
  value?: unknown;
  state?: string;
};
export type Devices = { devices: NetDevice[]; roles: Record<string, DeviceRole>; mqttTopics: string[] };
export const DEVICE_ROLES = ["refill_valve", "circulation_pump", "tank_full", "tank_empty", "tank_level"] as const;
