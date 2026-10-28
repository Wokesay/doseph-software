// SPDX-License-Identifier: AGPL-3.0-or-later
// Commissioning (SD-129): the steps of #10 in their order, kept by the robot so they resume after a
// restart. A step the robot sees is ticked off while it sees it; the user confirms the others, and
// skips an optional one. Each step links to the page where it is done.
import { useEffect, useState } from "preact/hooks";
import { CheckCircle2, Circle, Clock, ListChecks, SkipForward } from "lucide-preact";
import { get, put, type Setup, type SetupStep, type SetupStepId } from "../api";
import { t, type TextKey } from "../i18n";
import { info, refreshState, state, toastError } from "../store";
import { Banner, Button, Card, Pill } from "../ui";

// Where each step is done in the app
const PAGE: Partial<Record<SetupStepId, string>> = {
  password: "/settings",
  flow: "/water",
  probes: "/water",
  channels: "/channels",
  lines: "/channels",
  test: "/water",
  tank: "/tank",
  push: "/settings",
};

export const stepTitle = (id: SetupStepId) => t(`setup.${id}` as TextKey);

function stepText(id: SetupStepId): string {
  if (id === "placement") return t(info.value?.profile?.pumpIn === "device" ? "setup.placement.device" : "setup.placement.tank");
  return t(`setup.${id}.text` as TextKey);
}

function StateIcon(p: { step: SetupStep }) {
  if (p.step.state === "done") return <CheckCircle2 size={20} class="ok-text" aria-hidden="true" />;
  if (p.step.state === "skipped") return <SkipForward size={20} class="muted" aria-hidden="true" />;
  if (p.step.state === "later") return <Clock size={20} class="muted" aria-hidden="true" />;
  return <Circle size={20} class="muted" aria-hidden="true" />;
}

export function SetupPage() {
  const [setup, setSetup] = useState<Setup | null>(null);
  const load = async () => setSetup(await get<Setup>("/commissioning"));
  // The robot's own steps change with what it sees: read again whenever the state says so
  const seen = state.value?.setup;
  useEffect(() => {
    load().catch(toastError);
  }, [seen?.next, seen?.complete]);
  if (!setup) return null;
  const mark = async (id: SetupStepId, to: "done" | "skipped" | "open") => {
    setSetup(await put<Setup>(`/commissioning/${id}`, { state: to }));
    await refreshState();
  };
  const counted = setup.steps.filter((s) => s.state !== "later");
  const done = counted.filter((s) => s.state === "done" || s.state === "skipped").length;
  return (
    <Card title={t("nav.setup")} icon={<ListChecks size={18} />}>
      <div class="stack">
        <p class="muted">{t("setup.intro")}</p>
        {setup.complete ? (
          <Banner tone="ok">{t("setup.complete")}</Banner>
        ) : (
          <p data-testid="setup-progress">{t("setup.progress", { done, all: counted.length })}</p>
        )}
        <div class="list" data-testid="setup-steps">
          {setup.steps.map((s) => (
            <div class="item wrap-row" key={s.id} data-testid={`setup-${s.id}`} data-state={s.state}>
              <StateIcon step={s} />
              <div class="grow">
                <div class="title">
                  {stepTitle(s.id)} {setup.next === s.id ? <Pill tone="info">{t("setup.next")}</Pill> : <Pill>{t(`setup.state.${s.state}`)}</Pill>}
                </div>
                <div class="muted small">{stepText(s.id)}</div>
                {s.how === "device" && s.state === "open" && <div class="faint small">{t("setup.seen")}</div>}
              </div>
              <div class="row-actions">
                {s.state === "open" && PAGE[s.id] && (
                  <a class="btn sm" href={`#${PAGE[s.id]}`}>
                    {t("setup.go")}
                  </a>
                )}
                {s.how === "confirm" && s.state === "open" && (
                  <Button size="sm" variant="primary" onClick={() => mark(s.id, "done")} data-testid={`confirm-${s.id}`}>
                    {t("setup.confirm")}
                  </Button>
                )}
                {s.optional && s.state === "open" && (
                  <Button size="sm" onClick={() => mark(s.id, "skipped")} data-testid={`skip-${s.id}`}>
                    {t("setup.skip")}
                  </Button>
                )}
                {s.at > 0 && (
                  <Button size="sm" variant="ghost" onClick={() => mark(s.id, "open")} data-testid={`reopen-${s.id}`}>
                    {t("setup.reopen")}
                  </Button>
                )}
              </div>
            </div>
          ))}
        </div>
      </div>
    </Card>
  );
}

// Above every other page while commissioning is not complete: the next step, one tap away
export function SetupBanner(p: { path: string }) {
  const s = state.value?.setup;
  if (!s || s.complete || !s.next || p.path === "/setup") return null;
  return (
    <Banner tone="info">
      <span data-testid="setup-banner">
        {t("setup.banner", { step: stepTitle(s.next) })}{" "}
        <a href="#/setup">{t("setup.continue")}</a>
      </span>
    </Banner>
  );
}
