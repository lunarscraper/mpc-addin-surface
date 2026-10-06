# mpc-addin-surface

Ein `LD_PRELOAD`-Addin für die Akai Force (und MPC-OS-Geräte) mit zwei Funktionen:

- **Fernsteuerung:** Ein externer MIDI-Controller (z. B. Faderfox EC4) drückt Tasten der eingebauten Bedienoberfläche.
  Damit lassen sich Ansichten wie Matrix, Mixer und Track Edit direkt vom Controller umschalten.
- **Mithören:** Jede Meldung der Bedienoberfläche (Taste, Pad, Regler) landet als Zeile in einer Logdatei. So findet
  man die Codes weiterer Tasten.

**Stand:** Das Mithören (Version 0.1.0) läuft auf einer Force mit Firmware 3.9.1.0. Die Fernsteuerung (ab 0.2.0) ist
bisher nur am PC gegen ein nachgebautes libasound getestet, **nicht auf einem Gerät**.

Gerüst, Installer und Build stammen von [mpc-addin-usb-audio](https://github.com/jacob-sabella/mpc-addin-usb-audio)
und [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins).

## Voreinstellung

Ohne eigene Einstellungen gelten drei Auslöser, alle auf **MIDI-Kanal 16**:

| Controller sendet | Wirkung |
|---|---|
| CC 102, Wert 64 oder höher | Matrix |
| CC 103, Wert 64 oder höher | Mixer |
| CC 104, Wert 64 oder höher | Track Edit der Spur mit Track-Select-Code `60` |

Am EC4 also drei Encoder-Taster auf Kanal 16 mit CC 102, 103, 104 legen (drücken = 127, loslassen = 0).

## Bauen (GitHub Actions)

1. Den Inhalt dieses Ordners ins Repo hochladen (vorhandene Dateien überschreiben). Der Ordner `.github` muss mit hoch.
2. **Actions > Release > Run workflow**, Version `0.2.0`. Ergebnis: ein Release-Entwurf mit der Installations-ZIP.

## Installieren

Braucht root per SSH. Die ZIP auf die Force kopieren, entpacken und im Ordner:

```sh
sh install.sh
```

Eine vorhandene Version wird dabei ersetzt, die eigene `surface.conf` bleibt erhalten. Entfernen:

```sh
sh /data/mpc-addins/surface/uninstall.sh
```

Schnell abschalten: in `/data/mpc-addins/surface/surface.conf` die Zeile `enabled=0` setzen (oder nur `remote=0` für
reines Mithören) und die Force neu starten.

## Der Test

1. Controller per USB an die Force anschließen, Projekt öffnen.
2. Log ansehen:

```sh
cat /data/mpc-addins/surface/surface.log
```

Darin steht, welche MIDI-Geräte das Addin gefunden hat:

```
  midi device 24 'Faderfox EC4': listening on 1 port(s)
```

3. Am Controller etwas drücken oder drehen und das Log erneut ansehen. Jede Note und jeder Controller erscheint:

```
ext 'Faderfox EC4'  cc ch16 102 value 127
trigger cc ch16 102 from 'Faderfox EC4' -> matrix
```

Die Zeile `ext …` zeigt, was der Controller wirklich sendet. Passt das nicht zu den Auslösern, entweder den
Controller umstellen oder die Auslöser in `surface.conf` anpassen.

### Wenn es nicht umschaltet

- **Kein `midi device …` für den Controller:** Die Force sieht ihn nicht als MIDI-Gerät.
- **`ext …`, aber kein `trigger …`:** Kanal oder Nummer passen nicht zu einem Auslöser.
- **`trigger …`, danach `action … not completed`:** Die MPC-Software hat die eingeschleuste Taste nicht abgeholt. Das
  Addin zieht sie dann nach 0,3 s zurück, es bleibt nichts hängen. In dem Fall das Log schicken, dann braucht die
  Einschleusung einen anderen Weg.
- **`trigger …` ohne Fehler, aber falsche oder keine Reaktion:** Der Tasten-Code stimmt nicht (siehe „Codes").

## Einstellungen

`surface.conf` im Addin-Ordner, gelesen beim Start der MPC-Software. Alle Schlüssel mit Erklärung stehen in
`etc/surface.conf.example`.

Auslöser sind Zeilen der Form:

```
cc <Kanal> <Nummer> = <Aktion>      # löst aus bei Wert 64 oder höher
note <Kanal> <Nummer> = <Aktion>    # löst aus bei Note-On
```

Aktionen:

- Tasten mit Namen: `menu`, `matrix`, `note`, `mixer`, `edit`, `launch`, `stepseq`
- `trackedit:<Code>`: Edit halten und die Track-Select-Taste mit diesem Code antippen
- beliebige Tasten: `tap:<Code>`, `down:<Code>`, `up:<Code>`, mehrere Schritte durch Leerzeichen getrennt

Sobald die Datei eigene Auslöser enthält, ersetzen sie die drei voreingestellten. Beispiel mit Track Edit für zwei
Spuren:

```
cc 16 102 = matrix
cc 16 103 = mixer
cc 16 104 = trackedit:60
cc 16 105 = trackedit:62
```

Weitere wichtige Schlüssel: `source=EC4` (nur auf Geräte hören, deren Name diesen Text enthält), `log_external=0`
und `log_surface=0` (Log ruhigstellen, wenn alles läuft).

## Codes

Mit dem Mithörer abgelesen (Force, Firmware 3.9.1.0), jeweils `90 <Code> 7F` beim Drücken:

| Taste | Code |
|---|---|
| Menu | `02` |
| Matrix | `03` |
| Note | `04` |
| Mixer | `0B` |
| Edit | `25` |
| Launch | `74` |
| Step Seq | `75` |
| Track Select | `60`, `62` (je nach Spur; die übrigen bei Bedarf mithören) |

Eine weitere Taste findet man so: Taste drücken, Log ansehen, die Zeile `in hw:0,0,1  90 xx 7F` zeigt den Code `xx`.

## Wie es arbeitet

- Es hängt sich an libasounds Raw-MIDI-Funktionen, über die die MPC-Software ihre Bedienoberfläche liest.
- Ein eigener Thread hört über einen eigenen, versteckten ALSA-Sequencer-Anschluss auf externe MIDI-Geräte. Die
  Bedienoberfläche selbst, Programme (auch die eigenen Plugins mit MIDI-Port) und System-Anschlüsse bleiben außen vor.
- Bei einem Auslöser stellt er Tastenmeldungen in eine Warteschlange. Der Lese-Hook gibt sie der MPC-Software, als
  kämen sie von der Bedienoberfläche. Damit die Software dafür aufwacht, bekommt ihr Poll-Satz einen zusätzlichen
  Deskriptor (eine Pipe); bei blockierendem Lesen wartet der Hook selbst auf Gerät oder Pipe.
- Echte Daten der Bedienoberfläche werden nie verändert oder verworfen, und es wird nichts mitten in eine laufende
  Meldung eingefügt.
- Es wird nur im Prozess `/usr/bin/MPC` aktiv. In jedem anderen Prozess sind alle Hooks reine Durchreicher.

## Aufbau

| Pfad | Inhalt |
|---|---|
| `src/addin.c` | Konstruktor, Hooks, Mithör-Warteschlange, Einschleusung, Sequencer-Lauscher |
| `src/midi.c` | MIDI-Parser (Running Status, SysEx) und Klartext je Meldung |
| `src/config.c` | `surface.conf`, Auslöser und Aktionen |
| `src/seq_abi.h` | die wenigen Sequencer-Strukturen und -Konstanten, von Hand |
| `tests/` | Unit-Tests und ein Integrationstest (nachgebautes libasound, Testprogramm namens `MPC`) |
| `tools/test_host.sh` | alle Offline-Tests unter ASan und UBSan |
| `tools/build_armhf.sh` | Geräte-Build in `arm32v7/gcc:11-bullseye`; prüft GLIBC <= 2.31, DT_NEEDED und Exporte |

```sh
SKIP_INSTALL=1 sh tools/test_host.sh   # x86, ohne Gerät; ohne SKIP_INSTALL braucht es mpc-vst-plugins daneben
sh tools/build_armhf.sh                # braucht Docker mit arm/v7-Emulation
```

## Lizenz

MIT, siehe `LICENSE`.
