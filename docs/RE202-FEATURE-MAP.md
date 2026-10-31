# RE-202 Feature-Map — was davon unser Plugin bereichert

**Quelle:** Roland **RE-202** Reference Manual, © 2022 Roland Corp. (27 Seiten, gelesen 2026-10-30).
Nur als **Feature-Referenz** benutzt: keine Assets, keine IRs, keine Presets, keine Formulierungen
übernommen. Produkt bleibt „MPC Multihead Echo" (vendor `LarryDaimond`); es wird **nicht** als
RE-202-Emulation beworben — „im Geist von", wie D3 es festhält.

**Warum überhaupt:** Das RE-202-Handbuch beschreibt genau die Ebene, die unser Plugin noch nicht hat —
*Spielgesten* und *Tape-Zustand*. Und ein Teil davon ist im vendorten Kern schon implementiert und
wird von uns nicht benutzt (`setTapeAge`, `setInputGain`, `setDryLevel`, Pan/Width).

**CPU-Deckel (D5):** voll am Gerät p99 **24,4 %**, Gate **p99 ≤ 25 % / mean ≤ 23 %** → freie Luft
für **neue DSP praktisch null**. Alles, was hier empfohlen wird, ist Parameter-Ebene und kostet
0 pp; alles, was Rechenzeit braucht, steht unten ausdrücklich unter „parken".

---

## 1. Ist-Abgleich (was wir schon haben)

| RE-202 | unser Parameter | Status |
|---|---|---|
| MODE SELECTOR 1–12, 4 Heads | `mode` (12 Optionen) | **vorhanden**, aber RE-201-Satz (3 Heads; unser 12. Modus ist „Rev", nicht „dense 4-head") |
| REPEAT RATE (Bandgeschwindigkeit, Ton ändert mit) | `rate` (69,3…177,4 ms) | vorhanden |
| INTENSITY (Feedback, oszilliert ganz rechts) | `intensity` | vorhanden |
| ECHO VOL | `echo` | vorhanden |
| REVERB VOL | `reverb` | vorhanden (Feder) |
| BASS / TREBLE (Echo-EQ) | `bass`, `treble` | vorhanden |
| WOW & FLUTTER | `wow` | vorhanden (seit 2026-10-29 auch als Regler sichtbar) |
| TAPE (neu/gebraucht: Ton + Wackeln) | — | **Kern kann es** (`setTapeAge`, 3 Stufen), nicht verdrahtet |
| SATURATION (Vorstufen-Verzerrung + Sättigung) | `preamp` (Clean/Warm/Dirty) | als **3 Modi** statt als Regler |
| — | `chorus`, `dropouts`, `hiss`, `spread`, `jog`, `ui` | unsere Deltas (nicht RE-202) |

---

## 2. Delta, nach Machbarkeit sortiert

### A. Gratis — Kern kann es schon, wir rufen es nur nicht auf

| # | RE-202-Feature | Umsetzung | Kosten |
|---|---|---|---|
| A1 | **SATURATION** als Regler | `pre_set_drive(&pre, db)` neu in `preamp.c`: `drive` skalieren **und** `out_gain` mitziehen (`1/(drive·sech²(bias))`), sonst springt der Pegel. Der Regler fährt 0…+12 dB um den Modus-Grundwert; `preamp` bleibt der Charakter (Bias/EQ), der Regler die Menge. ADAA + 2×-Halbband sind schon da. | 0 pp (nur Koeffizienten) |
| A2 | **TAPE** (neu/gebraucht) | `dsp.setTapeAge()` — Kern quantisiert auf 3 Stufen (0 / 0,5 / 1). Enum „Tape: New / Used / Worn". Ändert Ton **und** Wow/Flutter von selbst. | 0 pp |
| A3 | **DIRECT ON/OFF** (nur Effekt, für Send/Return) | Toggle „Wet Only": `setDryLevel(0)`, `mix` bleibt unverändert. | 0 pp |
| A4 | **DIRECT MODE** (Vorstufe auf dem Direktsignal) | Ist bei uns schon so: die Preamp-Stufe sitzt **vor** dem Kern auf dem ganzen Signal, nicht nur auf dem Echo. Als bewusste Eigenschaft dokumentieren, nicht als Schalter. | 0 pp |
| A5 | ECHO-/REVERB-PAN, Stereo-Width | `setEchoPan`, `setReverbPan`, `setStereoWidth` — frei. **Aber:** überlappt mit unserem `spread` (eigene R-Verzögerung, bereits getestet). Kandidat zum *Aufräumen* (spread → Kern-Width), nicht zum Draufsatteln. | 0 pp |

### B. Gesten — Momentary-Parameter, kein neuer DSP

Die RE-202 ist vor allem eine **Performance-Kiste**: drei Taster mit Doppelfunktion. Das ist die
Lücke, die am meisten bringt, und sie kostet nichts.

| # | Feature | Umsetzung |
|---|---|---|
| B1 | **WARP** (ON/OFF lang drücken: „stärker werdender Hall, je länger man hält") | `warp` als Momentary. In `process_block`: `warp_t += n/(2·fs)` solange gehalten (Cap 1), sonst `warp_t → 0` mit gleicher Rampe (~200 ms). Override auf `setReverbLevel(reverb + 0,35·warp_t)` und `setIntensity(intensity + 0,15·warp_t)`, geklemmt. Muss **nach** `apply_params()` je Block laufen, sonst überschreibt der nächste Parameter-Set es. |
| B2 | **TWIST** (TAP lang drücken: „rotierendes Oszillieren des Echos") | `twist` als Momentary + `twisttype` (Normal/Hard/Natural). LFO im Modul (Sinus, ~0,7 Hz, blockweise, praktisch gratis) auf `setRepeatRate(base ± depth)`; `Normal` ±4 %, `Hard` ±9 %, `Natural` skaliert die Tiefe mit `intensity`. **Vorrangregel:** `jog` gewinnt (einseitiges Ziehen), `twist` wird dann auf 0 geblendet — beide gleichzeitig darf nicht zu einem Sprung führen. |
| B3 | **TAP → BPM-Sync** | `HAS_LFO_BPM: 1` in `vst.json`, Key `lfo_bpm` im Modul speichern; Kern hat die Umkehrfunktion `repeatRateForDelayMs()`. **Ehrliche Grenze:** der Kern läuft 69,3…177,4 ms — das ist 1/16 bei 85…173 BPM, 1/8 erst ab ~170 BPM. Absolute Syncs sind also meist außerhalb; sinnvoll ist **Sync + Drift** (D2): Zielrate aus dem Tempo, darauf ein kleiner, langsamer Drift (±0,5…2 %) — das ist auch musikalisch das, was ein Bandecho tut. |
| B4 | **Memories/Presets** (127 + MANUAL) | Auf MPC-Seite: `presets.json` + `vst/gen_presets.py`-Muster aus `../DubStation`, Ablage in `MPC Documents/Plugin Presets/Insert Effects/<vendor> - VST - <name>/` (Effekt-Presets, **nicht** der Skin-`Presets/`-Ordner). 6–8 Werks-Memories statt 127. Kein DSP. |

### C. Braucht neue DSP — nur mit CPU-Deal

| # | Feature | Warum es nicht gratis geht | Urteil |
|---|---|---|---|
| C1 | **REVERB TYPE** (Spring/Hall/Plate/Room/Ambience) | Der Feder-Tank ist ~60 % der Kernlast. Ein zweiter Algorithmus (kleines FDN) kostet auf dem Gerät grob 1,5–3 pp — wir haben 0,6 pp. Möglich nur gegen Abbau (2×-OS weg, eine Feder weniger) = Klangverlust. | **parken** |
| C2 | **TIME MODE Long** (1–2 s Tap-Delay) | Nicht nur ein Clamp: der Ringpuffer ist auf Head 3 bei langsamster Geschwindigkeit ausgelegt (~0,52 s), und die Motor-Kennlinie ist auf 69–177 ms gefittet. Beides sitzt im vendorten Kern. | **parken** (D2: Kern unangetastet) |
| C3 | **4. Playback-Head / Modus 12 „dense"** | Der Kern hat strukturell 3 Heads (`kHeadRatio[3]`). Head 4 (4× Abstand) ist kein Parameter. | **parken** |

### D. Geht auf MPC nicht

| Feature | Grund |
|---|---|
| **CARRYOVER** (Tail beim Ausschalten) | Bypass gehört dem Host; MPC ruft den Prozess nicht weiter auf, wenn der Insert aus ist. Kein Plugin-Kontrollpunkt. |
| **INPUT GUITAR/LINE**, MIDI-Kanäle, THRU, Device-ID, MEMORY EXTENT, Tastensperre | Geräte-/Systemkonfiguration der Hardware; im Insert-Kontext ohne Entsprechung. Ein Eingangs-Trim wäre nur ein zusätzlicher Gain-Regler. |

---

## 3. Vorschlag Parameter (append-only, ab Index 16)

| Idx | Key | Typ | Bereich | Seite |
|---|---|---|---|---|
| 16 | `saturation` | float | 0…1 (0 dB…+12 dB um den Modus) | ECHO |
| 17 | `tapeage` | enum | New / Used / Worn | ECHO |
| 18 | `warp` | momentary | 0/1 | PLAY |
| 19 | `twist` | momentary | 0/1 | PLAY |
| 20 | `twisttype` | enum | Normal / Hard / Natural | PLAY |
| 21 | `sync` | enum | Off / 1/16 / 1/8 / Short / Long | PLAY |
| 22 | `drift` | float | 0…1 (0 = starr) | PLAY |
| 23 | `wetonly` | toggle | 0/1 | PLAY |

Achtung Contract-Regeln: `display: int` für Enums nicht nötig (Text), `qlink_ticks: 4` für `mode`
bleibt Vorbild für `tapeage`/`twisttype`/`sync`; Momentary-Parameter (`warp`, `twist`) bekommen
keinen Regler, sondern Buttons.

**UI:** dritte Seite **PLAY** (neben ECHO/CHARACTER) mit WARP/TWIST als große Taster, `twisttype`/
`sync` als enum-Zeile, `drift` und `wetonly` klein. Q-Links: ECHO 8 → 10, CHARACTER 5, PLAY ≤ 6 —
jede Seite bleibt in **einer** Bank. Skin-Set: alle drei Layouts (`layout_{night,classic,rack}.conf`)
mit gleicher Geometrie.

---

## 4. Was jede Stufe beweisen muss

Pro Projektregel gilt: offline zuerst, dann Gerät.

1. **A1:** `preamp`-Test erweitern — THD steigt monoton mit `saturation` bei **konstantem** Pegel
   (±0,5 dB), `clean` bleibt bei `saturation=0` bit-exakt, Alias-Check (10 kHz) hält ≤ −40 dB.
2. **A2:** zwei Render mit `tapeage` 0 vs 1 — Spektrum unterschiedlich **und** Wow-Modulation
   unterschiedlich (Hüllkurve der Rate); beide endlich, keine DC-Verschiebung.
3. **B1:** 3-s-Render: Hall-Nachhall wächst monoton, während `warp` gehalten wird; nach dem Loslassen
   klingt es innerhalb ~300 ms auf den Ausgangswert zurück; max. Sample-Schritt ≤ Signalanstieg
   (kein Klick) — dieselbe Prüfung wie bei `jog`.
4. **B2:** Frequenz-/Ratenverlauf oszilliert sichtbar, `twist=0` bit-exakt, `jog`+`twist` gleichzeitig
   ohne Sprung.
5. **B3:** bei 120 BPM und `sync=1/16` liegt die gemessene erste Wiederholung bei 125 ms ± 2 %;
   außerhalb des Bereichs muss die Anzeige das sagen, statt still am Anschlag zu kleben.
6. **CPU:** nach jedem Paket `tools/bench.sh` — Gate bleibt p99 ≤ 25 %. Erwartung: 0 pp, weil nur
   Koeffizienten und Block-Rampen dazukommen. Wenn doch mehr: zurückbauen, nicht die Schwelle senken.

---

## 5. Empfehlung

**Paket 1 (A + B4) — umgesetzt 2026-10-30:** `saturation` (unser Vorstufen-Drive, +0…12 dB,
Pegel kompensiert), `tapeage` (New/Used/Worn, Kern-`setTapeAge`), `wetonly` (Kern-`setDryLevel`),
plus 9 Werks-Presets als Effekt-`.xpl` (`vst/gen_presets.py`, Ablage im
`Insert Effects`-Ordner). Dazu `state` im Modul: erst damit tragen Presets und Projekt-Restore
überhaupt Werte (vorher gab es keinen Chunk). Dritte Skin-Seite **TAPE**.
Parameter 16–18, Contract 16 → 19, Version 1000 → 1010.

**Paket 2 (B1–B3):** WARP, TWIST, Sync+Drift — Gesten statt Regler; `sync` mit Drift ist
entschieden (2026-10-30), gebaut noch nicht.
**Paket 3 (C):** nur wenn der Nutzer einen CPU-Deal eingeht (anderer Hall gegen Oversampling).
**Nicht anfassen:** C2/C3 (Kern, D2) und CARRYOVER (Host).

## 6. Entscheidungen (2026-10-30, Nutzer)

1. **Sättigung:** eigene Vorstufe (`pre_set_drive`), oversampled — nicht der Kern-`setInputGain`.
   Ein Regler, `preamp` bleibt der Charakter.
2. **Sync:** **mit Drift** (Paket 2).
3. **Presets:** ja — 9 Werks-Memories im Effekt-Preset-Ordner.

---

## 7. Die blockierten Punkte — Machbarkeit (2026-10-30)

Nachgefragt wurde: *„geht das doch?"* — hier pro Punkt, mit dem, was dagegen steht und was ein
Ausbau kosten würde. Nichts davon ist gebaut.

### 7.1 CARRYOVER — **nein, nicht als Plugin-Eigenschaft**

Der Hall-Tail soll beim Ausschalten weiterlaufen. Zwei mögliche Host-Verhalten, beide tödlich:

* **MPC ruft `process_block` nicht mehr auf**, sobald der Insert aus ist → es gibt keinen Ort, an
  dem ein Tail entstehen könnte.
* **MPC ruft weiter auf und verwirft das Ergebnis** → dann läuft der Tail, ist aber unhörbar.

Dazu kommt: der Wrapper bekommt **kein Bypass-Signal**. `effSetBypass` (Opcode 43) und
`audioMasterSetBypass` sind im Dispatcher nicht behandelt, `set_param("bypass")` gibt es nicht. Das
Plugin kann also nicht einmal *wissen*, dass es aus ist. Für einen Tail müsste MPC den Insert als
„stumm, aber weiterlaufend" fahren und uns das sagen — beides nicht vorgesehen.
**Fazit:** host-seitig, nicht lösbar. Wenn es je kommt, dann als MPC-Feature.
*(Eine CPU-Messung mit Insert an/aus würde nur bestätigen, welcher der beiden Fälle vorliegt; sie
ändert die Antwort nicht.)*

### 7.2 REVERB TYPE (Hall/Plate/Room/Ambience) — **ja, und billiger als der Status quo**

Der Denkfehler in der ersten Einschätzung: ein zweiter Hall würde *addiert*. Er **ersetzt** aber den
Feder-Tank, wenn ein Nicht-Feder-Typ gewählt ist (`setReverbLevel(0)` am Kern, eigener FDN im Modul).

| Posten | gemessen / geschätzt |
|---|---|
| Feder-Tank im Kern | ~60 % der Kernlast ≈ **11 pp** (Kern allein 18,4 pp) |
| kleiner FDN, 8 Leitungen + 2 Allpässe je Seite | ~25–30 µs/Block x86 ≈ **5 pp** (Schätzung aus den gemessenen Stufen: Chorus 2 Reads = 4,4 µs, Spread = 0,56 µs, Kern = 109 µs) |

**Netto also −6 pp**, solange ein Nicht-Feder-Typ läuft; „RE-201 SPRING" bleibt der teure Fall und
ist der Bench-Worst-Case, den wir heute schon messen (24,4 pp). Kein Gate-Problem, sondern eine
Bauaufgabe: 4 Presets (Decay/Damping/Diffusion) über einen FDN, `reverbtype`-Enum, Test auf
Nachhallzeit + Stabilität, Bench.

### 7.3 TIME MODE Long (1 s / 2 s) — **nur als Kern-Fork**

Der Ringpuffer wird aus `kMaxDelayMs` × `kHeadRatio[2]` × 1,05 berechnet (~0,52 s), und die
Motor-Kennlinie (`delayMsForRepeatRate`, kubischer Fit) ist auf 69,3–177,4 ms kalibriert; Wow/Flutter
skaliert mit der Bandgeschwindigkeit. „Long" heißt: 4 Konstanten + Kennlinie + Puffer + Wow-Skalierung
ändern — und danach die komplette Kalibrierung (12 Modi, Feder, Pitch-Verhalten) neu belegen.
Das ist ein **Fork des vendorten Kerns**, also ein bewusster Bruch von D2 („Re-Vendor = echter diff").
Technisch machbar, Aufwand ~1 Tag plus Neumessung. **Entscheidung des Nutzers, nicht des Agents.**

### 7.4 Vierter Playback-Head / Modus 12 „dense" — **dasselbe Fork-Thema**

Der Kern hat `kHeadRatio[3]` und `ppPhase[3]`; ein vierter Tap ist eine zusätzliche Hermite-Lesung
plus Pan/Ping-Pong-Slot, CPU grob +2–3 % der Kernlast (≈ +0,5 pp). Klein im DSP, groß in der
Modus-Tabelle (die Modi 8–12 der RE-202 sind *andere* Kombinationen als unsere) und in der
Verifikation. Gleiche Antwort wie 7.3: möglich, aber ein Fork.

### 7.5 Expression-Pedal mit MIN/MAX pro Memory — **nein**

Kein Pedal am MPC, und der Ersatz (Q-Link) ist ein absoluter Regler. Ein MIN/MAX-Morph bräuchte pro
Zielparameter **zwei** zusätzliche Parameter plus einen eigenen Morph-Pfad — bei 25 Parametern wäre
das Contract-Wildwuchs ohne echten Gewinn. Q-Links decken den Anwendungsfall ab.

### 7.6 CTL 1/2-Funktionen, MIDI-Block, MEMORY EXTENT — **nein / gegenstandslos**

Fußschalter- und MIDI-Zuweisung sind Hardware-Konfiguration. Preset-Auswahl („Memory") gibt es
bereits über die MPC-Effekt-Presets (9 `.xpl`), „MANUAL" ist bei uns inhärent (die Regler *sind* der
Klang). `MEMORY EXTENT` ist eine Anzeige-Einstellung ohne Entsprechung.

### 7.7 INPUT GUITAR/LINE — **ja, aber wertlos**

Wäre ein Eingangs-Trim plus Hochpass (Gitarren-Impedanz lässt sich digital nicht nachbilden). Ein
Regler mehr, den am MPC niemand braucht, weil der Insert bereits auf Line-Pegel arbeitet. **Nicht
empfohlen.**
