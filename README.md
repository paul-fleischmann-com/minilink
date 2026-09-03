# minilink — ein echter, minimaler ELF64-Linker

Ein von Grund auf neu geschriebener Linker in C, der reale, mit `gcc`
kompilierte ELF64-Objektdateien (`.o`) einliest, Symbole über Dateigrenzen
hinweg auflöst, Relocations anwendet und ein **lauffähiges** statisches
ELF64-Executable für x86-64 Linux erzeugt — ohne `ld`, ohne libc, ohne
externe Linker-Bibliothek.

## Warum dieses Projekt

Entstanden als konkreter Kompetenznachweis für Compiler-/Linker-nahe
Softwareentwicklung, aufbauend auf einem separaten Architekturdokument
(`../linker-implementierung.md`), das die Konzepte dahinter (Symbol
Resolution, Section Placement, Relocation-Typen, Load- vs. Run-Address)
im Detail erklärt und mit den realen Architekturen von LLVM `lld` und
dem TASKING-Linker (`ltc`) vergleicht.

## Was funktioniert

- Parsen echter ELF64-`ET_REL`-Objektdateien (Header, Section-Header,
  Symboltabelle, `.rela`-Relocation-Sections) über die Standard-`elf.h`
- Globale Symbolauflösung über mehrere Objektdateien hinweg, inklusive
  Erkennung von "multiple definition" und "undefined reference"
- Korrekte Behandlung von SECTION-Symbolen (für section-relative
  Relocations wie `.rodata`-Zugriffe auf statische Konstanten)
- Section-Merging nach Typ (`.text`, `.rodata`, `.data`, `.bss`) über
  alle Eingabedateien
- Statisches Speicher-Placement mit zwei Segmenten (R-X für Code,
  RW für Daten), inklusive korrekter `.bss`-Behandlung (kein
  Datei-Inhalt, aber virtueller Speicher via `p_memsz > p_filesz`)
- Relocation-Typen: `R_X86_64_PC32`, `R_X86_64_PLT32` (PC-relative
  Aufrufe/Zugriffe), `R_X86_64_64`, `R_X86_64_32`/`R_X86_64_32S`
  (absolute Referenzen)
- Schreiben eines gültigen, direkt vom Linux-Kernel ladbaren
  ELF64-Executables (`ET_EXEC`, zwei `PT_LOAD`-Segmente)
- MAP-Datei-artige Ausgabe (Symbol → finale Adresse)

## Was bewusst fehlt (siehe Dokument, Abschnitt 10/13)

- Kein dynamisches Linken (nur statische Executables)
- Keine Archiv-Unterstützung (`.a`) → keine Lazy-Symbole
- Zwei rudimentäre Linkerscript-Formate, **eines davon Pflicht** (kein
  eingebautes Default-Layout, ohne Script bricht `minilink` ab):
  - `-T <datei>` — Miniscript mit `#define BASE_ADDR` / `#define PAGE_SIZE`
    (nur diese zwei Werte, keine Section-Platzierung). Beispiel:
    `test/default.ldl`.
  - `--lsl <datei>` — stark vereinfachtes **TASKING-LSL**: `memory {}`-
    Regionen (`type = rom|ram`, Adresse aus `map (dest_offset = …)`) und
    `section_layout` / `group (… run_addr = mem:<id> …)` / `select
    "<pattern>"` für Reihenfolge und Segment-Zuordnung der Output-Sections.
    Damit landen z. B. `.data`/`.bss` an der RAM-Adresse aus dem Script,
    getrennt vom Code. Alles Übrige (`architecture`, `bus`, `derivative`,
    `section_setup`, …) wird überlesen. Beispiel: `test/tc27x.lsl`.
  Ein echtes LSL/GNU-ld-Script kann noch viel mehr (Copy-Tables,
  overlays, `align`/`fill`, mehrere Cores, …).
- Keine Section-Garbage-Collection, kein ICF, kein LTO
- Nur eine Handvoll Relocation-Typen (production-linker unterstützen
  dutzende, architekturabhängig)

Das sind exakt die Punkte, die einen produktiven Linker (lld: ~100k
Zeilen, TASKING: proprietär, vermutlich ähnliche Größenordnung) vom
hier gezeigten Kern unterscheiden — der Kern-Algorithmus selbst ist
aber identisch.

## Build & Test

```bash
# Testprogramm bauen (zwei .o-Dateien, um Symbolauflösung über
# Dateigrenzen hinweg zu testen)
gcc -c -ffreestanding -fno-pie -fno-stack-protector -O0 -o test/main.o test/main.c
gcc -c -ffreestanding -fno-pie -fno-stack-protector -O0 -o test/msg.o  test/msg.c

# minilink selbst bauen
gcc -O0 -g -Wall -o build/minilink src/minilink.c

# Linken mit unserem eigenen Linker (nicht mit ld!)
# Genau EIN Script ist Pflicht -- entweder -T oder --lsl:
./build/minilink -T   test/default.ldl test/main.o test/msg.o -o test/program
./build/minilink --lsl test/tc27x.lsl  test/main.o test/msg.o -o test/program_lsl

# Ausführen — läuft nativ unter Linux, kein Interpreter/keine Sandbox nötig
./test/program
./test/program_lsl
```

Beide Varianten müssen dieselbe Ausgabe und denselben Exit-Code liefern;
`--lsl` platziert `.data`/`.bss` nur an einer anderen (RAM-)Adresse.

Exit-Code `2` (beweist, dass `g_call_count` — definiert in `msg.o`,
inkrementiert bei jedem Aufruf, gelesen in `main.o` — über beide
Objektdateien hinweg auf dieselbe finale Adresse zeigt).

Am schnellsten: `./build_and_test.sh` baut, linkt beide Varianten und
prüft Ausgabe + Exit-Code.

## Projektstruktur

```
minilink/
├── src/minilink.c      Der Linker selbst (eine Datei)
├── test/main.c          Testprogramm Teil 1 (_start, Aufrufer)
├── test/msg.c            Testprogramm Teil 2 (Definitionen, Syscalls)
├── test/default.ldl     Minimales -T-Script (BASE_ADDR, PAGE_SIZE)
├── test/tc27x.lsl       Vereinfachtes TASKING-LSL für --lsl
├── build_and_test.sh    Baut + linkt (beide Script-Varianten) + prüft
├── docs/elf-aufbau.*    ELF64-Aufbau als Referenz (adoc / puml / svg)
├── build/minilink        Kompilierter Linker (nach Build)
└── test/program[_lsl]     Vom eigenen Linker erzeugte Executables
```
