# mpc-addin-surface

Ein `LD_PRELOAD`-Addin für die Akai Force (und MPC-OS-Geräte), das **mitschreibt, was die eingebaute
Bedienoberfläche an die MPC-Software sendet**: jede Taste, jedes Pad, jeder Regler als eine Zeile in einer Logdatei.

Das ist **Stufe 1** eines Steuer-Addins. Ziel von Stufe 2: Ansichten der Force (Matrix, Mixer, Track Edit) von einem
externen MIDI-Controller wie dem Faderfox EC4 umschalten, indem das Addin die passenden Tastendrücke einschleust.
Dafür müssen zuerst die Codes der Tasten sicher bekannt sein, und genau die liefert diese Stufe.

**Stand: ungetestet auf einem Gerät.** Die Host-Tests laufen (Parser, Konfiguration, Hooks gegen ein nachgebautes
libasound). Ob die Force ihre Tasten wirklich über die hier abgefangenen Funktionen liest, zeigt erst der Gerätetest;
das Log sagt es in jedem Fall (siehe „Auswerten").

Gerüst, Installer und Build stammen von [mpc-addin-usb-audio](https://github.com/jacob-sabella/mpc-addin-usb-audio)
und [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins).

## Was es tut und was nicht

- Es hängt sich an drei Funktionen von libasound: `snd_rawmidi_open`, `snd_rawmidi_read`, `snd_rawmidi_close`.
- Nach jedem Lesen kopiert es die empfangenen Bytes in eine kleine Warteschlange. Ein eigener Thread zerlegt sie in
  MIDI-Meldungen und schreibt das Log.
- **Es verändert nichts.** Die MPC-Software bekommt exakt die Bytes, die das Gerät gesendet hat. Es wird nichts
  eingeschleust, nichts zurückgehalten, nichts an die Oberfläche gesendet.
- Es wird nur im Prozess `/usr/bin/MPC` aktiv. In jedem anderen Prozess sind alle Hooks reine Durchreicher.
- Der Lese-Hook wartet nie: Ist die Warteschlange gerade belegt oder voll, wird der Block nur gezählt, nicht geloggt.

## Bauen (GitHub Actions)

1. Neues Repo anlegen (z. B. `mpc-addin-surface`) und den Inhalt dieses Ordners hochladen. Achtung: der Ordner
   `.github` muss mit hoch, sonst gibt es keine Workflows.
2. Unter **Actions** läuft bei jedem Push der Workflow **Test** (Host-Tests und ARM-Build).
3. **Actions > Release > Run workflow**, Version `0.1.0` eintragen. Ergebnis: ein Release-Entwurf mit
   `…-0.1.0-mpc-armv7.zip` (mit `dry_run` stattdessen als Artefakt am Workflow-Lauf).

Heißt das Repo anders oder liegt es unter einem anderen Konto, in `tools/release.sh` die Zeile `REPO=` anpassen
(nur für lokale Builds nötig).

## Installieren

Braucht root per SSH. Die ZIP auf die Force kopieren (z. B. auf die SD-Karte), dort entpacken und im Ordner:

```sh
sh install.sh
```

Der Installer legt alles nach `/data/mpc-addins/surface/` und **ergänzt** die Bibliothek in `LD_PRELOAD` des
MPC-Dienstes; andere Addins bleiben erhalten. Danach startet die MPC-Software neu.

Entfernen:

```sh
sh /data/mpc-addins/surface/uninstall.sh
```

Schnell abschalten ohne Deinstallation: in `/data/mpc-addins/surface/surface.conf` die Zeile `enabled=0` setzen und
die Force neu starten.

## Der Test (Stufe 1)

1. Nach der Installation warten, bis die Force wieder normal läuft.
2. Nacheinander drücken, jeweils mit ein paar Sekunden Pause, und die Reihenfolge merken:
   **Matrix**, **Mixer**, **Edit** allein, dann **Edit halten + eine Track-Select-Taste**, **Menu**.
3. Log ansehen:

```sh
cat /data/mpc-addins/surface/surface.log
```

## Auswerten

So sieht ein Tastendruck aus (drücken und loslassen):

```
13:58:29.405 in hw:0,0,0   90 03 7F  note-on  ch1 note 3 vel 127
13:58:29.512 in hw:0,0,0   90 03 00  note-off(on, vel 0)  ch1 note 3 vel 0
```

Die drei Hex-Bytes sind der Code, den Stufe 2 später einschleust.

- Steht im Log `input opened: … (logging)` und danach Zeilen mit `in …`: alles wie erhofft.
- Steht dort `active: …`, aber **kein** `input opened`: Die Force liest ihre Oberfläche nicht über libasound. Dann
  muss Stufe 2 an einer anderen Stelle ansetzen; das Log trotzdem aufheben.
- Gibt es gar keine Logdatei: Das Addin wurde nicht geladen (Installation prüfen).

## Einstellungen

`surface.conf` im Addin-Ordner, gelesen beim Start der MPC-Software. Alle Schlüssel mit Erklärung stehen in
`etc/surface.conf.example`. Die wichtigsten:

- `enabled=0`: Addin aus.
- `log_pressure=1`: auch Pad-Druck (Aftertouch) loggen; standardmäßig aus, weil er das Log flutet.
- `device=hw:0`: nur Eingänge loggen, deren Name diesen Text enthält.
- `max_lines=200`: höchstens so viele Zeilen pro Sekunde.

Das Log wird bei 512 KiB geleert, damit `/data` nicht vollläuft.

## Aufbau

| Pfad | Inhalt |
|---|---|
| `src/addin.c` | Konstruktor, die drei Hooks, Warteschlange, Logger-Thread |
| `src/midi.c` | MIDI-Parser (Running Status, SysEx) und Klartext je Meldung |
| `src/config.c`, `src/log.c` | `surface.conf`, Logdatei |
| `tests/` | Unit-Tests und ein Integrationstest (nachgebautes libasound, Testprogramm namens `MPC`) |
| `tools/test_host.sh` | alle Offline-Tests unter ASan und UBSan |
| `tools/build_armhf.sh` | Geräte-Build in `arm32v7/gcc:11-bullseye`; prüft GLIBC <= 2.31, DT_NEEDED und Exporte |

```sh
SKIP_INSTALL=1 tools/test_host.sh   # x86, ohne Gerät; ohne SKIP_INSTALL braucht es mpc-vst-plugins daneben
tools/build_armhf.sh                # braucht Docker mit arm/v7-Emulation
```

## Lizenz

MIT, siehe `LICENSE`.
