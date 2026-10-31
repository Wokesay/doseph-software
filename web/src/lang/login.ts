// SPDX-License-Identifier: AGPL-3.0-or-later
// Texts of one area of the app, German first. de.ts and en.ts merge
// all areas; en must have every key of de (checked by the compiler).
export const de = {
  "login.mismatch": "Die Passwörter stimmen nicht überein.",
  "login.welcome": "Willkommen",
  "login.firstText": "Lege zuerst ein eigenes Passwort fest. Es schützt Kanäle, Pumpen und Einstellungen in deinem Netz.",
  "login.newPassword": "Neues Passwort",
  "login.minLength": "Mindestens 8 Zeichen",
  "login.repeatPassword": "Passwort wiederholen",
  "login.code": "Einrichtungscode",
  "login.codeHint": "Der Roboter zeigt ihn beim Start an (im Simulator in der Konsole). Jeder Neustart erzeugt einen neuen.",
  "login.labelHint": "Er steht auf dem Etikett des Geräts. Drück zuerst die Funktionstaste am Gerät: 5\u00a0min lang nimmt es den Code. Heb das Etikett gut auf – nach einem Zurücksetzen brauchst du ihn wieder.",
  "login.password": "Passwort",
  "login.setPassword": "Passwort festlegen",
  "login.signIn": "Anmelden",
  "login.expired": "Deine Anmeldung ist abgelaufen. Hast du gerade etwas angeklickt, hat der Roboter es nicht ausgeführt – melde dich an und wiederhole es.",
  "login.localNote": "Die Web-App läuft auf dem Roboter in deinem Heimnetz – ohne Cloud und ohne Konto. Der Roboter arbeitet weiter, auch wenn diese Seite zu ist.",
} as const;

export const en: Record<keyof typeof de, string> = {
  "login.mismatch": "The passwords do not match.",
  "login.welcome": "Welcome",
  "login.firstText": "First set your own password. It protects channels, pumps and settings on your network.",
  "login.newPassword": "New password",
  "login.minLength": "At least 8 characters",
  "login.repeatPassword": "Repeat password",
  "login.code": "Setup code",
  "login.codeHint": "The robot shows it when it starts (in the simulator on the console). Every restart makes a new one.",
  "login.labelHint": "It is on the device's label. Press the function key on the device first: for 5\u00a0min it takes the code. Keep the label safe – you need the code again after a reset.",
  "login.password": "Password",
  "login.setPassword": "Set password",
  "login.signIn": "Sign in",
  "login.expired": "Your session has expired. If you just clicked something, the robot did not carry it out – sign in and do it again.",
  "login.localNote": "The web app runs on the robot on your home network – no cloud, no account. The robot keeps working even when this page is closed.",
};
