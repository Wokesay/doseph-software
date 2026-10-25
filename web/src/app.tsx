// SPDX-License-Identifier: AGPL-3.0-or-later
// Shell: navigation, header with the Stop, page choice, sign-in.
import type { ComponentChildren } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import { Droplets, FlaskConical, Gauge, History, LayoutDashboard, MoreHorizontal, OctagonX, Settings, Syringe, Waves } from "lucide-preact";
import { ApiError, post } from "./api";
import { msg, t, type TextKey } from "./i18n";
import { authed, config, info, lastUpdate, refreshState, simulated, stale, state, toast, toastError } from "./store";
import { Banner, ErrorBoundary, Modal, Pill, TestingNotice, Toasts, route } from "./ui";
import { Login } from "./pages/login";
import { StatusPage } from "./pages/status";
import { RecipePage } from "./pages/recipe";
import { TankPage } from "./pages/tank";
import { HistoryPage } from "./pages/history";
import { PumpsPage } from "./pages/pumps";
import { SettingsPage } from "./pages/settings";
import { SimPanel } from "./pages/sim";

type NavItem = { path: string; label: TextKey; icon: ComponentChildren; el: () => ComponentChildren };
const NAV: NavItem[] = [
  { path: "/", label: "nav.status", icon: <LayoutDashboard size={19} />, el: () => <StatusPage /> },
  { path: "/recipe", label: "nav.recipe", icon: <FlaskConical size={19} />, el: () => <RecipePage /> },
  { path: "/tank", label: "nav.tank", icon: <Waves size={19} />, el: () => <TankPage /> },
  { path: "/history", label: "nav.history", icon: <History size={19} />, el: () => <HistoryPage /> },
  { path: "/pumps", label: "nav.pumps", icon: <Syringe size={19} />, el: () => <PumpsPage /> },
  { path: "/settings", label: "nav.settings", icon: <Settings size={19} />, el: () => <SettingsPage /> },
];
// The bar at the bottom on phones; "More" has the rest
const MOBILE = ["/", "/recipe", "/tank", "/history"];

function NavLinks(p: { current: string; skip?: string[]; onClick?: () => void }) {
  return (
    <>
      {NAV.filter((n) => !p.skip?.includes(n.path)).map((n) => (
        <a href={`#${n.path}`} class={p.current === n.path ? "active" : ""} aria-current={p.current === n.path ? "page" : undefined} onClick={p.onClick}>
          {n.icon}
          {t(n.label)}
        </a>
      ))}
    </>
  );
}

// No answer within this time counts as not confirmed: the dialog says so and offers
// to try again, instead of waiting without a word (fetch has no time limit of its own)
const STOP_TIMEOUT_MS = 5000;

function StopButton() {
  const [ask, setAsk] = useState(false);
  const [sending, setSending] = useState(false);
  const [failed, setFailed] = useState<string | null>(null);
  const retry = useRef<HTMLButtonElement>(null);
  const open = useRef(false);  // the dialog, for a Stop that fails after it was closed
  const st = state.value;
  // A Stop that arrived after all ends the dialog, so it does not come back after Resume
  useEffect(() => {
    if (st?.stopped) {
      open.current = false;
      setAsk(false);
      setFailed(null);
    }
  }, [st?.stopped]);
  // The confirm button was disabled while sending: give it the focus back to try again
  useEffect(() => {
    if (failed && !sending) retry.current?.focus();
  }, [failed, sending]);
  if (st?.stopped)
    return (
      <button
        class="btn sm"
        data-testid="resume"
        onClick={async () => {
          try {
            await post("/resume");
            await refreshState();
            toast(t("shell.resumed"));
          } catch (e) {
            toastError(e);
          }
        }}
      >
        {t("shell.resume")}
      </button>
    );
  const close = () => {
    open.current = false;
    setAsk(false);
    setFailed(null);
  };
  const stop = async () => {
    setSending(true);
    setFailed(null);
    try {
      await post("/stop", {}, STOP_TIMEOUT_MS);
    } catch (e) {
      // Never silent: the dialog stays open to try again, or, closed while sending, a
      // message says so. It may have arrived late: a new state with the Stop closes the dialog.
      const said = e instanceof ApiError ? t("shell.stopRefused", { reason: e.message }) : t("shell.stopFailed");
      const text = /[.!?…]$/.test(said) ? said : `${said}.`;  // the emergency hint follows as its own sentence
      if (open.current) setFailed(text);
      else toast(`${text} ${t("shell.stopHint")}`, "error");
      void refreshState();
      return;
    } finally {
      setSending(false);
    }
    close();
    toast(t("shell.stopped"), "info");
    await refreshState();
  };
  return (
    <>
      <button
        class="stop-btn"
        data-testid="stop"
        onClick={() => {
          setFailed(null);  // a failure from a Stop cancelled while sending is not this one's
          open.current = true;
          setAsk(true);
        }}
        title={t("shell.stopTitle")}
      >
        <OctagonX size={18} /> <span class="hide-sm">{t("common.stop")}</span>
      </button>
      {ask && (
        <Modal
          title={t("shell.stopAsk")}
          onClose={close}
          footer={
            <>
              <button class="btn" onClick={close}>
                {t("common.cancel")}
              </button>
              <button ref={retry} class="btn danger" disabled={sending} onClick={stop} data-testid="stop-confirm">
                {sending ? t("shell.stopSending") : failed ? t("shell.stopRetry") : t("shell.stopAll")}
              </button>
            </>
          }
        >
          <p>{t("shell.stopText")}</p>
          <p class="muted small">{t("shell.stopNote")}</p>
          <div role="alert" data-testid="stop-failed">
            {failed && (
              <Banner tone="bad">
                {failed} {t("shell.stopHint")}
              </Banner>
            )}
          </div>
        </Modal>
      )}
    </>
  );
}

function LiveBadge() {
  // By the age of the values, as the phone's bar: polls standing in for the stream are
  // current, an open stream that brings nothing is not
  const l = stale.value ? "offline" : lastUpdate.value > 0 ? "live" : "connecting";
  const wd = state.value?.watchdog;
  return (
    <div class="row hide-sm">
      {simulated.value && <span class="sim-tag">{t("common.simulator")}</span>}
      {l === "live" ? (
        <Pill tone="ok" dot pulse title={t("shell.liveTitle")}>
          {t("common.live")}
        </Pill>
      ) : l === "connecting" ? (
        <Pill tone="neutral" dot>
          {t("common.connecting")}
        </Pill>
      ) : (
        <Pill tone="bad" dot title={t("shell.offlineTitle")}>
          {t("common.offline")}
        </Pill>
      )}
      {wd && (
        <a href="#/" style="text-decoration:none" data-testid="watchdog-headline">
          <Pill tone={wd.stale ? "bad" : wd.overall === "problem" ? "bad" : wd.overall === "ok" ? "ok" : "neutral"}>
            {wd.stale ? t("shell.watchdogStale") : msg(wd.headline)}
          </Pill>
        </a>
      )}
    </div>
  );
}

export function App() {
  const [more, setMore] = useState(false);
  if (authed.value === null)
    return (
      <div class="login">
        <p class="muted">{t("shell.connecting")}</p>
      </div>
    );
  if (!authed.value) return <Login />;
  if (!state.value || !config.value)
    return (
      <div class="login">
        <p class="muted">{t("shell.loading")}</p>
      </div>
    );
  const r = route.value;
  const page = NAV.find((n) => n.path === r.path) ?? NAV[0];  // an unknown path shows the Status, so the menus mark it
  const name = config.value.system.name;
  return (
    <div class="shell">
      <aside class="side">
        <div class="brand">
          <div class="brand-mark">
            <Droplets size={18} />
          </div>
          <div>
            Doseph
            <small>{name && name !== "Doseph" ? name : t("app.tagline")}</small>
          </div>
        </div>
        <nav class="nav" aria-label={t("nav.label")}>
          <NavLinks current={page.path} />
        </nav>
        <div class="side-foot">
          <span>
            <Gauge size={13} /> {t("common.version", { v: info.value?.version })}
          </span>
        </div>
      </aside>
      <div class="main">
        <div class="top">
          <header class="topbar">
            <h1>{t(page.label)}</h1>
            <div class="spacer" />
            <LiveBadge />
            <StopButton />
          </header>
          {state.value.stopped && (
            <div class="stopped-bar" role="status" data-testid="stopped-bar">
              <OctagonX size={18} /> {t("shell.stoppedBar")}
            </div>
          )}
          {/* Phones hide the live badge; the live region is always there, so a reader announces the text */}
          <div class="only-sm" role="status">
            {stale.value && (
              <div class="offline-bar" data-testid="offline-bar">
                {t("shell.offlineBar")}
              </div>
            )}
          </div>
        </div>
        <main class="content">
          <div class="stack">
            <TestingNotice />
            <ErrorBoundary key={page.path}>{page.el()}</ErrorBoundary>
          </div>
        </main>
      </div>
      <nav class="bottom-nav" aria-label={t("nav.label")}>
        {MOBILE.map((path) => NAV.find((n) => n.path === path)!).map((n) => (
          <a href={`#${n.path}`} class={page.path === n.path ? "active" : ""} aria-current={page.path === n.path ? "page" : undefined}>
            {n.icon}
            {t(n.label).split(" ")[0]}
          </a>
        ))}
        {/* "More" is marked while one of its pages is open */}
        <button class={MOBILE.includes(page.path) ? "" : "active"} aria-haspopup="dialog" onClick={() => setMore(true)}>
          <MoreHorizontal size={19} />
          {t("common.more")}
        </button>
      </nav>
      {more && (
        <Modal title={t("common.more")} onClose={() => setMore(false)}>
          <nav class="nav stack-sm">
            <NavLinks current={page.path} skip={MOBILE} onClick={() => setMore(false)} />
          </nav>
        </Modal>
      )}
      {simulated.value && <SimPanel />}
      <Toasts />
    </div>
  );
}
