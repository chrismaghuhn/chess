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
tools/    generations.sh (Trainingsschleife), plot.py (SVG-Kurve, ohne Zusatzpakete)
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
| 5 | Verbesserungen | bessere Suche (billigeres Training), weitere Merkmale, später kleines NNUE aus eigenen Daten |

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
- Elo gegen Gen 0 sättigt ab Gen 2 bei 400 : 0; aussagekräftig ist nur die verkettete Kurve.
  Die verketteten Fehler addieren sich, die absolute Zahl ist daher unsicher (± 217).
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
