// SPDX-License-Identifier: AGPL-3.0-or-later
// Settings: language and look, the device's name and time zone, the
// password, reporting a problem, signing out, the version.
import { useState } from "preact/hooks";
import { Bug, Download, ExternalLink, Info as InfoIcon, KeyRound, LogOut, Monitor } from "lucide-preact";
import { get, post, put } from "../api";
import { timeZone } from "../format";
import { lang, setLang, t, type Lang } from "../i18n";
import { config, info, logoutLocal, refreshConfig, toast } from "../store";
import { currentTheme, setTheme, type Theme } from "../theme";
import { Banner, Button, Card, Field, LANGS, Seg } from "../ui";

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
      <PasswordCard />
      <ReportCard />
      <Card title={t("settings.about")}>
        <div class="stack">
          <div class="kv">
            <span>{t("settings.version")}</span>
            <span data-testid="version">{info.value?.version}</span>
            <span>{t("settings.pumps")}</span>
            <span>{info.value?.pumpCount}</span>
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
