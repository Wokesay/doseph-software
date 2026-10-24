// SPDX-License-Identifier: AGPL-3.0-or-later
// Colour scheme: the system's, light or dark. Kept in this browser only, so
// each device can have its own.
export type Theme = "system" | "light" | "dark";

const KEY = "dp-theme";

export function currentTheme(): Theme {
  const v = document.documentElement.dataset.theme;
  return v === "light" || v === "dark" ? v : "system";
}

export function setTheme(v: Theme) {
  if (v === "system") delete document.documentElement.dataset.theme;
  else document.documentElement.dataset.theme = v;
  try {
    if (v === "system") localStorage.removeItem(KEY);
    else localStorage.setItem(KEY, v);
  } catch {
    /* no storage: holds until the page is reloaded */
  }
}

/** At start: the scheme saved in this browser, otherwise the system's. */
export function loadTheme() {
  try {
    const v = localStorage.getItem(KEY);
    if (v === "light" || v === "dark") document.documentElement.dataset.theme = v;
  } catch {
    /* no storage: follow the system setting */
  }
}
