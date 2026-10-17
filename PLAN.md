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
          features.[ch]  Stellung → Merkmalsindizes (EIN Code für Engine und Tuner)
          eval.[ch]      lineare Bewertung, lädt Gewichtsdatei
          search.[ch]    Negamax/Alpha-Beta, Quiescence, TT, Zugsortierung
          datafmt.[ch]   gepacktes Stellungsformat für Trainingsdaten
engine/   main.c, uci.c, selfplay.c         → Binary `engine`
tuner/    main.c, texel.c                   → Binary `tuner`
tests/    perft.c (Perft-Suite)             → `make test`
tools/    plot.py (Python, nur Auswertung)
weights/  Gewichtsdateien pro Generation
data/     Trainingsdaten (nicht in git)
log/      generations.csv
```

### Build

- C11, gcc oder clang, einfaches `Makefile`
- `make` baut mit `-O2 -march=native`
- `make debug` baut mit `-O0 -g -fsanitize=address,undefined`; Debug-Builds prüfen
  zusätzlich an jedem Perft-Knoten, ob Bitboards, Mailbox und Hash zusammenpassen
- `make test` führt die Perft-Suite aus (bis 25 Mio. Knoten, ~2 s);
  `make test-full` alles (~1,5 Mrd. Knoten, ~40 s); `make test-debug` die kleine Suite mit Sanitizern
- `build/release/perft divide <tiefe> <fen|startpos>` zum Eingrenzen von Perft-Fehlern
- Ausgaben landen in `build/`

## Design-Entscheidungen

### Board
- 12 Bitboards (6 Figurentypen × 2 Farben) plus Belegungs-Bitboards
- Seite am Zug, Rochaderechte (4 Bits), en-passant-Feld, Halbzugzähler, Zobrist-Hash
- **Copy-Make**: Ein Zug erzeugt eine Kopie des Boards (~100 Bytes). Das ist einfacher als Make/Unmake und kaum langsamer.

### Zuggenerierung
- Pseudo-legale Züge erzeugen, nach dem Ausführen prüfen, ob der eigene König angegriffen ist
- Zugliste als festes Array `Move moves[256]`, kein `malloc`
- Gleitende Figuren erst mit einfachen Strahl-Schleifen; Magic Bitboards erst, wenn Perft stimmt und Tempo zählt

### Bewertung (linear, ohne Vorwissen)
- Merkmale: **Figurentyp × Feld**, aus Sicht der Seite am Zug, für Schwarz gespiegelt
- 6 × 64 = **384 Gewichte**; eigene Figuren zählen +1, gegnerische −1 (gespiegelt).
  Die Farbsymmetrie ist eine Regel des Spiels, kein Schachwissen.
- `eval = Σ Gewicht[Merkmal]` über alle aktiven Merkmale (max. 32 pro Stellung)
- Figurenwerte entstehen implizit als Mittelwert über die Felder
- Bewusst **nicht** dabei: Mobilität, Königssicherheit, Bauernstruktur, Spielphase (tapered eval).
  Das kann später als zusätzliches Merkmal kommen, Werte immer gelernt.
- Gewichtsdatei: Header (Magic, Version, Anzahl Merkmale) + float32-Gewichte; die Engine rundet auf int32

### Suche
- Negamax mit Alpha-Beta, Iterative Deepening
- Quiescence Search (nur Schlagzüge und Umwandlungen)
- Transposition Table
- Zugsortierung: TT-Zug, MVV-LVA, Killer-Züge, History-Heuristik
- Später: Null-Move-Pruning, Late Move Reductions, Aspiration Windows
- Remis-Erkennung: Patt, dreifache Wiederholung, 50-Züge-Regel, ungenügendes Material

### Selbstpartien
- Laufen direkt in der Engine (`engine selfplay ...`), mehrere Threads, ohne UCI-Overhead
- **Feste Knotenzahl pro Zug** statt Zeit, damit der Aufwand reproduzierbar und vergleichbar ist
- Eröffnungsvielfalt: die ersten 8 Halbzüge zufällig (gleichverteilt über legale Züge)
- Kleines Rauschen bei gleichen Bewertungen, sonst spielt Gen 0 immer dieselbe Partie
- Partieende nur nach Regeln: Matt, Patt, Wiederholung, 50 Züge, ungenügendes Material,
  plus festes Zuglimit (z. B. 400 Halbzüge → Remis). **Nie** nach Material abbrechen, das wäre Vorwissen.
- Gespeichert wird jede Stellung nach der Eröffnungsphase mit dem Endergebnis der Partie

### Trainingsdaten-Format
- Gepackte Stellung: Belegungs-Bitboard (8 Bytes) + 32 Nibbles für Figuren (16 Bytes)
  + Seite am Zug, Rochade, en passant, Ergebnis (aus Sicht der Seite am Zug), Halbzug
- ca. 32 Bytes pro Stellung → 10 Mio. Stellungen ≈ 320 MB

### Tuner (Texel-Tuning)
- Liest gepackte Stellungen, löst sie per Quiescence Search mit den aktuellen Gewichten zu ruhigen Stellungen auf
- Merkmale über dasselbe `features`-Modul wie die Engine
- Verlust: `(Ergebnis − sigmoid(eval / K))²` mit festem K; Gewichte lernen die Skala
- Optimierer: Adam, mehrere Threads, sparse Gradienten (nur aktive Merkmale)
- 90/10-Aufteilung in Training und Validierung, um Overfitting zu erkennen
- Optional: Gewichte der Vorgeneration als Startpunkt statt 0

### Messung
- **Trainingsaufwand** = kumulierte CPU-Sekunden für Selbstpartien + Tuning
  (zusätzlich Anzahl Partien und Stellungen). Testmatches zählen nicht dazu.
- **Elo pro Generation**: festes Match (z. B. 1000 Partien) gegen die Vorgeneration und gegen Gen 0
  mit `cutechess-cli`, inkl. Fehlerbalken
- **Code-Änderungen** (Suche etc.) per SPRT gegen die vorige Version
- `log/generations.csv`: `gen, games, positions, selfplay_cpu_s, tune_cpu_s, cum_cpu_s, elo_vs_prev, elo_vs_gen0, err, notes`
- Absoluter Anker: am Ende Matches gegen Engines mit bekannter CCRL-Elo (nur Messung)

## Meilensteine

| # | Inhalt | Fertig, wenn … |
|---|---|---|
| 1 | Board, FEN, Zuggenerator, Perft | alle Standard-Perft-Werte stimmen (Startstellung, Kiwipete, Positionen 3–6), ohne Sanitizer-Fehler |
| 2 | Suche + UCI, alle Gewichte = 0 | Gen 0 spielt in einer GUI/cutechess legale Partien und findet Matt in 3 |
| 3 | Lineare Bewertung, Gewichtsdatei, Selbstpartien, Datenformat | `engine selfplay` erzeugt Daten und misst die CPU-Zeit |
| 4 | Tuner + Generationsschleife + Match-Messung | erste Kurve mit ≥ 5 Generationen; Gen N schlägt Gen 0 messbar |
| 5 | Verbesserungen | bessere Suche (billigeres Training), weitere Merkmale, später kleines NNUE aus eigenen Daten |

## Offene Fragen

- Wie stark hilft eine bessere Suche dem Training? (Gleiche Datenmenge, unterschiedliche Suchtiefe in den Selbstpartien)
- Lohnt es sich, die Gewichte der Vorgeneration als Startpunkt zu nehmen, oder jedes Mal neu ab 0?
- Ab welcher Datenmenge schlägt ein kleines NNUE die lineare Bewertung?
- Spielphase: lernbar machen (z. B. zwei Gewichtssätze plus gelernte Phase) statt fester Phasenformel?

## Werkzeuge

- gcc 13 / clang, make: vorhanden
- `cutechess-cli`: muss noch installiert werden (für Matches und SPRT)
- Python 3 + matplotlib: nur für `tools/plot.py`
