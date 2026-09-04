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
- Statisches Speicher-Placement: mit `-T` zwei Segmente (R-X für Code,
  RW für Daten); mit `--lsl` **ein `PT_LOAD` je genutzter `memory`-
  Region** (also z. B. `.data` und `.bss` an getrennten RAM-Adressen).
  Korrekte `.bss`-Behandlung (kein Datei-Inhalt, aber virtueller
  Speicher via `p_memsz > p_filesz`)
- Relocation-Typen: `R_X86_64_PC32`, `R_X86_64_PLT32` (PC-relative
  Aufrufe/Zugriffe), `R_X86_64_64`, `R_X86_64_32`/`R_X86_64_32S`
  (absolute Referenzen)
- Schreiben eines gültigen, direkt vom Linux-Kernel ladbaren
  ELF64-Executables (`ET_EXEC`, ein bis N `PT_LOAD`-Segmente)
- MAP-Ausgabe auf stdout und nach `minilink.map`: Script/Entry/Layout,
  Memory-Regionen, PT_LOAD-Segmente, Sektionen und Symbole (nach Adresse)
- Minimaler „Debug behalten"-Modus (`--debug` / `-g`): `.debug_*`-Sections
  über alle Eingabedateien konkateniert, ihre Relocations (auch die
  DWARF-internen Section-zu-Section-Verweise) angewandt, eine echte
  Section-Header-Tabelle plus rekonstruierte `.symtab` / `.strtab` /
  `.shstrtab` geschrieben. Ergebnis: `addr2line`, `nm` und
  `readelf --debug-dump` funktionieren, `gdb` bekommt Zeilennummern.

## Was bewusst fehlt (siehe Dokument, Abschnitt 10/13)

- Kein dynamisches Linken (nur statische Executables)
- Keine Archiv-Unterstützung (`.a`) → keine Lazy-Symbole
- Zwei rudimentäre Linkerscript-Formate, **eines davon Pflicht** (kein
  eingebautes Default-Layout, ohne Script bricht `minilink` ab):
  - `-T <datei>` — Miniscript mit `#define BASE_ADDR` / `#define PAGE_SIZE`
    (nur diese zwei Werte, keine Section-Platzierung). Beispiel:
    `test/default.ldl`.
  - `--lsl <datei>` — stark vereinfachtes **TASKING-LSL**: beliebig viele
    `memory {}`-Regionen (`type = rom|ram`, Adresse aus
    `map (dest_offset = …)`) und `section_layout` /
    `group (… run_addr = mem:<id> …)` / `select "<pattern>"` für
    Reihenfolge und Region-Zuordnung der Output-Sections. **Jede genutzte
    Region wird ein eigenes `PT_LOAD`** — `.data` nach `mem:ram`, `.bss`
    nach `mem:ram2` usw. landen an getrennten Adressen. Alles Übrige
    (`architecture`, `bus`, `derivative`, `section_setup`, …) wird
    überlesen. Beispiel: `test/tc27x.lsl` (rom / ram / ram2).
  Ein echtes LSL/GNU-ld-Script kann noch viel mehr (Copy-Tables,
  overlays, `align`/`fill`, mehrere Cores, …).
- Keine Section-Garbage-Collection, kein ICF, kein LTO
- Nur eine Handvoll Relocation-Typen (production-linker unterstützen
  dutzende, architekturabhängig)
- `--debug` ist bewusst minimal: `.debug_*` werden nur konkateniert (kein
  String-Merging in `.debug_str`, keine `.debug_line`-Deduplizierung,
  keine `DW_AT_ranges`-Fixups), `.eh_frame`/`.eh_frame_hdr` fehlen
  (Stack-Unwinding daher eingeschränkt), und `--debug` verlangt mit `-g`
  übersetzte Objektdateien

Das sind exakt die Punkte, die einen produktiven Linker (lld: ~100k
Zeilen, TASKING: proprietär, vermutlich ähnliche Größenordnung) vom
hier gezeigten Kern unterscheiden — der Kern-Algorithmus selbst ist
aber identisch.

## Build & Test

Am einfachsten: `./build_and_test.sh` baut minilink und linkt das
Testprogramm in **drei Varianten** (je eigener Ordner unter `test/`),
danach werden alle drei ausgeführt und geprüft:

| Ordner       | Aufruf                                | Besonderheit                         |
|--------------|--------------------------------------|-------------------------------------|
| `test/none/` | `-T test/default.ldl`                | Standard, kein Debug                 |
| `test/lsl/`  | `--lsl test/tc27x.lsl`               | ein `PT_LOAD` je genutzter Region   |
| `test/g/`    | `-T test/default.ldl --debug` (`-g`) | Debug-Info behalten (DWARF, symtab) |

Die `.o`-Dateien liegen direkt im Varianten-Ordner, das fertige
Executable unter `test/<variante>/out/program`.

Von Hand entspricht das:

```bash
# minilink selbst bauen
gcc -O0 -g -Wall -o build/minilink src/minilink.c

# Objektdateien je Variante (test/none nur exemplarisch gezeigt)
CF="-ffreestanding -fno-pie -fno-stack-protector -O0"
mkdir -p test/none/out
gcc -c $CF    -o test/none/main.o test/main.c
gcc -c $CF    -o test/none/msg.o  test/msg.c

# Linken mit unserem eigenen Linker (nicht mit ld!) -- genau EIN Script Pflicht
./build/minilink -T test/default.ldl test/none/main.o test/none/msg.o -o test/none/out/program

# Ausführen — läuft nativ unter Linux, kein Interpreter/keine Sandbox nötig
./test/none/out/program
```

Alle drei Varianten liefern dieselbe Ausgabe und Exit-Code `2` (beweist,
dass `g_call_count` — in `msg.c` definiert, in `main.c` gelesen — über
beide Objektdateien auf dieselbe finale Adresse zeigt). `--lsl` legt
`.data`/`.bss` nur an andere (RAM-)Adressen, im Beispiel `test/tc27x.lsl`
sogar in getrennte `PT_LOAD` (`mem:ram` @ `0x800000`, `mem:ram2` @
`0xc00000`). Bei `test/g/` funktionieren zusätzlich:

```bash
addr2line -e test/g/out/program -f 0x40101d   # -> _start / test/main.c:18
readelf -S test/g/out/program | grep debug    # .debug_info, .debug_line, ...
```

## Projektstruktur

```
minilink/
├── src/minilink.c        Der Linker selbst (eine Datei)
├── build_and_test.sh     Baut + linkt (none / lsl / g) + prüft
├── build/minilink         Kompilierter Linker (nach Build)
├── docs/elf-aufbau.*      ELF64-Aufbau als Referenz (adoc / puml / svg)
└── test/
    ├── main.c             Testprogramm Teil 1 (_start, Aufrufer)
    ├── msg.c              Testprogramm Teil 2 (Definitionen, Syscalls)
    ├── default.ldl        Minimales -T-Script (BASE_ADDR, PAGE_SIZE)
    ├── tc27x.lsl          Vereinfachtes TASKING-LSL für --lsl
    ├── none/  main.o msg.o  out/program     (-T)
    ├── lsl/   main.o msg.o  out/program     (--lsl)
    └── g/     main.o msg.o  out/program     (-T --debug, mit -g)
```
