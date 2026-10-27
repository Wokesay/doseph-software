// SPDX-License-Identifier: AGPL-3.0-or-later
import { de as login } from "./login";
import { de as status } from "./status";
import { de as recipe } from "./recipe";
import { de as tank } from "./tank";
import { de as history } from "./history";
import { de as pumps } from "./pumps";
import { de as settings } from "./settings";
import { de as sim } from "./sim";

// German texts of the app. Keys are stable; the English table (en.ts)
// must have every key. Placeholders in {braces}.
export const deBase = {
  // General
  "app.tagline": "Mischroboter für Nährlösung",
  "common.back": "Zurück",
  "common.name": "Name",
  "common.save": "Speichern",
  "common.cancel": "Abbrechen",
  "common.close": "Schließen",
  "common.gotIt": "Verstanden",
  "common.edit": "Bearbeiten",
  "common.delete": "Löschen",
  "common.add": "Hinzufügen",
  "common.more": "Mehr",
  "common.all": "Alle",
  "common.on": "an",
  "common.off": "aus",
  "common.none": "keine",
  "common.optional": "optional",
  "common.help": "Erklärung",
  "common.stop": "STOPP",
  "common.live": "live",
  "common.offline": "getrennt",
  "common.connecting": "verbinde …",
  "common.simulator": "Simulator",
  "common.renderError": "Dieser Teil lässt sich nicht anzeigen. Der Roboter ist davon nicht betroffen, STOPP oben funktioniert.",
  "common.reload": "Seite neu laden",
  "common.version": "Version {v}",
  "common.testingOnly": "Nur zum Testen: Diese Version ist ein Entwicklungsstand und nicht für den Dauerbetrieb freigegeben.",
  "common.justNow": "gerade eben",
  // Number and unit joined by a no-break space
  "common.secondsAgo": "vor {n} s",
  "common.minutesAgo": "vor {n} min",
  "common.hoursAgo": "vor {n} h",
  "common.daysAgo": "vor {n} Tagen",
  "common.language": "Sprache",
  "common.minutes": "{n} min",
  "common.suggested": "Vorschlag: {v}",

  // Navigation
  "nav.status": "Status",
  "nav.recipe": "Rezept",
  "nav.tank": "Tank",
  "nav.history": "Verlauf",
  "nav.pumps": "Pumpen & Sonden",
  "nav.settings": "Einstellungen",
  "nav.label": "Menü",

  // Terms with an explanation
  "term.volume": "Tankvolumen",
  "help.volume": "So viele Liter Nährlösung sind im Tank. Der Roboter rechnet jede Gabe darauf um; die erste Gabe eines neuen Tanks ist eine kleine Testgabe, die den Wert prüft.",
  "term.turnover": "Umlaufzeit",
  "help.turnover": "So lange braucht die Umwälzpumpe, um den Tank einmal durch das Gerät zu pumpen: Tankvolumen geteilt durch Durchfluss.",
  "term.waitTime": "Mischzeit",
  "help.waitTime": "So lange wird nach einer Gabe umgewälzt, bevor der Roboter wieder misst. Die Uhr läuft nur, solange Wasser fließt. Der Vorschlag rechnet mit 2 Umläufen für EC und 3 für pH; du legst die Zeit fest.",
  "term.holdTime": "Pause zwischen Prüfungen",
  "help.holdTime": "Ist der Tank im Ziel, ruht der Roboter so lange und prüft dann erneut. Er mischt nicht ständig.",
  "term.circulation": "Umwälzpumpe",
  "help.circulation": "„Nur zum Prüfen“: Der Roboter schaltet die Pumpe für jede Prüfung ein und danach aus. „Immer an“: Sie läuft durch. „Eigene Steuerung“: Du schaltest sie selbst, der Roboter wartet auf Durchfluss.",
  "term.testDose": "Testgabe",
  "help.testDose": "Die erste Gabe eines neuen Tanks ist klein. Wirkt sie viel stärker oder schwächer als für das eingetragene Volumen erwartet, hält der Roboter an und fragt nach dem Volumen.",
  "term.tolerance": "Toleranz",
  "help.tolerance": "Spielraum um das Ziel, in dem der Roboter nichts tut. Zu eng heißt ständiges Nachregeln.",
  "term.canister": "Kanister",
  "help.canister": "Ein Kanister mit Nährstoff oder pH−, an ihm hängt eine Dosierpumpe. Was darin ist, sieht der Roboter nicht – das sagst du ihm.",
  "term.pair": "Paar (A/B)",
  "help.pair": "Zwei Teile, die nur zusammen wirken. Der Roboter dosiert sie immer im festen Verhältnis, auch bei kleineren Gaben.",
  "term.fixed": "Feste Menge",
  "help.fixed": "Diese Menge gehört je Liter frisches Wasser hinein und wird beim Anheben der EC nicht mitdosiert, z. B. CalMag für weiches Wasser.",
  "term.calibratePump": "Einmessen",
  "help.calibratePump": "Die Pumpe läuft eine feste Zeit in einen Messbecher, du trägst die ml ein. Ohne diesen Wert dosiert sie nicht.",
  "term.prime": "Schlauch füllen",
  "help.prime": "Pumpt, bis die Flüssigkeit vorne ankommt. Nötig vor dem Einmessen und nach jedem Kanisterwechsel. Halte das Schlauchende in einen Becher, nicht in den Tank.",
  "term.calibrateProbe": "Sonde kalibrieren",
  "help.calibrateProbe": "pH: Sonde nacheinander in zwei Pufferlösungen tauchen (z. B. pH 7 und 4). EC: in eine Referenzlösung (z. B. 1,413 mS/cm). Ohne Kalibrierung zeigt der Roboter Werte nur an und dosiert nicht.",
  "term.latched": "Gesperrt bis Freigabe",
  "help.latched": "Eine Gabe hatte keine Wirkung oder die Testgabe passte nicht zum Tankvolumen. Der Roboter dosiert diese Art nicht mehr, bis du die Ursache geprüft und freigegeben hast.",
  "term.jumpLock": "Sprungsperre",
  "help.jumpLock": "Ein Messwert ist unerwartet gesprungen. Der Roboter nutzt ihn erst wieder, wenn er 15 min ruhig ist – freigeben musst du nichts.",

  // The frame of the app
  "shell.resume": "Fortsetzen",
  "shell.resumed": "Fortgesetzt",
  "shell.stopTitle": "Stopp: alle Pumpen aus",
  "shell.stopAsk": "Alles stoppen?",
  "shell.stopAll": "Alles stoppen",
  "shell.stopped": "Gestoppt: alles aus",
  "shell.stoppedBar": "Gestoppt – Dosierpumpen und Umwälzpumpe sind aus und bleiben aus, bis du fortsetzt.",
  "shell.stopText": "Dosierpumpen und Umwälzpumpe gehen sofort aus. Eine laufende Gabe wird abgebrochen, der Roboter ruht, bis du „Fortsetzen“ wählst.",
  "shell.stopNote": "Messen läuft weiter.",
  "shell.stopSending": "Sende …",
  "shell.stopRetry": "Erneut senden",
  "shell.stopRefused": "Der Roboter hat den Stopp abgelehnt: {reason}",
  "shell.stopFailed": "Der Stopp kam nicht an",
  "shell.stopHint": "Im Notfall zieh den Stecker des Roboters – dann ist alles aus.",
  "shell.liveTitle": "Live-Verbindung zum Roboter",
  "shell.offlineTitle": "Keine Verbindung zum Roboter – er arbeitet weiter",
  "shell.offlineBar": "Keine Verbindung zum Roboter – die Werte sind nicht aktuell. Er arbeitet weiter.",
  "shell.watchdogStale": "Überwachung ohne Bewertung",
  "shell.connecting": "Verbinde mit dem Roboter …",
  "shell.loading": "Lade …",
  "shell.noAnswer": "Der Roboter antwortet nicht. Prüf, ob er eingeschaltet und im selben Netz ist.",
  "shell.failed": "Das hat nicht geklappt.",
  "shell.httpError": "Fehler {status}",
} as const;

// The base texts plus the texts of each area.
export const de = { ...deBase, ...login, ...status, ...recipe, ...tank, ...history, ...pumps, ...settings, ...sim };
