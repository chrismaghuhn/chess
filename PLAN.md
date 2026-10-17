# Schach-Engine: Plan

## Ziel

Eine Schach-Engine in C, die mit **so wenig Training wie möglich** so stark wie möglich wird.
Das Ergebnis ist nicht nur die Engine, sondern eine **Kurve: Elo gegen Trainingsaufwand**.

## Regeln des Projekts (Variante B)

- **Kein Vorwissen.** Die Engine kennt nur die Schachregeln. Alle Bewertungsparameter starten bei 0.
  Auch Figurenwerte (Bauer = 1, Springer = 3, …) muss sie selbst lernen.
- **Keine fremden Daten.** Kein Stockfish, keine Lichess-Partien, keine fremden Bewertungen.
  Trainingsdaten kommen ausschließlich aus eigenen Selbstpartien.
- **Messen ist erlaubt.** Gegen fremde Engines darf getestet werden, um eine absolute Elo-Zahl zu bekommen.
  Davon fließt nichts ins Training.
- **Struktur ist erlaubt, Werte nicht.** Welche Merkmale es gibt, darf ich festlegen; welches Gewicht sie
  haben, muss gelernt werden. Jedes neue Merkmal wird im Log vermerkt, weil es eine Form von Vorwissen ist.

## Architektur

```
          ┌──────────────────────────────────────────────┐
          ▼                                              │
  weights/genN.bin ──► engine selfplay ──► data/genN.bin (Stellungen + Ergebnis)
                                                         │
  weights/genN+1.bin ◄────────── tuner ◄─────────────────┘
          │
          └──► Match gen N+1 gegen gen N ──► log/generations.csv ──► tools/plot.py
```

### Verzeichnisse

```
Makefile
src/      gemeinsame Bibliothek (Engine und Tuner)
          board.[ch]     Bitboards, Copy-Make, FEN
          attacks.[ch]   Angriffstabellen (erst einfache Schleifen, später Magic Bitboards)
          movegen.[ch]   pseudo-legale Züge + Legalitätscheck
          perft.[ch]     Perft und Divide (auch für UCI `go perft` später)
          zobrist.[ch]   Hashing für TT und Stellungswiederholung
          game.[ch]      Partie: Hash-Historie, Matt/Patt/Remis-Regeln, Züge per UCI-Name
          tt.[ch]        Transposition Table
          features.[ch]  Stellung → Merkmalsindizes (EIN Code für Engine und Tuner)
          eval.[ch]      lineare Bewertung, lädt Gewichtsdatei (Gen 0: immer 0)
          search.[ch]    Negamax/Alpha-Beta, Quiescence, TT, Zugsortierung
          datafmt.[ch]   gepacktes Stellungsformat für Trainingsdaten
engine/   main.c, uci.c, selfplay.c         → Binary `engine` (UCI; `bench`, `selfplay`, `data-info`)
tuner/    main.c, texel.[ch]                → Binary `tuner`
tests/    perft.c (Perft-Suite), search.c (Matt-Orakel, Remis, legale Partien),
          eval.c (Merkmale, Gewichte, Datenformat), uci.sh, selfplay.sh → `make test`
tools/    generations.sh (Trainingsschleife), anchor.sh (Läufe auf eine Elo-Skala), plot.py (SVG, ohne Zusatzpakete)
runs/<name>/  ein Trainingslauf: generations.csv, generations.svg, weights/, logs/,
              data/ (Trainingsdaten, nicht in git)
```

### Build

- C11, gcc oder clang, einfaches `Makefile`
- `make` baut mit `-O2 -march=native`
- `make debug` baut mit `-O0 -g -fsanitize=address,undefined`; Debug-Builds prüfen
  zusätzlich an jedem Perft-Knoten, ob Bitboards, Mailbox und Hash zusammenpassen
- `make test` führt Perft-Suite (bis 25 Mio. Knoten), Suchtests und UCI-Test aus (~5 s);
  `make test-full` mit voller Perft-Suite (~1,5 Mrd. Knoten, ~40 s); `make test-debug` alles mit Sanitizern
- `make bench`: Suche bis Tiefe 7 auf 6 Stellungen. Die Knotenzahl ist ein Fingerabdruck:
  Sie ändert sich genau dann, wenn sich das Suchverhalten ändert.
- `build/release/perft divide <tiefe> <fen|startpos>` zum Eingrenzen von Perft-Fehlern
- Ausgaben landen in `build/<mode>/`, Objektdateien in `build/<mode>/obj/`

## Design-Entscheidungen

### Board
- 12 Bitboards (6 Figurentypen × 2 Farben) plus Belegungs-Bitboards
- Seite am Zug, Rochaderechte (4 Bits), en-passant-Feld, Halbzugzähler, Zobrist-Hash
- **Copy-Make**: Ein Zug erzeugt eine Kopie des Boards (~200 Bytes). Das ist einfacher als Make/Unmake und kaum langsamer.

### Zuggenerierung
- Pseudo-legale Züge erzeugen, nach dem Ausführen prüfen, ob der eigene König angegriffen ist
- Zugliste als festes Array `Move moves[MAX_MOVES]`, kein `malloc`
- Gleitende Figuren erst mit einfachen Strahl-Schleifen; Magic Bitboards erst, wenn Perft stimmt und Tempo zählt

### Bewertung (linear, ohne Vorwissen)
- Merkmale: **Figurentyp × Feld**, aus Sicht der Seite am Zug, für Schwarz gespiegelt
- 6 × 64 = **384 Gewichte**; eigene Figuren zählen +1, gegnerische −1 (gespiegelt).
  Die Farbsymmetrie ist eine Regel des Spiels, kein Schachwissen.
- `eval = Σ Gewicht[Merkmal]` über alle aktiven Merkmale (max. 32 pro Stellung)
- Figurenwerte entstehen implizit als Mittelwert über die Felder
- Bewusst **nicht** dabei: Mobilität, Königssicherheit, Bauernstruktur, Spielphase (tapered eval).
  Das kann später als zusätzliches Merkmal kommen, Werte immer gelernt.
- Gewichtsdatei: `SCHW`, Version, Anzahl Merkmale (je uint32 LE) + float32-Gewichte; die Engine rundet auf
  int32 und kappt bei ±10000, die Bewertung bei ±20000 (weit weg von Matt-Werten). Laden per
  `selfplay --weights FILE` oder UCI `setoption name Weights value FILE`; ohne Datei sind alle Gewichte 0.
- Getestet: Startstellung ergibt für beliebige Gewichte 0, und Farbtausch + Spiegeln ändert die Bewertung nicht
- Kosten: Die Bewertung summiert alle Merkmale neu (6,2 statt 8,8 Mio. Knoten/s). Inkrementell geht später.

### Suche
- Negamax mit Alpha-Beta, Iterative Deepening
- Quiescence Search (nur Schlagzüge und Umwandlungen; im Schach alle Ausweichzüge, ohne Stand-Pat)
- Transposition Table (ein Eintrag pro Slot, immer ersetzen)
- Schach-Erweiterung: im Schach wird eine Halbzug tiefer gesucht
- Zugsortierung **ohne Vorwissen**: TT-Zug, dann Schlagzüge und Umwandlungen, dann Killer-Züge,
  dann History-Heuristik. History und Killer lernt die Suche selbst. Schlagzüge werden per MVV-LVA mit
  **gelernten** Figurenwerten sortiert (Mittel der Gewichte je Figurentyp); bei Gen 0 sind alle gleich.
  Ohne diese Sortierung explodiert die Quiescence Search in chaotischen Stellungen: Der Tuner brauchte
  für Gen 2 über 5 CPU-Stunden, mit Sortierung dauert dieselbe Auflösung Sekunden.
  Umwandlungen werden in Kodierungsreihenfolge erzeugt (Springer zuerst), nicht „Dame zuerst“.
- Später: Null-Move-Pruning, Late Move Reductions, Aspiration Windows
- Remis-Erkennung: Patt, Wiederholung (in der Suche zählt schon die zweite), 50-Züge-Regel, ungenügendes Material
- UCI: Suche im eigenen Thread; `go` mit depth, nodes, movetime, wtime/btime/winc/binc/movestogo, infinite, perft;
  Option `Hash`; `d` zeigt das Brett. Züge von außen werden gegen die legalen Züge geprüft.

### Selbstpartien
- Laufen direkt in der Engine (`engine selfplay ...`), mehrere Threads, ohne UCI-Overhead
- **Feste Knotenzahl pro Zug** statt Zeit, damit der Aufwand reproduzierbar und vergleichbar ist
- Eröffnungsvielfalt: die ersten 8 Halbzüge zufällig (gleichverteilt über legale Züge)
- Zufälliger Tie-Break: Mit Seed wählt die Wurzel zufällig unter **allen gleich bewerteten** Zügen
  (spätere Wurzelzüge werden dafür mit alpha − 1 gesucht, damit „gleich“ von „schlechter“ unterscheidbar
  ist; pro Tiefe neu gewürfelt). Ein bloßes Mischen der Zugliste reicht nicht: Dann gewinnt bei
  Gleichstand immer die Sortierung, und Gen 0 hätte immer geschlagen, wenn es ging.
- Partieende nur nach Regeln: Matt, Patt, Wiederholung, 50 Züge, ungenügendes Material,
  plus festes Zuglimit (z. B. 400 Halbzüge → Remis). **Nie** nach Material abbrechen, das wäre Vorwissen.
- Gespeichert wird jede Stellung nach der Eröffnungsphase mit dem Endergebnis der Partie
- **Reproduzierbar:** Partie i hat immer denselben Seed, Partien werden in Nummernreihenfolge geschrieben.
  Gleicher Seed → bitgleiche Datei, egal mit wie vielen Threads (wird getestet).
- Aufruf: `engine selfplay --out FILE [--games N] [--nodes N] [--threads N] [--weights FILE]
  [--seed N] [--random-plies 0..40, Standard 8] [--max-plies 400] [--hash MB]`. Am Ende eine Zeile
  `summary games=… positions=… nodes=… cpu_s=… wall_s=…` für das Generationen-Log.
- Zufällige Eröffnungen, in denen die Partie schon endet (auch Wiederholung), werden neu ausgewürfelt
- Bei einem Schreibfehler bricht `selfplay` ab, ohne `summary`-Zeile, mit Exit-Code 1
- `engine data-info FILE [n]` zeigt Ergebnisverteilung, Partieenden, Stellungen im Schach und n Beispiel-FENs
  und prüft die Labels: innerhalb einer Partie wechselt das Ergebnis mit der Seite am Zug, nur Matt ist
  entschieden, und die letzte Stellung einer Mattpartie gehört dem Gewinner
- Erster Lauf (Gen 0, 200 Partien, 5000 Knoten/Zug, 8 Threads): 1,5 s Wandzeit, 12 s CPU,
  15.469 Stellungen; 51 Partien durch Matt entschieden, der Rest meist Wiederholung

### Trainingsdaten-Format
- 16 Byte Header: `SCHD`, Version, Record-Größe, reserviert (je uint32 little-endian)
- 32 Byte pro Stellung:

  | Bytes | Inhalt |
  |---|---|
  | 0–7 | Belegungs-Bitboard |
  | 8–23 | Figurencodes (0–11) der belegten Felder, niedrigstes Feld zuerst, 2 pro Byte |
  | 24 | Seite am Zug |
  | 25 | Rochaderechte |
  | 26 | en-passant-Feld (64 = keins) |
  | 27 | Halbzugzähler, gekappt bei 255 |
  | 28 | Ergebnis aus Sicht der Seite am Zug: 0 verloren, 1 remis, 2 gewonnen |
  | 29 | Grund für das Partieende (`GameEnd`: Matt, Patt, Wiederholung, 50 Züge, Material, Zuglimit) |
  | 30–31 | Halbzug der Stellung in der Partie |

- Partiegrenzen: Die Stellungen einer Partie stehen hintereinander, eine neue Partie beginnt, wo der Halbzug
  nicht um 1 steigt (wichtig für eine Trainings-/Validierungs-Aufteilung nach Partien)
- Beim Entpacken gelten dieselben Legalitätsprüfungen wie bei FEN (`board_setup`); ungenutzte Nibbles
  und das reservierte Header-Wort müssen 0 sein
- 10 Mio. Stellungen ≈ 320 MB

### Tuner (Texel-Tuning)
- `tuner --out FILE --data FILE [--data …] [--init FILE] [--epochs 400] [--lr 2] [--threads N] [--val-every 10]`
- Löst jede Stellung mit `search_quiet` (nur Quiescence, mit den `--init`-Gewichten) zur ruhigen
  Stellung auf; deren Merkmale (gleicher `features`-Code wie die Engine) werden einmal vorberechnet.
  Bei Gen 0 (alle Gewichte 0) ist jede Stellung schon „ruhig“.
- Verlust: `(Ergebnis − sigmoid(eval · ln10/400))²`, Ergebnis 0 / 0,5 / 1 aus Sicht der Seite am Zug.
  Die feste Skala macht die gelernten Einheiten ungefähr zu Centipawns.
- Full-Batch-Adam; Gradienten in 64 festen Blöcken summiert → bitgleiche Gewichte für jede Thread-Anzahl
- Validierung: jede 10. Partie (Partiegrenzen über den Halbzug). Behalten werden die Gewichte der
  Epoche mit der kleinsten Validierungs-Loss (Early Stopping); mit `--val-every 0` die letzten.
- Gibt die gefundenen Figurenwerte aus (Mittel über die gesehenen Felder), z. B. nach Gen 0:
  `P=36 N=41 B=51 R=85 Q=156` – die Reihenfolge hat ihm niemand verraten
- Die Generationsschleife trainiert auf **allen** bisherigen Daten und startet bei den Vorgänger-Gewichten

### Messung
- **Trainingsaufwand** = kumulierte CPU-Sekunden für Selbstpartien + Tuning
  (zusätzlich Anzahl Partien und Stellungen). Testmatches zählen nicht dazu.
- **Elo pro Generation**: `engine match` (eigener Match-Runner, `cutechess-cli` ist nicht verfügbar):
  Partiepaare mit vertauschten Farben aus derselben Zufallseröffnung, feste Knotenzahl, reproduzierbar,
  Elo mit 95-%-Intervall aus der Varianz pro Partie. Gemessen gegen die Vorgeneration (verkettet zur
  Kurve, Fehler addieren sich quadratisch) und gegen Gen 0 (sättigt schnell bei ~100 %).
- **Code-Änderungen** (Suche etc.) per Match gegen die vorige Version (SPRT später)
- `tools/generations.sh RUN_DIR N` (Einstellungen per Umgebung: GAMES, NODES, MATCH_GAMES, EPOCHS, THREADS)
  schreibt `RUN_DIR/generations.csv`: `gen, games, positions, selfplay_cpu_s, tune_cpu_s, cum_cpu_s,
  val_loss, elo_vs_prev, err_vs_prev, elo_chain, err_chain, elo_vs_gen0, err_vs_gen0, pieces`.
  Abgebrochene Läufe setzen beim nächsten Aufruf fort. `tools/plot.py` zeichnet daraus die Kurve als SVG.
- Absoluter Anker: am Ende Matches gegen Engines mit bekannter CCRL-Elo (nur Messung)

## Meilensteine

| # | Inhalt | Fertig, wenn … |
|---|---|---|
| 1 | Board, FEN, Zuggenerator, Perft | alle Standard-Perft-Werte stimmen (Startstellung, Kiwipete, Positionen 3–6), ohne Sanitizer-Fehler |
| 2 | Suche + UCI, alle Gewichte = 0 | Gen 0 spielt in einer GUI/cutechess legale Partien und findet Matt in 3 |
| 3 | Lineare Bewertung, Gewichtsdatei, Selbstpartien, Datenformat | `engine selfplay` erzeugt Daten und misst die CPU-Zeit |
| 4 | Tuner + Generationsschleife + Match-Messung | erste Kurve mit ≥ 5 Generationen; Gen N schlägt Gen 0 messbar |
| 5 | Tempo (siehe unten) | Lauf 2 mit der schnellen Engine erreicht dieselbe Stärke wie Lauf 1 mit deutlich weniger CPU-Zeit |
| 6 | Reichere Bewertung (Experiment) | verworfen, siehe unten: kostet mehr Tempo, als es Elo bringt |
| 7 | Knotenzahl pro Zug | welche Knotenzahl ergibt die meiste Stärke pro CPU-Sekunde Training |
| 8 | Absolute Elo | Matches gegen Stockfish mit gedrosselter Stärke (nur Messung) |
| 9 | NNUE (Experiment) | gestoppt: bessere Loss, aber schwächeres Spiel; Nebenbefund: lineares Modell auf allen Daten +97 Elo |

### Meilenstein 5: Tempo

80 % des Trainingsaufwands sind Selbstpartien. Jede Beschleunigung der Engine verschiebt die Kurve nach links.

Profil einer Selbstpartie (Gen-8-Gewichte, 5000 Knoten/Zug, gprof): Zugsortierung ~24 %, Bewertung ~18 %,
Angriffe gleitender Figuren ~13 %, Legalitätsprüfung + Zug ausführen ~12 %, Zuggenerierung ~9 %.

- **A. Schneller bei gleichem Verhalten.** Prüfstein: Bench-Fingerabdruck und Selbstpartien (mit und ohne
  Gewichte) bleiben bitgleich, nur die CPU-Zeit sinkt.
  1. Inkrementelle Bewertung: Die Suche rechnet pro Zug nur die Änderung, statt alle Figuren neu zu summieren
  2. Billigere Sortierung: Hash-Zug zuerst probieren, die übrigen Züge erst bewerten, wenn er nicht abschneidet
  3. Magic Bitboards für Läufer, Türme, Damen
  4. Legalität über Fesselungen: Nur Königszüge, gefesselte Figuren, en passant und Züge im Schach brauchen den Test
  **Ergebnis Phase A** (64 Selbstpartien, 5000 Knoten/Zug, 16 Threads, bitgleiche Ausgabe):
  Gen-8-Gewichte 10,8 → 8,3 s CPU (−23 %), Null-Gewichte 13,1 → 9,6 s (−27 %); Bench 6,5 → 8,7 Mio.
  Knoten/s; volle Perft-Suite 37 → 53 Mio. Knoten/s. Schritt 2 (Hash-Zug vor dem Bewerten) ist
  zurückgestellt: Die übrigen Züge sähen dann neuere History-Werte, das ändert die Reihenfolge – und
  brachte kein messbares Tempo. Die Startzeit wächst um 0,12 s (Suche nach den magischen Zahlen).
- **B. Messwerkzeug für Suchänderungen:** Match zwischen zwei Engine-Binaries über UCI (Knoten oder Zeit),
  damit eine neue Suche gegen die alte bei gleicher CPU-Zeit antreten kann; später SPRT
- **C. Schlauere Suche** (ändert das Verhalten, jeweils per Match bei gleicher CPU-Zeit geprüft):
  PVS, Null-Move-Pruning, Late Move Reductions, Aspiration Windows. Der Zufalls-Tie-Break an der Wurzel muss erhalten bleiben.
  **Ergebnis Phase B/C** (Gen-8-Gewichte auf beiden Seiten, 1000 Partien, ±20 Elo = 95 %):
  `engine match --a-search/--b-search` schaltet die Verfahren pro Seite (`selfplay --search`, `bench … OPTS`).

  | Vergleich (5000 Knoten/Zug) | Elo |
  |---|---|
  | PVS gegen nichts | −6 |
  | PVS + Null-Move gegen PVS | −6 |
  | PVS + LMR gegen PVS | +14 |
  | alle drei gegen nichts | +8 |
  | Wurzelzüge mischen statt exakte Gleichstände suchen (ab Gen 1) | +13 (mit allen dreien: +26) |

  | Knoten pro Zug (gleiche Suche) | Elo gegen 5000 |
  |---|---|
  | 10000 | +147 … +168 |
  | 2500 | −195 |
  | 1000 | −391 |
  | 500 | −569 |
  | 200 | −827 |

  - Die Suchverfahren bringen bei 5000 Knoten (Tiefe ~4–5) wenig; sie sind trotzdem Standard (`SEARCH_DEFAULT`),
    weil sie nichts kosten und bei längerer Bedenkzeit mehr bringen.
  - Exakte Gleichstände an der Wurzel kosten fast einen Halbzug Tiefe. Sie bleiben nur bei Null-Gewichten
    (Gen 0, wo jeder Zug gleich ist); mit gelernten Gewichten werden die Wurzelzüge nur gemischt.
    Überraschend: Der Halbzug mehr Tiefe ist kaum Elo wert, doppelte Knoten dagegen ~+150.
  - **Die Knotenzahl ist ein riesiger Stärkefaktor** (~+150 Elo pro Verdopplung). Ob mehr Knoten pro
    Zug auch besseres *Training* pro CPU-Sekunde ergeben, muss Lauf 2 zeigen.
  - Bench jetzt mit Tiefe 8 und allen Verfahren (Fingerabdruck 10196665 Knoten bei Null-Gewichten);
    `bench 7 exact` = 14201976 ist der Fingerabdruck der Trainings-Engine (gleich wie vor Meilenstein 5).
- **D. Lauf 2** mit der schnellen Engine und gleichen Einstellungen; Kurve gegen Lauf 1 auf der CPU-Achse

  **Ergebnis Phase D** (Gewichte direkt gegeneinander, 5000 Knoten, 2000 Partien = ±14 Elo):

  | Lauf | Suche in den Selbstpartien | CPU für 8 Gen. | Gen 8 gegen Lauf 1 Gen 8 |
  |---|---|---|---|
  | 1 | alte Engine (exakte Gleichstände) | 4824 s | – |
  | 2 | PVS+NMP+LMR, Mischen; NMP auch in Gen 0 (Fehler) | 3940 s | −20 |
  | 3 | wie 2, ohne NMP in Gen 0 | 3962 s | −34 |
  | 4 | `exact` (schnelle Engine, sonst wie Lauf 1) | **3802 s (−21 %)** | identisch (alle Gewichte bitgleich) |

  - Lauf 2 und 3 sind gleich stark (±0), lernen pro Generation aber ~25 Elo weniger als Lauf 1.
    Bei gleicher CPU-Zeit (Lauf 2/3 Gen 8 mit 3940 s gegen Lauf 1 Gen 7 mit 4184 s) sind sie gleich stark
    (+5 / +1): Das Tempo wird von schlechteren Trainingsdaten aufgefressen. Vermutete Ursache: Das bloße
    Mischen an der Wurzel macht die Selbstpartien weniger vielfältig als echte Zufalls-Gleichstände.
  - Null-Move ist bei Null-Gewichten unsinnig (jede Stellung ist „≥ beta“) und nahm Gen 0 die Suche;
    jetzt ist es dort aus.
  - Das Early Stopping im Tuner wählt ab Gen ~3 praktisch zufällige Epochen: Die Validierungs-Loss ist
    flach bis aufs Rauschen (~1e-4). Das lineare Modell ist an seiner Kapazitätsgrenze.
  - Konsequenz: Fürs Training ist `--search exact` Standard (`SEARCH` in `generations.sh`). Damit erzeugt
    die schnelle Engine bitgleich dieselben Daten wie die alte (geprüft an Gen-1-Daten von Lauf 1), mit
    ~21 % weniger CPU. PVS/NMP/LMR + Mischen bleiben Standard fürs Spielen (UCI).
  - Läufe speichern den Such-Fingerabdruck der Engine in `settings.txt` und lassen sich nur mit einer
    gleich suchenden Engine fortsetzen (Lauf 2/3 sind mit ihrer Engine abgeschlossen).
  - Lauf 4 bestätigt das: alle 8 Generationen, Gewichte und Elo-Werte identisch zu Lauf 1, 21 % weniger CPU
    (`runs/speed-comparison.svg`). **Meilenstein 5 erreicht.**
  - Nächste Hebel: mehr Ausdruckskraft der Bewertung (Merkmale), und die Frage nach der besten Knotenzahl
    pro Zug (doppelte Knoten ≈ +150 Elo Spielstärke, aber halb so viele Partien pro CPU-Sekunde).

### Meilenstein 6: reichere Bewertung (Experiment, verworfen)

Code auf dem Branch `m6-features` (nicht in `main`). Idee: mehr Ausdruckskraft, weil die Kurve ab Gen ~3 flach
wird und die Validierungs-Loss auskonvergiert ist. Merkmale (Struktur vorgegeben, Werte gelernt, 918 statt 384
Gewichte): Spielphase (jedes Merkmal für Mittel- und Endspiel, gemischt nach Zahl der Figuren ohne Bauern/Könige),
Mobilität (Tabelle pro Figurentyp und Feldanzahl), Freibauer je Reihe, Doppel-/isolierter Bauer, Läuferpaar.
Alte Gewichtsdateien laden in beide Phasen und ergeben bitgenau das alte Verhalten (gepinnter Test).

| Messung | Ergebnis |
|---|---|
| Lauf 5 (neue Bewertung, sonst wie Lauf 4), Gen 8 gegen Lauf 4 Gen 8, gleiche Knoten | **−128 ± 16 Elo**, bei 5607 statt 3802 CPU-s |
| Ablation auf Gen-0-Daten (Validierungs-Loss, kleiner = besser) | alt 0,1359 · +Phase 0,1385 · +Mobilität 0,1363 · +Bauern 0,1364 · alles 0,1385 |
| Ablation auf allen Daten von Lauf 4 (Start: Lauf 4 Gen 8) | alt 0,1227 · +Phase 0,1217 · +Mobilität 0,1220 · +Bauern 0,1223 · alles 0,1214 |
| Alle Merkmale (auf Lauf-4-Daten trainiert) gegen Lauf 4 Gen 8 | +13 ± 14 Elo bei gleichen Knoten, **−68 ± 15** bei gleicher CPU (30 % weniger Knoten) |
| Nur Spielphase / nur Bauernstruktur+Läuferpaar gegen Lauf 4 Gen 8 | −7 ± 14 / +18 ± 14 |
| Tempo des neuen Codes mit alten Gewichten (Merkmale ungenutzt) | 6–11 % langsamer; mit Merkmalen ~40 % langsamer pro Knoten |

Erkenntnisse:
- **Überanpassung durch zu wenige echte Stichproben:** Das Label gehört zur Partie, nicht zur Stellung – 2000
  Partien sind für 918 Gewichte zu wenig. Mit den Zufallspartien der ersten Generationen lernt das reiche Modell
  Rauschen, spielt schlechter und erzeugt schlechtere Daten (Abwärtsspirale in Lauf 5). Allgemeines L2 hilft
  nicht; ein Strafterm nur auf die zusätzlichen Freiheiten (Mittel- minus Endspielgewicht, Strukturgewichte)
  hält Gen 0 auf altem Niveau, kostet aber den Gewinn auf guten Daten.
- **Bessere Loss ist nicht gleich mehr Elo:** Selbst auf guten Daten bringt die geringere Validierungs-Loss
  kaum Spielstärke (+13 Elo). Bei dieser Spielstärke entscheidet Taktik, also die Knotenzahl (~+150 Elo pro
  Verdopplung), nicht positionelle Feinheiten – und die teurere Bewertung kostet genau Knoten.
- Mögliche spätere Wege, falls die Frage wiederkommt: einfaches Modell für die ersten Generationen und reiche
  Merkmale erst ab guten Daten (Curriculum); Bauernstruktur per Pawn-Hash billig machen; mehr Partien pro
  Generation. Die Werkzeuge dafür (Phasen-Merkmale, Ablations-Ergebnisse) liegen auf dem Branch.

### Meilenstein 7: Knotenzahl pro Zug

Frage: Wie viele Knoten pro Zug ergeben die meiste Spielstärke pro CPU-Sekunde Training? Mehr Knoten heißen
bessere Partien (≈ +150 Elo pro Verdopplung), aber weniger Partien pro CPU-Sekunde.

Läufe wie Lauf 4 (2000 Partien/Generation, `exact`), nur die Knotenzahl variiert. Weil jeder Lauf intern mit
seiner eigenen Knotenzahl misst, spielt jede Generation zusätzlich gegen einen gemeinsamen Anker
(`tools/anchor.sh`: Lauf 4 Gen 8, 5000 Knoten, 1000 Partien, ±20 Elo). Grafik: `runs/nodes-comparison.svg`.

| CPU-s Training | 500 Knoten (Lauf 6) | **1500 Knoten (Lauf 7)** | 5000 Knoten (Lauf 4) | 15000 Knoten (Lauf 8) |
|---|---|---|---|---|
| ~1400 | −418 (G7) | −170 (G6) | −362 (G3) | −554 (G1) |
| ~1800 | −326 (G8) | **−105** (G8) | −234 (G4) | – |
| ~2900 | −191 (G12) | **−7** (G12) | −61 (G6) | – |
| ~3800 | −184 (G15) | – | −4 (G8) | −274 (G3) |
| ~7500 | – | – | – | −14 (G6) |

- **1500 Knoten sind am effizientesten:** Endstärke von Lauf 4 mit 2882 statt ~3700 CPU-s (−24 %), früh im
  Training über 100 Elo vor 5000 Knoten. Neuer Standard in `generations.sh`.
- **500 Knoten sind zu wenig:** Die Partien werden so schlecht, dass das Lernen bei ~−170 stecken bleibt.
- **15000 Knoten** lernen pro Generation am meisten (Gen 6: −14 gegen −61 bei 5000), sind pro CPU-Sekunde aber
  klar schlechter.
- Zusammen mit Meilenstein 5: Lauf 7 erreicht die Stärke von Lauf 1 mit 2882 statt 4824 CPU-s (−40 %).
- Offen: das Optimum zwischen 1000 und 3000 Knoten genauer; ob es sich mit stärkeren Gewichten verschiebt
  (später mehr Knoten?); Partien pro Generation als zweiter Regler.

### Meilenstein 8: absolute Elo

`engine match --b-uci PATH --b-option Name=Value --movetime MS` lässt eine externe UCI-Engine als Gegner B spielen
(eigener Prozess; jeder ihrer Züge wird gegen unsere legalen Züge geprüft). Referenz: Stockfish 16 aus dem
Paketsystem mit `UCI_LimitStrength=true` und `UCI_Elo` (gegen CCRL kalibriert). Reine Messung – nichts davon
fließt ins Training. Logs: `runs/anchor-stockfish/`.

Lauf 7 Gen 12 (stärkste Gewichte, ~48 CPU-Minuten Training), PVS+NMP+LMR, beide Seiten 0,1 s pro Zug, je 400 Partien:

| Stockfish `UCI_Elo` | Ergebnis | Elo-Differenz | Schätzung |
|---|---|---|---|
| 1800 | +230 =20 −150 | +70 ± 34 | 1870 |
| 1900 | +202 =21 −177 | +22 ± 33 | 1922 |
| 2000 | +166 =16 −218 | −45 ± 34 | 1955 |

- **Etwa 1900 Elo auf der Stockfish-Skala** bei 0,1 s pro Zug. Statistisch ±20, aber Stockfishs Stufen sind auf
  längere Bedenkzeiten kalibriert: Bei 0,1 s liegen seine Stufen 1800 und 2000 für uns nur ~115 statt 200 Elo
  auseinander. Ehrlicher ist daher **1900 ± 100**, und nur für diese Bedenkzeit.
- Einordnung: unter Engines eher schwach (Spitze > 3500), etwa im Bereich einfacher Lehr-Engines mit
  handgeschriebener Bewertung – erreicht ohne jedes Schachwissen in ~3 Minuten Wandzeit Training.

### Meilenstein 9: kleines NNUE (Offline-Test, Entwurf – nach Review überarbeitet)

Ziel: billig klären, ob ein kleines Netz auf unseren eigenen Daten das lineare Modell schlägt, bevor es in die
Trainingsschleife kommt (und bevor ein großer Lauf, z. B. auf AWS, sich lohnt). Lehre aus Meilenstein 6: Das Label
gehört zur Partie; 918 Gewichte waren für 2000 Partien zu viele.

**Daten:** alle eigenen Läufe ohne bitgleiche Dateien (Lauf 4 = Lauf 1; Lauf 5 Gen 0 = Lauf 1 Gen 0):
65 Dateien, 130.000 Partien, ~14,1 Mio. Stellungen. Achtung: Alle Läufe nutzen pro Generation denselben Seed, also
dieselben Eröffnungen – nur ~32.000 verschiedene Eröffnungen; die effektive Stichprobe liegt zwischen 32k und 130k.
Aufteilung nach der Partienummer *innerhalb jeder Datei*: Test = Nummer % 10 == 0 (nur für den Bericht),
Validierung = % 10 == 5 (Early Stopping), Rest Training. Damit landen gleiche Eröffnungen aller Läufe immer im
selben Teil. Auflösung zur ruhigen Stellung mit den linearen Gewichten von Lauf 7 Gen 12 für beide Modelle.

**Netz** (`src/nnue.[ch]`): Bewertung = linearer Figur-Feld-Teil (768, Start: Lauf 7 Gen 12) + Netz. Das Netz lernt
nur den Rest, kann also nicht schlechter als linear starten, und der lineare Teil liefert weiter die gelernten
Figurenwerte für die Zugsortierung. Netz: 768 Eingänge pro Perspektive (eigene/gegnerische Figur × Typ × Feld, für
Schwarz gespiegelt), geteilte erste Schicht, H = 32/64 pro Perspektive, Clipped ReLU auf [0, 1],
Ausgang `400 · (w2 · [h_am_Zug, h_Gegner] + b2)`. Init: b1 = 0,5, w1 ~ U(±0,05), w2 ~ ±1/√(2H), b2 = 0.
Gewichte während des Trainings auf die späteren Quantisierungsgrenzen begrenzt.

**Training** (`tuner/nnue_train.c`, CPU, C): Verlust wie Texel. Mini-Batch-Adam (Batch 16384, lr ~1e-3 mit Abfall),
Weight Decay auf w1, Gradienten in festen Blöcken summiert (bitgleich für jede Thread-Anzahl), Reihenfolge und
Initialisierung mit festem Seed, Validierung jede halbe Epoche, Anteil toter/gesättigter Neuronen im Log,
CPU-Zeit wird gemessen und berichtet. Prüfungen: Gradiententest (finite Differenzen), Trainer-Vorwärtsrechnung
= `nnue_evaluate`, Farbtausch-Symmetrie, Überanpassen von 1000 Stellungen gelingt.

**Lernkurve:** linear und Netz jeweils auf 2k / 8k / 32k / 130k Partien – zeigt, ab welcher Datenmenge das Netz
gewinnt, also ob und wann es in der Trainingsschleife (2000 Partien pro Generation) trägt.

**Vergleich in der Engine:** eine einzige Bewertungsstelle für alle Aufrufe (Blätter, Stand-Pat, Null-Move-Bedingung).
Matches pro Seite mit Netz oder ohne, je 2000 Partien: bei 5000 Knoten mit `all` und bei 1500 Knoten mit `exact`
(Trainingsbedingung). Das Netz wird zum Testen an jedem Blatt neu gerechnet (misst nur Qualität). Tempo: Prototyp
eines inkrementellen Float-Akkumulators im Copy-Make, gemessen per `bench`.

**Go/Stop-Regel (vorab festgelegt):** Weiter nur, wenn das beste Netz bei gleichen Knoten mehr Elo bringt als sein
Tempoverlust kostet – ~150 · log2(Verlangsamung), z. B. +60 Elo bei 1,3× langsamer – mit dem 95-%-Intervall
oberhalb dieser Schwelle. Sonst Stopp und dokumentieren.

**Danach (falls es trägt):** Generationsschleife mit Netz und **Gate**: Eine neue Generation übernimmt die
Selbstpartien nur, wenn sie die aktuelle im sequentiellen Test besteht (Partiepaare, pentanomial, SPRT elo0 = 0,
elo1 = +25…30, α = β = 0,1, abgebrochen nach ~800 Partien); sonst bleibt das alte Netz, die neuen Daten werden
trotzdem verwendet. Die CPU-Zeit des Gates zählt zum Trainingsaufwand; die ehrliche Kurve kommt weiter aus den
Anker-Matches (das Gate selbst überschätzt die Fortschritte).

**Ergebnis** (Daten wie oben, 130.000 Partien; Logs und Netze in `runs/nnue-offline/`):

| Modell | Test-Loss | Training (CPU) | Elo, 2000 Partien |
|---|---|---|---|
| linear über den Netz-Pfad (H = 0) | 0,1261 | 588 s | Bezug |
| Netz H = 32 | **0,1187** | 1916 s | **−62 ± 15** (5000 Knoten, `all`), −60 ± 15 (1500, `exact`) |
| Netz H = 64 | 0,1189 | 3360 s | – (nicht besser als H = 32) |
| nur späte Generationen (52.800 Partien): linear / H = 32 | 0,1168 / **0,1140** | 202 / 884 s | H = 32: **−156 ± 16** |

- **Stopp nach der vorab festgelegten Regel:** Das Netz sagt Ergebnisse besser vorher, spielt aber deutlich schlechter,
  schon bei gleicher Knotenzahl. Die Bewertungen typischer Stellungen sehen vernünftig aus (kein offensichtlicher
  Fehler). Wahrscheinlichste Ursache: Verteilungsverschiebung – trainiert wird nur auf ruhigen Stellungen aus echten
  Partien, die Suche bewertet aber Millionen Stellungen, die so nie vorkommen; dort extrapoliert das lineare Modell
  (Material bleibt Material), das Netz nicht, und Alpha-Beta findet genau diese Fehler.
- **Nebenbefund: mehr Daten helfen dem linearen Modell stark.** Mit dem normalen Tuner auf allen 130.000 Partien
  nachtrainiert (Start Lauf 7 Gen 12, 1328 CPU-s): **+97 ± 15 Elo** bei gleichen Knoten, **+168 ± 36** bei 0,1 s pro Zug
  gegen Lauf 7 Gen 12 – grob 2050–2100 auf der Stockfish-Skala. Diese Gewichte (`runs/nnue-offline/linear-all.bin`)
  spielt jetzt der Lichess-Bot.
- Folgerung: Die Labels sind der Engpass, nicht das Modell. Daher als Nächstes Datenformat v2 (Suchbewertung als
  Label) – siehe Recherche unten.

**Option für später:** Selbstpartien speichern zusätzlich die eigene Suchbewertung jeder Stellung (Datenformat v2)
und trainieren auf einer Mischung aus Bewertung und Ergebnis – ein Label pro Stellung statt pro Partie, deutlich
dateneffizienter (erlaubt, weil es unsere eigenen Daten sind).

## Ergebnisse

### Lauf 1 (`runs/run1`): 8 Generationen, je 2000 Selbstpartien, 5000 Knoten/Zug, Matches mit 400 Partien

| Gen | CPU kumuliert | Elo gegen Vorgänger | Elo verkettet | gelernte Werte (Bauer = 1) |
|---|---|---|---|---|
| 1 | 9,5 min | +920 ± 197 | +920 | N 1,16 · B 1,42 · R 2,38 · Q 4,37 |
| 2 | 18 min | +167 ± 37 | +1087 | N 1,15 · B 1,40 · R 2,55 · Q 4,20 |
| 3 | 28 min | +272 ± 42 | +1359 | N 1,21 · B 1,45 · R 2,87 · Q 4,97 |
| 4 | 38 min | +173 ± 35 | +1532 | N 1,26 · B 1,50 · R 2,98 · Q 5,13 |
| 5 | 49 min | +130 ± 34 | +1662 | N 1,43 · B 1,70 · R 3,21 · Q 5,55 |
| 6 | 59 min | +70 ± 32 | +1732 | N 1,54 · B 1,85 · R 3,35 · Q 5,91 |
| 7 | 70 min | +55 ± 32 | +1788 | N 1,59 · B 1,88 · R 3,33 · Q 5,99 |
| 8 | 80 min | +57 ± 32 | +1845 ± 217 | N 1,63 · B 1,91 · R 3,34 · Q 6,09 |

- Die Kurve flacht ab: Ab Gen 6 bringt jede Generation nur noch ~+60 Elo. Mit 384 Gewichten
  (Figur × Feld) kann die Bewertung nicht mehr ausdrücken; der nächste Schritt sind neue Merkmale.
- Die Figurenwerte nähern sich ohne jedes Vorwissen dem klassischen 1 : 3 : 3 : 5 : 9 an
  (Reihenfolge stimmt ab Gen 1, Läufer > Springer, Verhältnisse wachsen weiter).
- ~80 % des Aufwands sind Selbstpartien, ~20 % Tuning (Auflösung zu ruhigen Stellungen eingeschlossen).
- Elo gegen Gen 0 sättigt ab Gen 2 bei 399 : 1 (Gen 1: 398 : 2); aussagekräftig ist nur die verkettete Kurve.
  Die verketteten Fehler addieren sich, die absolute Zahl ist daher unsicher (± 217). Besonders der
  erste Schritt (+920) liegt nahe an der Messgrenze von 400 Partien (~1160 Elo) und trägt das Niveau
  der ganzen Kurve; die Zuwächse ab Gen 2 sind belastbarer.
- Gen 1 ist eher untertrainiert: Ihr Tuning endete bei Epoche 400 noch mit sinkender Validierungs-Loss.
- `val_loss` in der CSV ist nicht zwischen Generationen vergleichbar: Die Validierungsmenge wächst mit
  (jede 10. Partie aller bisherigen Daten).
- Ab Gen 7 enden ~6 % der Selbstpartien am Zuglimit: Die Engine kann Gewinnstellungen nicht verwerten.

## Offene Fragen

- Wie stark hilft eine bessere Suche dem Training? (Gleiche Datenmenge, unterschiedliche Suchtiefe in den Selbstpartien)
- Lohnt es sich, die Gewichte der Vorgeneration als Startpunkt zu nehmen, oder jedes Mal neu ab 0?
- Ab welcher Datenmenge schlägt ein kleines NNUE die lineare Bewertung?
- Spielphase: lernbar machen (z. B. zwei Gewichtssätze plus gelernte Phase) statt fester Phasenformel?

## Werkzeuge

- gcc 13 / clang, make: vorhanden
- `cutechess-cli`: muss noch installiert werden (für Matches und SPRT)
- Python 3 + matplotlib: nur für `tools/plot.py`
