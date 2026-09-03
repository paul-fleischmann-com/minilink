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
- Kein Custom-Linkerscript/LSL-Äquivalent (Layout ist fest verdrahtet)
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
./build/minilink test/main.o test/msg.o -o test/program

# Ausführen — läuft nativ unter Linux, kein Interpreter/keine Sandbox nötig
./test/program
```

Erwartete Ausgabe:
```
Hello from mini-linker!
Hello from mini-linker!
```
Exit-Code `2` (beweist, dass `g_call_count` — definiert in `msg.o`,
inkrementiert bei jedem Aufruf, gelesen in `main.o` — über beide
Objektdateien hinweg auf dieselbe finale Adresse zeigt).

## Projektstruktur

```
minilink/
├── src/minilink.c      Der Linker selbst (~500 Zeilen, eine Datei)
├── test/main.c          Testprogramm Teil 1 (_start, Aufrufer)
├── test/msg.c            Testprogramm Teil 2 (Definitionen, Syscalls)
├── build/minilink        Kompilierter Linker (nach Build)
└── test/program           Vom eigenen Linker erzeugtes Executable
```
