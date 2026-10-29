// SPDX-License-Identifier: AGPL-3.0-or-later
// Settings: language and look, the device's name and time zone, quiet hours
// and what was notified, the password, access tokens, backup and restore,
// reporting a problem, signing out, the version.
import { useEffect, useState } from "preact/hooks";
import { Bell, Bug, Cable, Copy, DatabaseBackup, House, Link2, Download, ExternalLink, Info as InfoIcon, KeyRound, Lock, LogOut, Monitor, Send, Ticket, Upload } from "lucide-preact";
import { ApiError, type CirculationMode, DEVICE_ROLES, del, get, post, put, REPORTABLE_LOCKS, TOKEN_ROLES, type Devices, type RoleBinding, type RoleSource, type AccessTokens, type Coupling, type MqttSettings, type Msg, type NewToken, type Notice, type PushSettings, type TokenRole } from "../api";
import { dateTime, timeZone } from "../format";
import { lang, msg, setLang, t, type Lang, type TextKey } from "../i18n";
import { config, info, logoutLocal, refreshConfig, state, toast, toastError } from "../store";
import { currentTheme, setTheme, type Theme } from "../theme";
import { Banner, Button, Card, Field, LANGS, Pill, Seg, Toggle } from "../ui";

function LookCard() {
  const [theme, setThemeState] = useState<Theme>(currentTheme());
  return (
    <Card title={t("settings.look")} icon={<Monitor size={18} />}>
      <div class="stack">
        <Field label={t("common.language")} group>
          <Seg<Lang>
            value={lang.value}
            options={LANGS}
            onChange={async (l) => {
              setLang(l);
              // The robot's language too: the sign-in page and a new browser start in it
              await put("/system", { language: l });
              await refreshConfig();
            }}
          />
        </Field>
        <Field label={t("settings.theme")} group>
          <Seg<Theme>
            value={theme}
            onChange={(v) => {
              setTheme(v);
              setThemeState(v);
            }}
            options={[
              ["system", t("settings.theme.system")],
              ["light", t("settings.theme.light")],
              ["dark", t("settings.theme.dark")],
            ]}
          />
        </Field>
        <p class="muted small">{t("settings.themeNote")}</p>
      </div>
    </Card>
  );
}

function DeviceCard() {
  const sys = config.value!.system;
  const [name, setName] = useState(sys.name);
  const [tz, setTz] = useState(sys.timezone);
  const zone = timeZone(tz);
  const browserZone = Intl.DateTimeFormat().resolvedOptions().timeZone;
  return (
    <Card title={t("settings.device")} icon={<InfoIcon size={18} />}>
      <div class="stack">
        <div class="form-grid">
          <Field label={t("settings.name")} hint={t("settings.nameHint")}>
            <input class="input" value={name} maxLength={40} onInput={(e) => setName((e.target as HTMLInputElement).value)} name="name" />
          </Field>
          <Field label={t("settings.timezone")} error={tz && !zone ? t("settings.timezoneInvalid") : null} hint={t("settings.timezoneHint", { zone: browserZone })}>
            <input class="input" value={tz} onInput={(e) => setTz((e.target as HTMLInputElement).value)} name="timezone" autoCapitalize="off" spellcheck={false} />
          </Field>
        </div>
        <div>
          <Button
            variant="primary"
            disabled={!name.trim() || !zone}
            onClick={async () => {
              await put("/system", { name: name.trim(), timezone: zone });
              await refreshConfig();
              toast(t("settings.saved"));
            }}
          >
            {t("common.save")}
          </Button>
        </div>
      </div>
    </Card>
  );
}

const hhmm = (min: number) => `${String(Math.floor(min / 60)).padStart(2, "0")}:${String(min % 60).padStart(2, "0")}`;
const minutes = (v: string) => {
  const m = /^(\d{2}):(\d{2})$/.exec(v);
  return m ? Number(m[1]) * 60 + Number(m[2]) : null;
};

// Quiet hours and what the robot notified of late (SD-123): at night only critical alarms; the rest
// comes in the morning report when the quiet hours end
function NotifyCard() {
  const n = config.value!.notify;
  const [from, setFrom] = useState(hhmm(n.quietFromMin));
  const [to, setTo] = useState(hhmm(n.quietToMin));
  const [recent, setRecent] = useState<Notice[]>([]);
  const eventId = state.value?.eventId;
  useEffect(() => {
    get<{ notifications: Notice[] }>("/notifications")
      .then((r) => setRecent(r.notifications))
      .catch(() => {});
  }, [eventId]);
  const f = minutes(from), u = minutes(to);
  const save = async () => {
    try {
      await put("/notify", { quietFromMin: f, quietToMin: u });
      await refreshConfig();
      toast(t("settings.saved"));
    } catch (e) {
      toastError(e);
    }
  };
  const tone = (l: Notice["level"]) => (l === "critical" ? "bad" : l === "warning" ? "warn" : "info");
  return (
    <Card title={t("settings.notify")} icon={<Bell size={18} />}>
      <div class="stack">
        <div class="form-grid">
          <Field label={t("settings.quietFrom")}>
            <input class="input" type="time" value={from} onInput={(e) => setFrom((e.target as HTMLInputElement).value)} name="quietFrom" />
          </Field>
          <Field label={t("settings.quietTo")}>
            <input class="input" type="time" value={to} onInput={(e) => setTo((e.target as HTMLInputElement).value)} name="quietTo" />
          </Field>
        </div>
        <p class="muted small">{f === u ? t("settings.quietNone") : t("settings.quietHint", { to })}</p>
        <div>
          <Button variant="primary" onClick={save} disabled={f === null || u === null} data-testid="save-quiet">
            {t("common.save")}
          </Button>
        </div>
        <div class="section-title">{t("settings.recent")}</div>
        {recent.length ? (
          <div class="list" data-testid="notifications">
            {recent.map((x) => (
              <div class="item wrap-row">
                <div class="grow">
                  <div class="title">
                    <Pill tone={tone(x.level)}>{t(`status.level.${x.level}`)}</Pill> {msg(x.title)}
                  </div>
                  {x.lines.map((l) => (
                    <div class="small">{msg(l)}</div>
                  ))}
                  <div class="muted small">{dateTime(x.at)}</div>
                </div>
              </div>
            ))}
          </div>
        ) : (
          <p class="muted small">{t("settings.recentNone")}</p>
        )}
        <p class="muted small">{t("settings.recentNote")}</p>
      </div>
    </Card>
  );
}

type Channel = "ntfy" | "telegram" | "email";

// The push channels without an account (SD-124): a secret is only ever sent, never shown; left
// empty it stays as it is
function PushCard() {
  const [p, setP] = useState<PushSettings | null>(null);
  const [secret, setSecret] = useState<Record<string, string>>({});
  const load = () =>
    get<PushSettings>("/push")
      .then(setP)
      .catch(() => {});
  useEffect(() => {
    load();
  }, []);
  // How a test went comes back a moment later
  const eventId = state.value?.eventId;
  useEffect(() => {
    load();
  }, [eventId]);
  if (!p) return null;
  const set = <C extends Channel>(ch: C, patch: Partial<PushSettings[C]>) => setP({ ...p, [ch]: { ...p[ch], ...patch } });
  const field = (ch: Channel, key: string, label: string, opts: { placeholder?: string; email?: boolean; hint?: string } = {}) => (
    <Field label={label} hint={opts.hint}>
      <input
        class="input"
        inputMode={opts.email ? "email" : undefined}
        name={`${ch}-${key}`}
        value={(p[ch] as Record<string, unknown>)[key] as string}
        placeholder={opts.placeholder}
        autoCapitalize="off"
        spellcheck={false}
        onInput={(e) => set(ch, { [key]: (e.target as HTMLInputElement).value } as never)}
      />
    </Field>
  );
  const secretField = (ch: Channel, key: string, label: string, isSet: boolean) => (
    <Field label={label} hint={isSet ? t("settings.push.secretSet") : undefined}>
      <input
        class="input"
        type="password"
        name={`${ch}-${key}`}
        autoComplete="new-password"
        value={secret[`${ch}.${key}`] ?? ""}
        placeholder={isSet ? "••••••••" : ""}
        onInput={(e) => setSecret({ ...secret, [`${ch}.${key}`]: (e.target as HTMLInputElement).value })}
      />
    </Field>
  );
  const save = async () => {
    const body: Record<string, Record<string, unknown>> = {
      ntfy: { enabled: p.ntfy.enabled, server: p.ntfy.server, topic: p.ntfy.topic },
      telegram: { enabled: p.telegram.enabled, chatId: p.telegram.chatId },
      email: { enabled: p.email.enabled, server: p.email.server, from: p.email.from, to: p.email.to, user: p.email.user },
    };
    for (const [k, v] of Object.entries(secret)) {
      const [ch, key] = k.split(".");
      if (v) body[ch][key] = v;
    }
    try {
      setP(await put<PushSettings>("/push", body));
      setSecret({});
      toast(t("settings.saved"));
    } catch (e) {
      toastError(e);
    }
  };
  const test = async () => {
    try {
      await post("/push/test");
      toast(t("settings.push.testSent"));
    } catch (e) {
      toastError(e);
    }
  };
  const status = (ch: Channel) => {
    const s = p.status[ch];
    if (!s) return null;
    return (
      <div class="small" data-testid={`push-status-${ch}`}>
        {s.ok ? (
          <Pill tone="ok">{t("settings.push.ok", { time: dateTime(s.at) })}</Pill>
        ) : (
          <>
            <Pill tone="bad">{t("settings.push.failed", { time: dateTime(s.at) })}</Pill> <span class="muted">{s.error}</span>
          </>
        )}
      </div>
    );
  };
  const section = (ch: Channel, title: string, body: preact.ComponentChildren) => (
    <div class="stack-sm" data-testid={`push-${ch}`}>
      <div class="row" style="justify-content:space-between">
        <div class="section-title">{title}</div>
        <Toggle checked={p[ch].enabled} onChange={(v) => set(ch, { enabled: v } as never)} label={t("settings.push.enabled")} />
      </div>
      {body}
      {status(ch)}
    </div>
  );
  return (
    <Card title={t("settings.push")} icon={<Send size={18} />}>
      <div class="stack">
        <p class="muted small">{t("settings.push.intro")}</p>
        {section(
          "ntfy",
          "ntfy",
          <div class="form-grid">
            {field("ntfy", "server", t("settings.push.server"), { placeholder: "https://ntfy.sh" })}
            {field("ntfy", "topic", t("settings.push.topic"), { hint: t("settings.push.topicHint") })}
            {secretField("ntfy", "token", t("settings.push.token"), p.ntfy.tokenSet)}
          </div>,
        )}
        {section(
          "telegram",
          "Telegram",
          <div class="form-grid">
            {field("telegram", "chatId", t("settings.push.chatId"))}
            {secretField("telegram", "token", t("settings.push.botToken"), p.telegram.tokenSet)}
          </div>,
        )}
        {section(
          "email",
          t("settings.push.email"),
          <div class="form-grid">
            {field("email", "server", t("settings.push.mailServer"), { placeholder: "smtps://mail.example.org:465" })}
            {field("email", "from", t("settings.push.from"), { email: true })}
            {field("email", "to", t("settings.push.to"), { email: true })}
            {field("email", "user", t("settings.push.user"))}
            {secretField("email", "password", t("settings.push.password"), p.email.passwordSet)}
          </div>,
        )}
        <div class="row">
          <Button variant="primary" onClick={save} data-testid="save-push">
            {t("common.save")}
          </Button>
          <Button onClick={test} disabled={!p.ntfy.enabled && !p.telegram.enabled && !p.email.enabled} data-testid="test-push">
            {t("settings.push.test")}
          </Button>
        </div>
        <p class="muted small">{t("settings.push.curl")}</p>
      </div>
    </Card>
  );
}

function PasswordCard() {
  const [old, setOld] = useState("");
  const [pw, setPw] = useState("");
  const [pw2, setPw2] = useState("");
  const mismatch = pw2 !== "" && pw !== pw2;
  return (
    <Card title={t("settings.password")} icon={<KeyRound size={18} />}>
      <form
        class="stack"
        onSubmit={(e) => e.preventDefault()}
      >
        <div class="form-grid">
          <Field label={t("settings.oldPassword")}>
            <input class="input" type="password" autoComplete="current-password" value={old} onInput={(e) => setOld((e.target as HTMLInputElement).value)} name="old" />
          </Field>
          <Field label={t("login.newPassword")} hint={t("login.minLength")}>
            <input class="input" type="password" autoComplete="new-password" value={pw} onInput={(e) => setPw((e.target as HTMLInputElement).value)} name="new" />
          </Field>
          <Field label={t("login.repeatPassword")} error={mismatch ? t("login.mismatch") : null}>
            <input class="input" type="password" autoComplete="new-password" value={pw2} onInput={(e) => setPw2((e.target as HTMLInputElement).value)} name="new2" />
          </Field>
        </div>
        <p class="muted small">{t("settings.passwordNote")}</p>
        <div>
          <Button
            variant="primary"
            type="submit"
            disabled={!old || pw.length < 8 || pw !== pw2}
            onClick={async () => {
              await put("/auth/password", { old, new: pw });
              toast(t("settings.passwordChanged"));
              logoutLocal();
            }}
          >
            {t("settings.changePassword")}
          </Button>
        </div>
      </form>
    </Card>
  );
}

// Coupling two devices on one tank (PD-457, SD-131): the second device shows a code, the leading
// one pairs with it; each then holds a token for the other. Either side ends it.
function CouplingCard() {
  const [c, setC] = useState<Coupling | null>(null);
  const [code, setCode] = useState<string | null>(null);
  const [address, setAddress] = useState("");
  const [typed, setTyped] = useState("");
  const load = () => get<Coupling>("/coupling").then(setC).catch(() => {});
  useEffect(() => {
    load();
  }, []);
  // While a pairing is under way, ask again until it is decided; while coupled, the link now and then
  useEffect(() => {
    if (c?.pairing?.state !== "pairing" && !c?.peer) return;
    const id = window.setTimeout(load, c?.peer ? 5000 : 1000);
    return () => window.clearTimeout(id);
  }, [c]);
  if (!c) return null;
  const end = async () => {
    if (!confirm(t("settings.coupling.endAsk", { name: c.peer!.name }))) return;
    setC(await del<Coupling>("/coupling"));
    toast(t("settings.coupling.ended"));
  };
  return (
    <Card title={t("settings.coupling")} icon={<Link2 size={18} />}>
      <div class="stack" data-testid="coupling">
        <p class="muted small">{t("settings.coupling.intro")}</p>
        {c.peer ? (
          <>
            <Banner tone="ok">
              <span data-testid="coupled">
                {c.peer.role === "leader" ? t("settings.coupling.leads", { name: c.peer.name }) : t("settings.coupling.follows", { name: c.peer.name })}
              </span>
            </Banner>
            <div class="kv">
              <span>{t("settings.deviceId")}</span>
              <span class="mono">{c.peer.id}</span>
              {c.peer.address && (
                <>
                  <span>{t("settings.coupling.address")}</span>
                  <span class="mono">{c.peer.address}</span>
                </>
              )}
              <span>{t("settings.coupling.since")}</span>
              <span>{dateTime(c.peer.since)}</span>
              <span>{t("settings.coupling.link")}</span>
              <span data-testid="link">
                {c.link?.ok ? (
                  <Pill tone="ok">{t("settings.coupling.linkOk")}</Pill>
                ) : (
                  <Pill tone="warn">{c.link?.heard ? t("settings.coupling.linkLost", { time: dateTime(c.link.heard) }) : t("settings.coupling.linkNone")}</Pill>
                )}
              </span>
            </div>
            <p class="muted small">{t(c.peer.role === "follower" ? "settings.coupling.followHint" : "settings.coupling.later")}</p>
            <div>
              <Button variant="danger-soft" onClick={end} data-testid="end-coupling">
                {t("settings.coupling.end")}
              </Button>
            </div>
          </>
        ) : (
          <>
            <div class="stack-sm">
              <div class="section-title">{t("settings.coupling.asSecond")}</div>
              <p class="muted small">{t("settings.coupling.asSecondHint")}</p>
              {code && c.code ? (
                <p>
                  <code class="mono token" data-testid="pairing-code">{code}</code>{" "}
                  <span class="muted small">{t("settings.coupling.codeUntil", { time: dateTime(c.code.until) })}</span>
                </p>
              ) : (
                <div>
                  <Button
                    onClick={async () => {
                      const r = await post<{ code: string; until: number }>("/coupling/code");
                      setCode(r.code);
                      await load();
                    }}
                    data-testid="show-code"
                  >
                    {t("settings.coupling.showCode")}
                  </Button>
                </div>
              )}
            </div>
            <form class="stack-sm" onSubmit={(e) => e.preventDefault()}>
              <div class="section-title">{t("settings.coupling.asLeader")}</div>
              <div class="form-grid">
                <Field label={t("settings.coupling.address")} hint={t("settings.coupling.addressHint")}>
                  <input class="input" name="peer-address" value={address} placeholder="192.168.1.20" autoCapitalize="off" spellcheck={false} onInput={(e) => setAddress((e.target as HTMLInputElement).value)} />
                </Field>
                <Field label={t("settings.coupling.code")}>
                  <input class="input mono" name="peer-code" value={typed} placeholder="XXXX-XXXX" autoCapitalize="characters" spellcheck={false} onInput={(e) => setTyped((e.target as HTMLInputElement).value)} />
                </Field>
              </div>
              {c.pairing?.state === "pairing" && <p class="muted small" data-testid="pairing-state">{t("settings.coupling.pairing", { address: c.pairing.address })}</p>}
              {c.pairing?.state === "failed" && c.pairing.error && (
                <Banner tone="bad">
                  <span data-testid="pairing-state">{msg(c.pairing.error)}</span>
                </Banner>
              )}
              <div>
                <Button
                  variant="primary"
                  type="submit"
                  disabled={!address.trim() || !typed.trim() || c.pairing?.state === "pairing"}
                  data-testid="pair"
                  onClick={async () => {
                    await post("/coupling/pair", { address: address.trim(), code: typed.trim() });
                    setTyped("");
                    await load();
                  }}
                >
                  {t("settings.coupling.pair")}
                </Button>
              </div>
            </form>
          </>
        )}
      </div>
    </Card>
  );
}

// MQTT with Home Assistant discovery (PD-461, SD-130): off by default. Values go out also without
// TLS; commands (Stop, the recipe in force, its targets) only over TLS with a user.
function MqttCard() {
  const [m, setM] = useState<MqttSettings | null>(null);
  const [password, setPassword] = useState("");
  const load = () => get<MqttSettings>("/mqtt").then(setM).catch(() => {});
  useEffect(() => {
    load();
  }, []);
  if (!m) return null;
  const set = (patch: Partial<MqttSettings>) => setM({ ...m, ...patch });
  const canCommand = m.tls && m.user.trim() !== "";
  const save = async () => {
    const body: Record<string, unknown> = { enabled: m.enabled, host: m.host.trim(), port: m.port, tls: m.tls, user: m.user, commands: m.commands && canCommand, discovery: m.discovery };
    if (password) body.password = password;
    try {
      setM(await put<MqttSettings>("/mqtt", body));
      setPassword("");
      toast(t("settings.saved"));
    } catch (e) {
      toastError(e);
    }
  };
  return (
    <Card title={t("settings.mqtt")} icon={<House size={18} />}>
      <div class="stack" data-testid="mqtt">
        <p class="muted small">{t("settings.mqtt.intro")}</p>
        <Toggle checked={m.enabled} onChange={(v) => set({ enabled: v })} label={t("settings.mqtt.enabled")} />
        <div class="form-grid">
          <Field label={t("settings.mqtt.host")}>
            <input class="input" name="mqtt-host" value={m.host} placeholder="homeassistant.local" autoCapitalize="off" spellcheck={false} onInput={(e) => set({ host: (e.target as HTMLInputElement).value })} />
          </Field>
          <Field label={t("settings.mqtt.port")}>
            <input class="input" name="mqtt-port" inputMode="numeric" value={String(m.port)} onInput={(e) => set({ port: Number((e.target as HTMLInputElement).value) || 0 })} />
          </Field>
          <Field label={t("settings.push.user")}>
            <input class="input" name="mqtt-user" value={m.user} autoCapitalize="off" spellcheck={false} onInput={(e) => set({ user: (e.target as HTMLInputElement).value })} />
          </Field>
          <Field label={t("settings.push.password")} hint={m.passwordSet ? t("settings.push.secretSet") : undefined}>
            <input class="input" type="password" name="mqtt-password" autoComplete="new-password" value={password} placeholder={m.passwordSet ? "••••••••" : ""} onInput={(e) => setPassword((e.target as HTMLInputElement).value)} />
          </Field>
        </div>
        <Toggle checked={m.tls} onChange={(v) => set({ tls: v, port: v && m.port === 1883 ? 8883 : !v && m.port === 8883 ? 1883 : m.port })} label={t("settings.mqtt.tls")} />
        {m.enabled && !m.tls && <Banner tone="warn">{t("settings.mqtt.plain")}</Banner>}
        <Toggle checked={m.commands && canCommand} onChange={(v) => set({ commands: v })} label={t("settings.mqtt.commands")} disabled={!canCommand} />
        <p class="muted small">{canCommand ? t("settings.mqtt.commandsHint") : t("settings.mqtt.commandsNeed")}</p>
        {m.enabled && (
          <p class="small" data-testid="mqtt-status">
            {m.status.connected ? (
              <Pill tone="ok">{t("settings.mqtt.connected")}</Pill>
            ) : m.status.error ? (
              <>
                <Pill tone="bad">{t("settings.mqtt.failed")}</Pill> <span class="muted">{msg(m.status.error)}</span>
              </>
            ) : (
              <Pill>{t("settings.mqtt.waiting")}</Pill>
            )}{" "}
            <span class="muted">{t("settings.mqtt.topic", { topic: m.topic })}</span>
          </p>
        )}
        <div>
          <Button variant="primary" onClick={save} data-testid="save-mqtt">
            {t("common.save")}
          </Button>
        </div>
      </div>
    </Card>
  );
}

// Network devices (SD-132): Shelly Gen2+ and ESPHome devices by their address, and the roles bound
// to them. An output only on a Shelly; the robot reads its safety settings back before it switches it.
function DevicesCard() {
  const [d, setD] = useState<Devices | null>(null);
  const [kind, setKind] = useState<"shelly" | "esphome">("shelly");
  const [address, setAddress] = useState("");
  const [name, setName] = useState("");
  const [login, setLogin] = useState<{ id: string; user: string; password: string } | null>(null);
  const load = () => get<Devices>("/devices").then(setD).catch(() => {});
  useEffect(() => {
    load();
    const id = window.setInterval(load, 5000);
    return () => window.clearInterval(id);
  }, []);
  if (!d) return null;
  const add = async () => {
    try {
      await post("/devices", { kind, address: address.trim(), name: name.trim() });
      setAddress("");
      setName("");
      await refreshConfig();
      await load();
      toast(t("settings.saved"));
    } catch (e) {
      toastError(e);
    }
  };
  const remove = async (id: string, label: string) => {
    if (!confirm(t("settings.devices.removeAsk", { name: label }))) return;
    try {
      await del(`/devices/${encodeURIComponent(id)}`);
      await refreshConfig();
      await load();
    } catch (e) {
      toastError(e);
    }
  };
  const saveLogin = async () => {
    if (!login) return;
    try {
      await put(`/devices/${encodeURIComponent(login.id)}/credentials`, { user: login.user, password: login.password });
      setLogin(null);
      await load();
      toast(t("settings.saved"));
    } catch (e) {
      toastError(e);
    }
  };
  return (
    <Card title={t("settings.devices")} icon={<Cable size={18} />}>
      <div class="stack" data-testid="devices">
        <p class="muted small">{t("settings.devices.intro")}</p>
        {d.devices.length > 0 && (
          <div class="list">
            {d.devices.map((x) => (
              <div class="item wrap-row" data-testid={`device-${x.id}`}>
                <div class="grow">
                  <div class="title">
                    {x.name} <span class="muted small">{x.kind === "shelly" ? "Shelly" : "ESPHome"} · <span class="mono">{x.address}</span></span>
                  </div>
                  <div class="small">
                    {!x.polled ? (
                      <Pill>{t("settings.devices.unused")}</Pill>
                    ) : x.online ? (
                      <Pill tone="ok">{t("settings.devices.online")}</Pill>
                    ) : (
                      <Pill tone="bad">{t("settings.devices.offline")}</Pill>
                    )}{" "}
                    {x.problem && <span class="muted">{msg(x.problem)}</span>}
                  </div>
                  {login?.id === x.id && (
                    <div class="form-grid">
                      {x.kind === "esphome" && (
                        <Field label={t("settings.push.user")}>
                          <input class="input" name="device-user" value={login.user} autoCapitalize="off" spellcheck={false} onInput={(e) => setLogin({ ...login, user: (e.target as HTMLInputElement).value })} />
                        </Field>
                      )}
                      <Field label={t("settings.push.password")} hint={t("settings.devices.passwordHint")}>
                        <input class="input" type="password" name="device-password" autoComplete="new-password" value={login.password} onInput={(e) => setLogin({ ...login, password: (e.target as HTMLInputElement).value })} />
                      </Field>
                      <div class="row">
                        <Button size="sm" variant="primary" onClick={saveLogin}>
                          {t("common.save")}
                        </Button>
                        <Button size="sm" onClick={() => setLogin(null)}>
                          {t("common.cancel")}
                        </Button>
                      </div>
                    </div>
                  )}
                </div>
                <div class="row-actions">
                  <Button size="sm" onClick={() => setLogin({ id: x.id, user: "", password: "" })}>
                    {x.hasPassword ? t("settings.devices.passwordSet") : t("settings.devices.password")}
                  </Button>
                  <Button size="sm" onClick={() => remove(x.id, x.name)}>
                    {t("settings.devices.remove")}
                  </Button>
                </div>
              </div>
            ))}
          </div>
        )}
        <div class="form-grid">
          <Field label={t("settings.devices.kind")}>
            <Seg value={kind} onChange={setKind} options={[["shelly", "Shelly"], ["esphome", "ESPHome"]]} />
          </Field>
          <Field label={t("settings.devices.address")}>
            <input class="input" name="device-address" value={address} placeholder="shelly-1.local" autoCapitalize="off" spellcheck={false} onInput={(e) => setAddress((e.target as HTMLInputElement).value)} />
          </Field>
          <Field label={t("settings.devices.name")}>
            <input class="input" name="device-name" value={name} maxLength={40} onInput={(e) => setName((e.target as HTMLInputElement).value)} />
          </Field>
        </div>
        <div>
          <Button variant="primary" onClick={add} disabled={!address.trim() || !name.trim()} data-testid="add-device">
            {t("settings.devices.add")}
          </Button>
        </div>
        <div class="section-title">{t("settings.devices.roles")}</div>
        {DEVICE_ROLES.map((r) => (
          <RoleRow key={`${r}${JSON.stringify(d.roles[r]?.binding)}`} role={r} devices={d} reload={load} />
        ))}
        {d.roles.refill_valve?.binding && <RefillPanel />}
        {d.roles.circulation_pump?.binding && <CirculationPanel />}
      </div>
    </Card>
  );
}

// The circulation pump (SD-132): with each dosing round and fill until mixed in, or always; never
// during Stop, maintenance or a hold
function CirculationPanel() {
  const save = async (mode: CirculationMode) => {
    try {
      await put("/circulation", { mode });
      await refreshConfig();
      toast(t("settings.saved"));
    } catch (e) {
      toastError(e);
    }
  };
  return (
    <div class="stack-sm" data-testid="circulation">
      <div class="section-title" id="circulation-mode">
        {t("settings.circulation")}
      </div>
      <Seg<CirculationMode>
        value={config.value!.circulation.mode}
        onChange={save}
        labelledBy="circulation-mode"
        options={[
          ["mixing", t("settings.circulation.mixing")],
          ["always", t("settings.circulation.always")],
        ]}
      />
      <p class="muted small">{t("settings.circulation.intro")}</p>
    </div>
  );
}

// The refill valve (SD-132): whether a fill runs or why it cannot start, "Fill now" and stop, and
// the longest fill. A fill also starts on its own when "top up water" is raised.
function RefillPanel() {
  const r = state.value!.refill;
  const [max, setMax] = useState(String(config.value!.refill.maxFillMin));
  const act = async (path: string) => {
    try {
      await post(path);
    } catch (e) {
      toastError(e);
    }
  };
  const save = async () => {
    try {
      await put("/refill", { maxFillMin: Number(max.replace(",", ".")) });
      await refreshConfig();
      toast(t("settings.saved"));
    } catch (e) {
      toastError(e);
    }
  };
  return (
    <div class="stack-sm" data-testid="refill">
      <div class="section-title">{t("settings.refill")}</div>
      <p class="muted small">{t("settings.refill.intro")}</p>
      <div class="row" style="justify-content:space-between">
        <span class="small" data-testid="refill-state">
          {r.filling ? (
            <Pill tone="info">{t(r.opened ? "settings.refill.filling" : "settings.refill.opening")}</Pill>
          ) : r.blocked ? (
            <span class="muted">{msg(r.blocked)}</span>
          ) : (
            <Pill tone="ok">{t("settings.refill.ready")}</Pill>
          )}
        </span>
        {r.filling ? (
          <Button size="sm" onClick={() => act("/refill/stop")} data-testid="refill-stop">
            {t("settings.refill.stop")}
          </Button>
        ) : (
          <Button size="sm" variant="primary" onClick={() => act("/refill/start")} disabled={!!r.blocked} data-testid="refill-start">
            {t("settings.refill.start")}
          </Button>
        )}
      </div>
      <div class="form-grid">
        <Field label={t("settings.refill.maxFill")} hint={t("settings.refill.maxFillHint")}>
          <input class="input" name="refill-max" inputMode="decimal" value={max} onInput={(e) => setMax((e.target as HTMLInputElement).value)} />
        </Field>
      </div>
      <div>
        <Button size="sm" onClick={save} data-testid="save-refill">
          {t("common.save")}
        </Button>
      </div>
    </div>
  );
}

// A role bound to a device: a fixed one here, a watering circuit's output on its page (SD-134)
export function RoleRow(p: { role: string; label?: string; devices: Devices; reload: () => unknown }) {
  const info = p.devices.roles[p.role];
  const output = info?.kind === "output";
  const blank: RoleBinding = { source: "shelly", device: "", channel: 0, entity: "", field: "", invert: false, availability: "" };
  const [b, setB] = useState<RoleBinding>(info?.binding ?? blank);
  const set = (patch: Partial<RoleBinding>) => setB({ ...b, ...patch });
  const fits = p.devices.devices.filter((x) => x.kind === b.source);
  const state = output ? info?.output?.state : info?.state;
  const problem = output ? info?.output?.problem : null;
  const save = async () => {
    try {
      await put(`/roles/${p.role}`, b);
      await refreshConfig();
      await p.reload();
      toast(t("settings.saved"));
    } catch (e) {
      toastError(e);
    }
  };
  const release = async () => {
    try {
      await del(`/roles/${p.role}`);
      await refreshConfig();
      await p.reload();
    } catch (e) {
      toastError(e);
    }
  };
  const sources: [RoleSource, string][] = output ? [["shelly", "Shelly"]] : [["shelly", "Shelly"], ["esphome", "ESPHome"], ["mqtt", "MQTT"]];
  return (
    <div class="stack-sm" data-testid={`role-${p.role}`}>
      <div class="row" style="justify-content:space-between">
        <strong>{p.label ?? t(`settings.devices.role.${p.role}` as TextKey)}</strong>
        <span class="small" data-testid={`role-state-${p.role}`}>
          {info?.binding ? (
            <Pill tone={state === "ok" || state === "on" || state === "off" ? "ok" : state === "checking" || state === "waiting" ? "info" : "bad"}>
              {t(`settings.devices.state.${state ?? "not_assigned"}` as TextKey)}
              {!output && state === "ok" ? `: ${typeof info?.value === "boolean" ? t(info.value ? "settings.devices.yes" : "settings.devices.no") : String(info?.value)}` : ""}
            </Pill>
          ) : (
            <span class="muted">{t("settings.devices.state.not_assigned")}</span>
          )}
        </span>
      </div>
      {problem && <span class="muted small">{msg(problem)}</span>}
      <div class="form-grid">
        {sources.length > 1 && (
          <Field label={t("settings.devices.source")}>
            <Seg<RoleSource> value={b.source} onChange={(source) => set({ source, device: "", entity: "", field: "", availability: "" })} options={sources} />
          </Field>
        )}
        {b.source !== "mqtt" && (
          <Field label={t("settings.devices.device")}>
            <select class="input" name={`role-device-${p.role}`} value={b.device} onChange={(e) => set({ device: (e.target as HTMLSelectElement).value })}>
              <option value="">–</option>
              {fits.map((x) => (
                <option value={x.id}>{x.name}</option>
              ))}
            </select>
          </Field>
        )}
        {b.source === "shelly" && (
          <Field label={output ? t("settings.devices.output") : t("settings.devices.input")}>
            <input class="input" name={`role-channel-${p.role}`} inputMode="numeric" value={String(b.channel)} onInput={(e) => set({ channel: Number((e.target as HTMLInputElement).value) || 0 })} />
          </Field>
        )}
        {b.source === "esphome" && (
          <Field label={t("settings.devices.entity")}>
            <input class="input mono" name={`role-entity-${p.role}`} value={b.entity} placeholder="binary_sensor/tank_full" autoCapitalize="off" spellcheck={false} onInput={(e) => set({ entity: (e.target as HTMLInputElement).value })} />
          </Field>
        )}
        {b.source === "mqtt" && (
          <>
            <Field label={t("settings.devices.topic")}>
              <input class="input mono" name={`role-topic-${p.role}`} value={b.entity} autoCapitalize="off" spellcheck={false} onInput={(e) => set({ entity: (e.target as HTMLInputElement).value })} />
            </Field>
            <Field label={t("settings.devices.field")}>
              <input class="input mono" name={`role-field-${p.role}`} value={b.field} autoCapitalize="off" spellcheck={false} onInput={(e) => set({ field: (e.target as HTMLInputElement).value })} />
            </Field>
            <Field label={t("settings.devices.availability")}>
              <input class="input mono" name={`role-availability-${p.role}`} value={b.availability} autoCapitalize="off" spellcheck={false} onInput={(e) => set({ availability: (e.target as HTMLInputElement).value })} />
            </Field>
          </>
        )}
      </div>
      {!output && (
        <label class="row small" style="gap:6px">
          <Toggle checked={b.invert} onChange={(v) => set({ invert: v })} label={t("settings.devices.invert")} />
          {t("settings.devices.invert")}
        </label>
      )}
      {output && info?.pulse && <p class="muted small">{t("settings.devices.autoOff", { min: Math.round((info?.autoOffS ?? 0) / 60) })}</p>}
      {output && <p class="muted small">{t("settings.devices.detached")}</p>}
      <div class="row">
        <Button size="sm" variant="primary" onClick={save} data-testid={`save-role-${p.role}`}>
          {t("settings.devices.assign")}
        </Button>
        {info?.binding && (
          <Button size="sm" onClick={release}>
            {t("settings.devices.release")}
          </Button>
        )}
      </div>
    </div>
  );
}

// Locks a person releases may only report, for operations that watch in their own way (SD-133).
// Leak, high level, Stop and the limits always hold; the card says so.
function LocksCard() {
  const cfg = config.value!;
  const [report, setReport] = useState<string[]>(cfg.locks.reportOnly);
  const set = (kind: string, only: boolean) => setReport(only ? [...report.filter((k) => k !== kind), kind] : report.filter((k) => k !== kind));
  const same = report.length === cfg.locks.reportOnly.length && report.every((k) => cfg.locks.reportOnly.includes(k));
  const save = async () => {
    try {
      await put("/locks", { reportOnly: REPORTABLE_LOCKS.filter((k) => report.includes(k)) });
      await refreshConfig();
      toast(t("settings.saved"));
    } catch (e) {
      toastError(e);
    }
  };
  return (
    <Card title={t("settings.locks")} icon={<Lock size={18} />}>
      <div class="stack" data-testid="locks">
        <p class="muted small">{t("settings.locks.intro")}</p>
        <div class="list">
          {REPORTABLE_LOCKS.map((kind) => (
            <div class="item wrap-row" data-testid={`lock-${kind}`}>
              <div class="grow" id={`lock-${kind}`}>
                {t(`settings.locks.${kind}` as TextKey)}
              </div>
              <Seg
                value={report.includes(kind) ? "report" : "lock"}
                onChange={(v) => set(kind, v === "report")}
                labelledBy={`lock-${kind}`}
                options={[
                  ["lock", t("settings.locks.lock")],
                  ["report", t("settings.locks.report")],
                ]}
              />
            </div>
          ))}
        </div>
        {report.length > 0 && <Banner tone="warn">{t("settings.locks.warning")}</Banner>}
        <div>
          <Button variant="primary" onClick={save} disabled={same} data-testid="save-locks">
            {t("common.save")}
          </Button>
        </div>
      </div>
    </Card>
  );
}

// Access tokens (SD-129): for an app, the account and a coupled device. Each has a role that says
// what it may do; the token is shown once, the robot keeps only its hash.
function TokensCard() {
  const [list, setList] = useState<AccessTokens | null>(null);
  const [name, setName] = useState("");
  const [role, setRole] = useState<TokenRole>("app");
  const [made, setMade] = useState<NewToken | null>(null);
  const load = async () => setList(await get<AccessTokens>("/tokens"));
  useEffect(() => {
    load().catch(toastError);
  }, []);
  const full = list !== null && list.tokens.length >= list.max;
  return (
    <Card title={t("settings.tokens")} icon={<Ticket size={18} />}>
      <div class="stack">
        <p class="muted small">{t("settings.tokens.intro")}</p>
        {made && (
          <Banner tone="warn">
            <div class="stack">
              <span>{t("settings.tokens.once", { name: made.name })}</span>
              <code class="mono token" data-testid="new-token">{made.token}</code>
              <div>
                <Button
                  size="sm"
                  onClick={async () => {
                    await navigator.clipboard.writeText(made.token);
                    toast(t("settings.tokens.copied"));
                  }}
                >
                  <Copy size={16} /> {t("settings.tokens.copy")}
                </Button>
              </div>
            </div>
          </Banner>
        )}
        {list && list.tokens.length === 0 && <p class="muted">{t("settings.tokens.none")}</p>}
        {list && list.tokens.length > 0 && (
          <div class="list" data-testid="tokens">
            {list.tokens.map((tk) => (
              <div class="item wrap-row" key={tk.id} data-testid={`token-${tk.id}`}>
                <div class="grow">
                  <div class="title">
                    {tk.name} <Pill>{t(`settings.tokens.as.${tk.role}`)}</Pill>
                  </div>
                  <div class="muted small">
                    {t("settings.tokens.created", { at: dateTime(tk.created) })}
                    {" · "}
                    {tk.used ? t("settings.tokens.used", { at: dateTime(tk.used) }) : t("settings.tokens.unused")}
                  </div>
                </div>
                <div class="row-actions">
                  <Button
                    size="sm"
                    variant="danger-soft"
                    onClick={async () => {
                      if (!confirm(t("settings.tokens.revokeAsk", { name: tk.name }))) return;
                      await del(`/tokens/${encodeURIComponent(tk.id)}`);
                      if (made?.id === tk.id) setMade(null);
                      toast(t("settings.tokens.revoked"));
                      await load();
                    }}
                  >
                    {t("settings.tokens.revoke")}
                  </Button>
                </div>
              </div>
            ))}
          </div>
        )}
        <form class="stack" onSubmit={(e) => e.preventDefault()}>
          <div class="form-grid">
            <Field label={t("settings.tokens.name")} hint={t("settings.tokens.nameHint")}>
              <input class="input" value={name} maxLength={40} onInput={(e) => setName((e.target as HTMLInputElement).value)} name="tokenName" />
            </Field>
            <Field label={t("settings.tokens.role")} group>
              <Seg value={role} onChange={setRole} options={TOKEN_ROLES.map((r) => [r, t(`settings.tokens.as.${r}`)] as [TokenRole, string])} />
            </Field>
          </div>
          <p class="muted small">{t(`settings.tokens.role.${role}`)}</p>
          <div>
            <Button
              variant="primary"
              type="submit"
              disabled={!name.trim() || full}
              data-testid="create-token"
              onClick={async () => {
                setMade(await post<NewToken>("/tokens", { name: name.trim(), role }));
                setName("");
                await load();
              }}
            >
              {t("settings.tokens.create")}
            </Button>
          </div>
          {full && <p class="muted small">{t("settings.tokens.full", { max: list!.max })}</p>}
        </form>
      </div>
    </Card>
  );
}

// Backup and restore: the configuration as a file, and back. The robot checks
// a file like any change and says every reason it refuses one.
function BackupCard() {
  const [file, setFile] = useState<{ name: string; data: object } | null>(null);
  const [errors, setErrors] = useState<Msg[]>([]);
  return (
    <Card title={t("settings.backup")} icon={<DatabaseBackup size={18} />}>
      <div class="stack" data-testid="backup">
        <p class="muted">{t("settings.backup.intro")}</p>
        <div class="row">
          <a class="btn" href="/api/v1/config/export" download data-testid="backup-download">
            <Download size={16} /> {t("settings.backup.save")}
          </a>
          <label class="btn">
            <Upload size={16} /> {t("settings.backup.choose")}
            <input
              type="file"
              accept="application/json,.json"
              class="sr-only"
              name="restore"
              onChange={async (e) => {
                const input = e.target as HTMLInputElement;
                const f = input.files?.[0];
                input.value = "";
                setErrors([]);
                if (!f) return;
                try {
                  const data = JSON.parse(await f.text());
                  if (data === null || typeof data !== "object" || Array.isArray(data)) throw new SyntaxError();
                  setFile({ name: f.name, data });
                } catch {
                  setFile(null);
                  toast(t("settings.backup.notJson"), "error");
                }
              }}
            />
          </label>
        </div>
        {file && (
          <Banner tone="warn">
            <div class="stack">
              <span>{t("settings.backup.confirm", { file: file.name })}</span>
              <div class="row">
                <Button
                  variant="primary"
                  onClick={async () => {
                    try {
                      await post("/config/import", file.data);
                    } catch (e) {
                      const list = e instanceof ApiError ? (e.body?.errors as Msg[] | undefined) : undefined;
                      if (!list?.length) throw e;
                      setErrors(list);
                      return;
                    }
                    setFile(null);
                    await refreshConfig();
                    toast(t("settings.backup.restored"));
                  }}
                >
                  {t("settings.backup.restore")}
                </Button>
                <Button onClick={() => setFile(null)}>{t("common.cancel")}</Button>
              </div>
            </div>
          </Banner>
        )}
        {errors.length > 0 && (
          <Banner tone="bad">
            <div class="stack" data-testid="backup-errors">
              <span>{t("settings.backup.refused")}</span>
              <ul>
                {errors.map((m, i) => (
                  <li key={i}>{msg(m)}</li>
                ))}
              </ul>
            </div>
          </Banner>
        )}
      </div>
    </Card>
  );
}

// Where a report goes: the public bug form, filled in from here
export const REPO = "https://github.com/Wokesay/doseph-software";

type Diagnostics = { reportId: string; info: { version: string }; events: unknown[] };

// Report a problem: the robot packs its states and recent events, never a
// secret; the owner looks at it, downloads it and attaches it to the report.
function ReportCard() {
  const [what, setWhat] = useState("");
  const [pkg, setPkg] = useState<{ d: Diagnostics; text: string } | null>(null);
  const [shown, setShown] = useState(false);
  const download = () => {
    const url = URL.createObjectURL(new Blob([pkg!.text], { type: "application/json" }));
    const a = document.createElement("a");
    a.href = url;
    a.download = `doseph-diagnostics-${pkg!.d.reportId}.json`;
    a.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  };
  const issue = () => {
    const q = new URLSearchParams({ template: "bug.yml", version: info.value?.version ?? "" });
    if (pkg) q.set("report", pkg.d.reportId);
    const text = what.trim();
    if (text) {
      q.set("title", text.split("\n")[0].slice(0, 80));
      q.set("what", text.slice(0, 2000));
    }
    return `${REPO}/issues/new?${q}`;
  };
  return (
    <Card title={t("settings.report")} icon={<Bug size={18} />}>
      <div class="stack" data-testid="report">
        <p class="muted">{t("settings.report.intro")}</p>
        <Field label={t("settings.report.what")}>
          <textarea class="input" rows={3} maxLength={2000} value={what} onInput={(e) => setWhat((e.target as HTMLTextAreaElement).value)} name="what" />
        </Field>
        <div>
          <Button
            onClick={async () => {
              const d = await get<Diagnostics>("/diagnostics");
              setPkg({ d, text: JSON.stringify(d, null, 2) });
            }}
          >
            {t(pkg ? "settings.report.again" : "settings.report.create")}
          </Button>
        </div>
        {pkg && (
          <>
            <p data-testid="report-id">
              {t("settings.report.ready", { id: pkg.d.reportId, events: pkg.d.events.length, kb: Math.max(1, Math.round(pkg.text.length / 1024)) })}
            </p>
            <p class="muted small">{t("settings.report.holds")}</p>
            <div class="row wrap">
              <Button onClick={() => setShown(!shown)} aria-expanded={shown}>
                {t(shown ? "settings.report.hide" : "settings.report.show")}
              </Button>
              <Button onClick={download} data-testid="report-download">
                <Download size={16} /> {t("settings.report.download")}
              </Button>
            </div>
            {shown && <pre class="code" data-testid="report-contents">{pkg.text}</pre>}
          </>
        )}
        <p class="muted small">{t("settings.report.steps")}</p>
        <div>
          <a class="btn primary" href={issue()} target="_blank" rel="noopener noreferrer" data-testid="report-issue">
            <ExternalLink size={16} /> {t("settings.report.github")}
          </a>
        </div>
        <p class="muted small">
          {t("settings.report.security")}{" "}
          <a href={`${REPO}/blob/main/SECURITY.md`} target="_blank" rel="noopener noreferrer">SECURITY.md</a>
        </p>
      </div>
    </Card>
  );
}

export function SettingsPage() {
  const rev = config.value!.revision;
  return (
    <div class="stack">
      <div class="grid-2">
        <LookCard />
        <DeviceCard key={rev} />
      </div>
      <NotifyCard key={`n${rev}`} />
      <PushCard />
      <MqttCard />
      <DevicesCard />
      <CouplingCard />
      <PasswordCard />
      <TokensCard />
      <LocksCard key={`l${rev}`} />
      <BackupCard />
      <ReportCard />
      <Card title={t("settings.about")}>
        <div class="stack">
          <div class="kv">
            <span>{t("settings.version")}</span>
            <span data-testid="version">{info.value?.version}</span>
            <span>{t("settings.channels")}</span>
            <span>{info.value?.channelCount}</span>
            <span>{t("settings.deviceId")}</span>
            <span data-testid="device-id">{info.value?.deviceId}</span>
          </div>
          <Banner tone="info">{t("settings.localOnly")}</Banner>
          <div>
            <Button
              onClick={async () => {
                await post("/auth/logout");
                logoutLocal();
              }}
            >
              <LogOut size={16} /> {t("settings.signOut")}
            </Button>
          </div>
        </div>
      </Card>
    </div>
  );
}
