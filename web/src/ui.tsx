// SPDX-License-Identifier: AGPL-3.0-or-later
// Building blocks of the page: cards, buttons, fields, dialogs, status.
import { Component, render, type ComponentChildren, type ButtonHTMLAttributes } from "preact";
import { useEffect, useLayoutEffect, useRef, useState } from "preact/hooks";
import { signal } from "@preact/signals";
import { AlertTriangle, Check, CheckCircle2, HelpCircle, Info, Minus, X, XCircle } from "lucide-preact";
import { lang, t, type TextKey } from "./i18n";
import { testingOnly, toastError, toasts } from "./store";

// ---------- Router (by hash, so the app runs from the device's flash without routing on the server)
function parseHash() {
  const h = location.hash.replace(/^#/, "") || "/";
  const [path, q = ""] = h.split("?");
  return { path, query: Object.fromEntries(new URLSearchParams(q)) as Record<string, string> };
}
export const route = signal(parseHash());
window.addEventListener("hashchange", () => (route.value = parseHash()));
export function navigate(to: string) {
  location.hash = to.startsWith("#") ? to : `#${to}`;
}

// ---------- Building blocks
export function Card(p: { title?: ComponentChildren; icon?: ComponentChildren; actions?: ComponentChildren; children?: ComponentChildren; class?: string; id?: string }) {
  return (
    <section class={`card ${p.class ?? ""}`} id={p.id}>
      {(p.title || p.actions) && (
        <div class="card-h">
          <h2>
            {p.icon}
            {p.title}
          </h2>
          {p.actions && <div class="row">{p.actions}</div>}
        </div>
      )}
      {p.children}
    </section>
  );
}

type BtnProps = Omit<ButtonHTMLAttributes<HTMLButtonElement>, "onClick"> & {
  variant?: "primary" | "danger" | "danger-soft" | "ghost" | "default";
  size?: "sm" | "lg";
  block?: boolean;
  onClick?: () => unknown | Promise<unknown>;
  disabled?: boolean;
  type?: "button" | "submit";
};
export function Button({ variant = "default", size, block, onClick, children, disabled, type = "button", ...rest }: BtnProps) {
  const [busy, setBusy] = useState(false);
  const cls = ["btn", variant !== "default" ? variant : "", size ?? "", block ? "block" : ""].join(" ");
  return (
    <button
      {...rest}
      type={type}
      class={cls}
      disabled={disabled || busy}
      onClick={async () => {
        if (!onClick) return;
        try {
          setBusy(true);
          await onClick();
        } catch (e) {
          toastError(e);
        } finally {
          setBusy(false);
        }
      }}
    >
      {children}
    </button>
  );
}

export type Tone = "ok" | "bad" | "warn" | "info" | "neutral" | "accent";
export function Pill(p: { tone?: Tone; dot?: boolean; pulse?: boolean; wrap?: boolean; children: ComponentChildren; title?: string }) {
  return (
    <span class={`pill ${p.tone ?? ""}${p.wrap ? " wrap" : ""}`} title={p.title}>
      {p.dot && <span class={`dot ${p.pulse ? "pulse" : ""}`} />}
      {p.children}
    </span>
  );
}

// An explanation of a term: ⓘ opens its text. A <span role="button"> rather
// than a <button>, so the <label> around it still means the input field.
export type HelpTopic = "volume" | "turnover" | "waitTime" | "holdTime" | "circulation" | "testDose" | "tolerance" | "canister" | "pair" | "fixed" | "calibratePump" | "prime" | "calibrateProbe" | "latched" | "jumpLock";

export function HelpButton(p: { open: boolean; onToggle: () => void; topic: HelpTopic }) {
  const toggle = (e: Event) => {
    e.preventDefault();
    e.stopPropagation();
    p.onToggle();
  };
  return (
    <span
      class="help-btn"
      role="button"
      tabIndex={0}
      aria-expanded={p.open}
      aria-label={`${t("common.help")}: ${t(`term.${p.topic}` as TextKey)}`}
      onClick={toggle}
      onKeyDown={(e) => (e.key === "Enter" || e.key === " ") && toggle(e)}
    >
      <HelpCircle size={15} />
    </span>
  );
}

export function HelpBox(p: { topic: HelpTopic }) {
  return (
    <div class="help-box" role="note">
      <span>{t(`help.${p.topic}` as TextKey)}</span>
    </div>
  );
}

/** A term with ⓘ and its explanation to open, in headings and lists. */
export function Term(p: { topic: HelpTopic; children?: ComponentChildren }) {
  const [open, setOpen] = useState(false);
  return (
    <span class="term">
      <span class="field-label">
        {p.children ?? t(`term.${p.topic}` as TextKey)}
        <HelpButton topic={p.topic} open={open} onToggle={() => setOpen(!open)} />
      </span>
      {open && <HelpBox topic={p.topic} />}
    </span>
  );
}

let fieldIds = 0;
// A field around a choice of buttons (Seg, colours) is a group, not a <label>: a label
// would give its own text to the first button as that button's name.
export function Field(p: { label: ComponentChildren; hint?: ComponentChildren; error?: string | null; help?: HelpTopic; group?: boolean; children: ComponentChildren }) {
  const [open, setOpen] = useState(false);
  const [id] = useState(() => `field-${++fieldIds}`);
  const label = (
    <span class="field-label" id={p.group ? id : undefined}>
      {p.label}
      {p.help && <HelpButton topic={p.help} open={open} onToggle={() => setOpen(!open)} />}
    </span>
  );
  const foot = p.error ? <small class="err">{p.error}</small> : p.hint ? <small class="hint">{p.hint}</small> : null;
  const field = p.group ? (
    <div class="field" role="group" aria-labelledby={id}>
      {label}
      {p.children}
      {foot}
    </div>
  ) : (
    <label class="field">
      {label}
      {p.children}
      {foot}
    </label>
  );
  if (!p.help) return field;
  return (
    <div class="field-wrap">
      {field}
      {open && <HelpBox topic={p.help} />}
    </div>
  );
}

// Decimal comma in German, decimal point in English (gc PD-035); typing accepts both.
const shown = (v: number) => (lang.value === "de" ? String(v).replace(".", ",") : String(v));

export function NumberInput(p: { value: number | null | undefined; onValue: (v: number | null) => void; unit?: string; min?: number; max?: number; step?: number; placeholder?: string; id?: string; name?: string; ariaLabel?: string }) {
  const [text, setText] = useState(p.value === null || p.value === undefined ? "" : shown(p.value));
  useEffect(() => {
    const cur = parseFloat(text.replace(",", "."));
    if (p.value !== null && p.value !== undefined && cur !== p.value) setText(shown(p.value));
    if ((p.value === null || p.value === undefined) && text !== "" && Number.isNaN(cur)) setText("");
  }, [p.value]);
  // A language switch shows the value again with the page's separator.
  useEffect(() => {
    if (p.value !== null && p.value !== undefined) setText(shown(p.value));
  }, [lang.value]);
  const input = (
    <input
      class="input"
      inputMode="decimal"
      id={p.id}
      name={p.name}
      aria-label={p.ariaLabel}
      value={text}
      placeholder={p.placeholder}
      onInput={(e) => {
        const t = (e.target as HTMLInputElement).value;
        setText(t);
        const v = parseFloat(t.replace(",", "."));
        p.onValue(t.trim() === "" || Number.isNaN(v) ? null : v);  // empty stays empty, never 0
      }}
    />
  );
  return p.unit ? (
    <div class="input-unit">
      {input}
      <span class="u">{p.unit}</span>
    </div>
  ) : (
    input
  );
}

// A change that fails says why, as a Button does; a switch or choice never fails without a word
const changed = (f: () => unknown) => void Promise.resolve().then(f).catch(toastError);

export function Toggle(p: { checked: boolean; onChange: (v: boolean) => unknown; disabled?: boolean; label?: string }) {
  return (
    <label class="toggle" title={p.label}>
      <input type="checkbox" checked={p.checked} disabled={p.disabled} aria-label={p.label} onChange={(e) => changed(() => p.onChange((e.target as HTMLInputElement).checked))} />
      <span />
    </label>
  );
}

/** The two languages in their own names, in the same order everywhere (SD-014); each name is read in its language. */
export const LANGS: ["en" | "de", ComponentChildren][] = [
  ["en", <span lang="en" class="notranslate">English</span>],
  ["de", <span lang="de" class="notranslate">Deutsch</span>],
];

export function Seg<T extends string>(p: { value: T; options: [T, ComponentChildren][]; onChange: (v: T) => unknown; label?: string; labelledBy?: string; large?: boolean }) {
  return (
    <div class={`seg${p.large ? " lg" : ""}`} role="tablist" aria-label={p.label} aria-labelledby={p.labelledBy}>
      {p.options.map(([v, l]) => (
        <button type="button" role="tab" aria-selected={p.value === v} class={p.value === v ? "on" : ""} onClick={() => changed(() => p.onChange(v))}>
          {l}
        </button>
      ))}
    </div>
  );
}

// Renders its children at the end of <body>: a window opened from the sticky
// top bar would otherwise be fenced in by it (its blur makes it the frame of
// fixed elements), cut off at the top and over an undimmed page. The app
// uses no context, so a second root loses nothing.
function Portal(p: { children: ComponentChildren }) {
  const host = useRef<HTMLDivElement | null>(null);
  if (!host.current) host.current = document.createElement("div");
  useLayoutEffect(() => {
    const el = host.current!;
    document.body.appendChild(el);
    return () => {
      render(null, el);
      el.remove();
    };
  }, []);
  useLayoutEffect(() => render(<>{p.children}</>, host.current!));
  return null;
}

// What Tab can reach; a <details> is reached by its <summary>
const FOCUSABLE = 'a[href], button:not(:disabled), input:not(:disabled):not([type="hidden"]), select:not(:disabled), textarea:not(:disabled), details > summary:first-of-type, [tabindex]:not([tabindex="-1"])';
let modalIds = 0;
// Open dialogs. The topmost is the last in the page: each one sits at the end of <body>
// in its own host (Portal), a dialog opened from another one after it.
const dialogs: HTMLElement[] = [];
const topmost = () => dialogs.reduce<HTMLElement | undefined>((a, b) => (!a || a.compareDocumentPosition(b) & Node.DOCUMENT_POSITION_FOLLOWING ? b : a), undefined);
// The element that last had the focus. A button that disables itself while its click
// runs (Button) loses the focus to <body> before a dialog it opens after an answer
// from the robot can see it.
let lastFocused: HTMLElement | null = null;
document.addEventListener("focusin", (e) => (lastFocused = e.target as HTMLElement), true);

// A dialog (gc#105): focus moves into it when it opens, Tab and Shift+Tab stay
// inside, focus returns to what opened it when it closes, and its title names it.
// With a message it is a confirmation (role alertdialog): a screen reader reads
// the message with the title. returnFocus names where the focus goes when what
// opened the dialog is gone (a removed row's button).
export function Modal(p: {
  title: ComponentChildren;
  onClose: () => void;
  children?: ComponentChildren;
  message?: ComponentChildren;
  footer?: ComponentChildren;
  wide?: boolean;
  returnFocus?: () => HTMLElement | null;
}) {
  // Escape calls the current onClose, not the one from the first render. The keys are
  // listened for as soon as the window is in the page, not only after the next frame.
  const onClose = useRef(p.onClose);
  onClose.current = p.onClose;
  const returnFocus = useRef(p.returnFocus);
  returnFocus.current = p.returnFocus;
  const box = useRef<HTMLDivElement>(null);
  const [titleId] = useState(() => `modal-title-${++modalIds}`);
  // What opened the dialog, taken before the dialog takes the focus: the focused
  // element, or a button that disabled itself. Safari and Firefox on a Mac do not focus
  // a clicked button, so <body> with an enabled last element means "unknown", not that one.
  const [opener] = useState(() => {
    const at = document.activeElement as HTMLElement | null;
    if (at && at !== document.body) return at;
    return lastFocused?.isConnected && lastFocused.matches(":disabled") ? lastFocused : null;
  });
  useLayoutEffect(() => {
    const el = box.current!;
    dialogs.push(el);
    const keys = (e: KeyboardEvent) => {
      if (topmost() !== el) return;
      if (e.key === "Escape") onClose.current();
      else if (e.key === "Tab") trap(el, e);
    };
    window.addEventListener("keydown", keys);
    return () => {
      window.removeEventListener("keydown", keys);
      dialogs.splice(dialogs.indexOf(el), 1);
      // Back to what opened the dialog once the page has settled (a route change may
      // remove it), else to the fallback; never out of a dialog that is open now.
      const fallback = returnFocus.current;
      queueMicrotask(() => {
        const to = opener?.isConnected ? opener : fallback?.();
        const top = topmost();
        if (to && (!top || top.contains(to))) to.focus({ preventScroll: true });
      });
    };
  }, []);
  useEffect(() => {
    const el = box.current;
    // The dialog itself takes the focus, so a screen reader names it by its title and a
    // phone opens no keyboard; Tab goes on to its first control. A part of the dialog
    // that took the focus itself keeps it (a retry button).
    if (el && !el.contains(document.activeElement)) el.focus();
  }, []);
  const message = p.message !== undefined;
  return (
    <Portal>
      <div class="overlay" onClick={(e) => e.target === e.currentTarget && p.onClose()}>
        <div
          class={`modal ${p.wide ? "wide" : ""}`}
          role={message ? "alertdialog" : "dialog"}
          aria-modal="true"
          aria-labelledby={titleId}
          aria-describedby={message ? `${titleId}-text` : undefined}
          tabIndex={-1}
          ref={box}
        >
          <div class="row-between">
            <h2 id={titleId}>{p.title}</h2>
            <button class="btn ghost sm" onClick={p.onClose} aria-label={t("common.close")}>
              <X size={18} />
            </button>
          </div>
          {message && <p id={`${titleId}-text`}>{p.message}</p>}
          {p.children}
          {p.footer && <div class="modal-foot">{p.footer}</div>}
        </div>
      </div>
    </Portal>
  );
}

// Keeps Tab inside the dialog: from the last control to the first and back, and into
// the dialog when the focus has left it (a control that disabled itself drops it to <body>).
// What Tab skips does not count: hidden, tabindex below 0, inside a closed <details>.
function trap(box: HTMLElement, e: KeyboardEvent) {
  const all = Array.from(box.querySelectorAll<HTMLElement>(FOCUSABLE)).filter(
    (f) => f.getClientRects().length > 0 && !f.matches('[tabindex^="-"]') && !f.closest("details:not([open]) > :not(summary)"),
  );
  const at = document.activeElement;
  const first = all[0], last = all[all.length - 1];
  if (!all.length) {
    e.preventDefault();
    box.focus();
  } else if (!box.contains(at)) {
    e.preventDefault();
    (e.shiftKey ? last : first).focus();
  } else if (e.shiftKey && (at === first || at === box)) {
    e.preventDefault();
    last.focus();
  } else if (!e.shiftKey && at === last) {
    e.preventDefault();
    first.focus();
  }
}

export function Toasts() {
  return (
    <div class="toasts" aria-live="polite">
      {toasts.value.map((t) => (
        <div class={`toast ${t.kind}`} key={t.id}>
          {t.kind === "error" ? <XCircle size={18} /> : t.kind === "ok" ? <CheckCircle2 size={18} /> : <Info size={18} />}
          <span>{t.text}</span>
        </div>
      ))}
    </div>
  );
}

export function Banner(p: { tone?: "info" | "warn" | "bad" | "ok"; children: ComponentChildren; icon?: ComponentChildren }) {
  const icon = p.icon ?? (p.tone === "bad" || p.tone === "warn" ? <AlertTriangle size={18} /> : p.tone === "ok" ? <CheckCircle2 size={18} /> : <Info size={18} />);
  return (
    <div class={`banner ${p.tone ?? ""}`}>
      {icon}
      <div>{p.children}</div>
    </div>
  );
}

/** A prototype or beta says it is for testing only (SD-096); a Stable release shows nothing. */
export function TestingNotice() {
  if (!testingOnly.value) return null;
  return (
    <Banner tone="info">
      <span data-testid="testing-notice">{t("common.testingOnly")}</span>
    </Banner>
  );
}

/** Catches an error while drawing its part, so the rest of the page and the shell with STOP stay usable. */
export class ErrorBoundary extends Component<{ children: ComponentChildren }, { failed: boolean }> {
  state = { failed: false };
  static getDerivedStateFromError() {
    return { failed: true };
  }
  componentDidCatch(e: unknown) {
    console.error(e);  // still diagnosable in the browser console
  }
  render() {
    if (!this.state.failed) return this.props.children;
    return (
      <Banner tone="bad">
        <div class="stack-sm">
          <p>{t("common.renderError")}</p>
          <div>
            <button class="btn" onClick={() => location.reload()}>
              {t("common.reload")}
            </button>
          </div>
        </div>
      </Banner>
    );
  }
}

export function Empty(p: { icon?: ComponentChildren; title: string; text?: ComponentChildren; action?: ComponentChildren }) {
  return (
    <div class="empty">
      {p.icon}
      <h3>{p.title}</h3>
      {p.text && <p class="muted">{p.text}</p>}
      {p.action}
    </div>
  );
}

export function CheckRow(p: { ok: boolean; soft?: boolean; text: string; neutral?: boolean }) {
  return (
    <div class={`check ${p.ok ? "ok" : p.soft ? "soft" : "no"}`}>
      {p.neutral ? <Minus size={16} /> : p.ok ? <Check size={16} /> : p.soft ? <AlertTriangle size={16} /> : <X size={16} />}
      <span>{p.text}</span>
    </div>
  );
}

export function useNow(intervalMs = 1000) {
  const [now, setNow] = useState(Date.now());
  useEffect(() => {
    const t = setInterval(() => setNow(Date.now()), intervalMs);
    return () => clearInterval(t);
  }, [intervalMs]);
  return now;
}
