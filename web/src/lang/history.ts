// SPDX-License-Identifier: AGPL-3.0-or-later
// Texts of the history page.
export const de = {
  "history.intro": "Messwerte mit Zielband; gestrichelte Linien zeigen Gaben.",
  "history.range6h": "6\u00a0h",
  "history.range24h": "24\u00a0h",
  "history.range7d": "7\u00a0Tage",
  "history.range30d": "30\u00a0Tage",
  "history.onlyWithFlow": "pH, EC und Temperatur werden nur aufgezeichnet, solange Wasser durch das Gerät fließt – ohne Durchfluss steht die Messkammer und misst nicht den Tank.",
  "history.csvFormat": "Die Kurven dieses Zeitraums als CSV in UTC, für eine Tabellenkalkulation auf Deutsch: Semikolon und Dezimalkomma. Eine Lücke bleibt ein leeres Feld.",
  "history.events": "Ereignisse",
  "history.filter.dose": "Gaben",
  "history.filter.control": "Ablauf",
  "history.filter.alarm": "Alarme",
  "history.filter.calibration": "Einmessen",
  "history.filter.config": "Einstellungen",
  "history.filter.system": "System",
  "history.filter.auth": "Anmeldung",
} as const;

export const en: Record<keyof typeof de, string> = {
  "history.intro": "Readings with the target band; dashed lines mark doses.",
  "history.range6h": "6\u00a0h",
  "history.range24h": "24\u00a0h",
  "history.range7d": "7\u00a0days",
  "history.range30d": "30\u00a0days",
  "history.onlyWithFlow": "pH, EC and temperature are recorded only while water flows through the device – without flow the measuring chamber stands still and does not measure the tank.",
  "history.csvFormat": "The curves of this period as CSV in UTC, for a spreadsheet set to English: commas and a decimal point. A gap stays an empty field.",
  "history.events": "Events",
  "history.filter.dose": "Doses",
  "history.filter.control": "Cycle",
  "history.filter.alarm": "Alarms",
  "history.filter.calibration": "Calibration",
  "history.filter.config": "Settings",
  "history.filter.system": "System",
  "history.filter.auth": "Sign-in",
};
