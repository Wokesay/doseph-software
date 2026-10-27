Doseph – simulator
==================

English
-------
The simulator is the Doseph software with a simulated device, tank, pumps
and probes. Nothing is installed; no hardware and no internet are needed.
It is a prototype: for trying the software, not for dosing into a real
tank.

Start:
  Windows:  double-click doseph-simulator.exe.
            If Windows says "Windows protected your PC":
            "More info" → "Run anyway".
  macOS:    in Terminal, change to the unpacked folder (type cd and a
            space, then drag the folder into Terminal) and run once:
              xattr -d com.apple.quarantine doseph-simulator
            then double-click it, or run ./doseph-simulator
  Linux:    ./doseph-simulator

Your browser opens http://127.0.0.1:8080 (8081 … if the port is busy).
The window shows the address and the demo password; sign in with it.

The demo runs in memory only: closing it starts it over. To keep the data,
start it with --data and a folder, for example
  doseph-simulator --data doseph-data
Stop: close the window or press Ctrl+C.

More options: doseph-simulator --help
  for example --scenario new   (a new device: set the first password)
              --speed 60       (time-lapse)

Feedback and bug reports:
  https://github.com/Wokesay/doseph-software/issues/new/choose

License: free software under the GNU Affero General Public License,
version 3 or later (LICENSE.txt). Source code:
  https://github.com/Wokesay/doseph-software
Third-party licenses of the included libraries: THIRD_PARTY_LICENSES.txt

Deutsch
-------
Der Simulator ist die Doseph-Software mit nachgebildetem Gerät, Tank,
Pumpen und Sonden. Nichts wird installiert, es braucht keine Hardware und
kein Internet. Es ist ein Prototyp: zum Ausprobieren, nicht zum Dosieren
in einen echten Tank.

Starten:
  Windows:  doseph-simulator.exe doppelklicken. Meldet Windows
            „Der Computer wurde durch Windows geschützt“:
            „Weitere Informationen“ → „Trotzdem ausführen“.
  macOS:    im Terminal in den entpackten Ordner wechseln und einmal
              xattr -d com.apple.quarantine doseph-simulator
            ausführen, dann doppelklicken oder ./doseph-simulator starten.
  Linux:    ./doseph-simulator

Der Browser öffnet http://127.0.0.1:8080 (8081 …, wenn der Port belegt
ist). Das Fenster zeigt die Adresse und das Demo-Passwort; damit anmelden.
Sprache: in der App unter Settings › Language and look auf Deutsch umstellen.

Die Demo läuft nur im Speicher: Schließen startet sie von vorn. Mit
--data und einem Ordner bleiben die Daten erhalten, zum Beispiel
  doseph-simulator --data doseph-daten
Beenden: Fenster schließen oder Strg+C.

Rückmeldungen und Fehler:
  https://github.com/Wokesay/doseph-software/issues/new/choose

Lizenz: freie Software unter der GNU Affero General Public License,
Version 3 oder später (LICENSE.txt). Quellcode:
  https://github.com/Wokesay/doseph-software
