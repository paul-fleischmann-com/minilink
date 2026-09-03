# Die Implementierung eines Linkers — Architektur, Datenstrukturen und Arbeitsweise

## Inhaltsverzeichnis

1. Einordnung: Was ein Linker im Build-Prozess leistet
2. Eingabeformate und Objektmodell
3. Die Kernpipeline im Detail
4. Zentrale Datenstrukturen
5. Symbolauflösung im Detail
6. Section-Klassifikation und Layout
7. Placement / Address Assignment
8. Relocation-Anwendung
9. Output-Erzeugung
10. Sonderfälle: Archive, LTO, Multicore/Embedded-Placement
11. Vergleich zweier realer Architekturen: LLVM lld vs. TASKING ltc
12. Ein lauffähiges Minimalmodell in C (Referenzimplementierung)
13. Performance-Überlegungen
14. Glossar

---

## 1. Einordnung: Was ein Linker im Build-Prozess leistet

Der Linker ist die letzte Stufe der klassischen Compiler-Toolchain:

```
Quellcode (.c)
     │  Compiler-Frontend (Parsing, Semantik)
     ▼
Zwischendarstellung (IR)
     │  Compiler-Backend (Codegen, Optimierung)
     ▼
Objektdatei (.o / .obj)  ──┐
Objektdatei (.o / .obj)  ──┼──►  LINKER  ──►  Executable / Library
Bibliothek (.a / .lib)   ──┘
```

Jede Objektdatei ist für sich genommen **unvollständig**: Sie enthält Code und Daten mit relativen, noch nicht endgültigen Adressen, sowie Referenzen auf Symbole (Funktionen, globale Variablen), die in anderen Objektdateien oder Bibliotheken definiert sind. Der Linker hat die Aufgabe, aus dieser Menge unvollständiger Fragmente ein einziges, in sich konsistentes und adressiertes Artefakt zu erzeugen.

Zwei Linker-Familien lassen sich unterscheiden:

- **Klassische Unix-/ELF-Linker** (GNU ld, gold, lld, mold): erzeugen ein Executable oder eine Shared Library für ein Betriebssystem mit virtuellem Speicher. Adressvergabe ist relativ frei (der Loader kann später noch verschieben, ASLR etc.).
- **Locate-Linker für Embedded-Systeme** (TASKING ltc, IAR ILINK, Green Hills, Keil): erzeugen ein Firmware-Image für ein System **ohne** virtuellen Speicher. Jede Adresse ist final und muss exakt in die physische Speicherkarte (Flash-Bänke, verschiedene RAM-Bereiche, Core-lokaler Speicher bei Multicore) passen. Diese Linker brauchen deshalb ein explizites Speichermodell (bei TASKING: LSL) statt eines einfachen linearen Adressraums.

Beide Familien teilen sich dieselbe innere Pipeline; sie unterscheiden sich vor allem in Schritt 6–7 (Placement/Layout).

---

## 2. Eingabeformate und Objektmodell

Ein Linker muss zunächst das Objektdateiformat parsen. Die gängigsten:

| Format | Plattform | Typische Linker |
|---|---|---|
| ELF | Linux, viele Embedded-Targets | GNU ld, gold, lld, mold, TASKING (intern) |
| COFF/PE | Windows | MSVC link.exe, lld-link |
| Mach-O | macOS/iOS | ld64, lld |
| Proprietär/erweitertes ELF | Diverse Embedded-Toolchains | TASKING, IAR, Green Hills |

Unabhängig vom konkreten Byteformat extrahiert der Reader-Teil des Linkers aus jeder Datei drei Kernbestandteile:

1. **Section-Liste** — benannte, zusammenhängende Daten-/Codeblöcke (`.text`, `.data`, `.bss`, `.rodata`, herstellerspezifische Sections)
2. **Symboltabelle** — Namen mit Bindungstyp (lokal/global/schwach), Wert (Offset innerhalb einer Section) und Definitionsstatus
3. **Relocation-Einträge** — pro Section eine Liste von Stellen, die nach der endgültigen Adressvergabe gepatcht werden müssen, inklusive Relocation-Typ (absolut, PC-relativ, GOT-relativ, etc.)

```c
typedef enum { SYM_LOCAL, SYM_GLOBAL, SYM_WEAK } SymbolBinding;

typedef struct {
    char           name[256];
    SymbolBinding  binding;
    uint32_t       section_index;   // welche Section definiert dieses Symbol
    uint64_t       value;           // Offset innerhalb der Section
    int            is_defined;
} ObjSymbol;

typedef struct {
    uint64_t offset;        // Byte-Offset in der Section, der gepatcht wird
    uint32_t symbol_index;  // Referenz in die Symboltabelle
    uint32_t type;          // architekturspezifischer Relocation-Typ
    int64_t  addend;        // zusätzlicher konstanter Offset
} Relocation;

typedef struct {
    char        name[128];       // z.B. ".text", ".bss.private1.mod.var"
    uint8_t    *raw_data;        // NULL bei .bss (nicht initialisiert)
    uint64_t    size;
    uint32_t    alignment;
    uint32_t    flags;           // SHF_ALLOC, SHF_WRITE, SHF_EXECINSTR, ...
    Relocation *relocs;
    uint32_t    reloc_count;
} ObjSection;

typedef struct {
    char        filename[512];
    ObjSection *sections;
    uint32_t    section_count;
    ObjSymbol  *symbols;
    uint32_t    symbol_count;
} ObjectFile;
```

---

## 3. Die Kernpipeline im Detail

```
                    ┌─────────────────────────────────────────────┐
                    │              LINKER DRIVER                  │
                    │  (parst Kommandozeile, orchestriert Phasen) │
                    └───────────────────┬───────────────────────--┘
                                        │
            ┌───────────────┬───────────┼────────────────┬────────────────┐
            ▼               ▼           ▼                ▼                ▼
      [1] File Reader  [2] Layout-  [3] Symbol-    [4] Section-    [5] Placement /
      (.o, .a Archive)  Beschreibung  Resolver       Classifier     Address Assignment
                        (Linkerscript                (select/match  (Locator)
                         oder LSL)                    Patterns)
                                        │
                                        ▼
                                [6] Relocation-
                                    Engine
                                        │
                                        ▼
                                [7] Output-Writer
                                (ELF/HEX/MAP-File)
```

Jede Phase im Detail (mit den Fragen, die sie beantwortet):

| Phase | Kernfrage | Typische Fehlerklasse bei Problemen |
|---|---|---|
| 1. File Reader | Was enthalten die Eingabedateien? | Korrupte/falsche Objektdatei |
| 2. Layout-Beschreibung | Wie sieht der Zielspeicher aus? | Syntaxfehler im Linkerscript/LSL |
| 3. Symbol-Resolver | Wer definiert welches Symbol? | „undefined reference" (z.B. TASKING E121) |
| 4. Section-Classifier | Welche Section gehört in welche Zielgruppe? | Section landet in keiner Gruppe (Warnung) |
| 5. Placement | Wo genau (welche Adresse)? | Speicherüberlauf, Overlap |
| 6. Relocation | Wie werden Referenzen final gepatcht? | Falscher Relocation-Typ, Reichweitenüberschreitung (z.B. Branch zu weit entfernt) |
| 7. Output-Writer | Wie wird das Ergebnis geschrieben? | Formatfehler beim Zielformat |

---

## 4. Zentrale Datenstrukturen

Am Beispiel eines vereinfachten, aber vollständigen Linker-Kerns (angelehnt an die Konzepte aus lld's `NewLLD.rst` und klassischen Locate-Linkern):

```c
/* ---- Symbol-Repräsentation ---- */

typedef enum { SYM_DEFINED, SYM_UNDEFINED, SYM_LAZY } SymbolState;

typedef struct Symbol {
    char          name[256];
    SymbolState   state;
    ObjSection   *owning_section;   // nur gültig wenn DEFINED
    uint64_t      section_offset;
    uint64_t      final_address;    // erst nach Placement gültig
    ObjectFile   *lazy_archive_member; // nur gültig wenn LAZY
} Symbol;

/* Es existiert genau EINE Symbol-Instanz pro eindeutigem Namen.
   Alle Referenzen im Programm zeigen auf denselben Speicherort ->
   sobald der Resolver ihn von UNDEFINED auf DEFINED umstellt,
   "sehen" alle Referenzen automatisch die Auflösung.               */

/* ---- Hashtabelle für globale Symbole ---- */

typedef struct {
    Symbol   **buckets;
    uint32_t   bucket_count;
    uint32_t   symbol_total;
} SymbolTable;

/* ---- Zielspeicher-Modell (Locate-Linker-Variante) ---- */

typedef struct MemoryRegion {
    char      name[64];         // z.B. "PFLASH0", "DSPR1"
    uint64_t  base_address;
    uint64_t  size;
    uint32_t  attributes;       // READ, WRITE, EXEC
    uint64_t  cursor;           // aktuell nächste freie Adresse
} MemoryRegion;

/* ---- Zielgruppe: fasst mehrere InputSections zusammen ---- */

typedef struct OutputGroup {
    char           name[64];
    MemoryRegion  *target_region;
    uint64_t       run_address;     // Laufzeitadresse
    uint64_t       load_address;    // Ladeadresse (kann abweichen: ROM->RAM Copy)
    char         **select_patterns; // Wildcard-Muster, welche InputSections gehören dazu
    uint32_t       pattern_count;
    ObjSection   **members;         // final zugeordnete InputSections
    uint32_t       member_count;
    uint64_t       total_size;
} OutputGroup;
```

Diese vier Strukturen — `Symbol`, `SymbolTable`, `MemoryRegion`, `OutputGroup` — bilden bereits den Kern fast jeder Linker-Implementierung, egal ob GNU ld, lld oder TASKING.

---

## 5. Symbolauflösung im Detail

Der Resolver arbeitet nach einem festen Regelwerk zur Konfliktauflösung, wenn ein Symbolname mehrfach auftaucht:

```
Neuer Fund  \  Bereits im Table   DEFINED        UNDEFINED      LAZY
-------------------------------------------------------------------
DEFINED                            Fehler*        ersetzen       ersetzen +
                                    (Multiple Def) 
UNDEFINED                          behalten        behalten       behalten
LAZY                                behalten        ersetzen +     behalten
                                                    Archiv laden
```
`*` außer bei COMDAT/`weak`-Symbolen, dort gelten Sonderregeln (kleinste/erste Definition gewinnt).

```c
void symtab_add(SymbolTable *tab, Symbol *incoming) {
    Symbol *existing = symtab_lookup(tab, incoming->name);

    if (!existing) {
        symtab_insert(tab, incoming);
        return;
    }

    switch (existing->state) {
        case SYM_UNDEFINED:
            if (incoming->state == SYM_DEFINED || incoming->state == SYM_LAZY) {
                symtab_replace(tab, existing, incoming);
                if (incoming->state == SYM_LAZY)
                    archive_extract_member(incoming->lazy_archive_member);
            }
            break;

        case SYM_LAZY:
            if (incoming->state == SYM_DEFINED)
                symtab_replace(tab, existing, incoming);
            /* UNDEFINED gegen LAZY: LAZY gewinnt, Archiv wird geladen */
            if (incoming->state == SYM_UNDEFINED)
                archive_extract_member(existing->lazy_archive_member);
            break;

        case SYM_DEFINED:
            if (incoming->state == SYM_DEFINED && !is_weak(incoming))
                emit_error("multiple definition of '%s'", incoming->name);
            /* sonst: existing gewinnt, incoming wird verworfen */
            break;
    }
}
```

Nach Durchlauf aller Objektdateien und (transitiv) aller benötigten Archiv-Member prüft der Linker, ob noch `SYM_UNDEFINED`-Einträge übrig sind — falls ja, wird der Link mit einem Fehler abgebrochen ("undefined reference to ...").

**Wichtiges Detail zum Archiv-Handling** (siehe auch lld-Vergleich unten): Ein einfacher, naiver Resolver, der Dateien strikt in Kommandozeilenreihenfolge abarbeitet und nur *undefinierte* Symbole im Blick behält, scheitert an zyklischen Abhängigkeiten zwischen Archiven. Die robuste Lösung: alle Symbole aus Archiven werden sofort als `LAZY` in die Tabelle eingetragen (nicht erst bei Bedarf gesucht), sodass ein späteres `UNDEFINED` sie jederzeit sofort finden und "scharfschalten" kann — unabhängig von der Reihenfolge auf der Kommandozeile.

---

## 6. Section-Klassifikation und Layout

Nachdem alle Symbole aufgelöst sind, muss jede **Section** (nicht Symbol!) einer Zielposition zugeordnet werden. Es gibt zwei grundsätzlich verschiedene Mechanismen:

**A) Implizite Klassifikation** (klassischer Unix-Linker ohne Custom-Script): Sections mit demselben Namen aus verschiedenen Objektdateien werden automatisch zusammengefasst (`.text` aller `.o`-Dateien → eine `.text`-OutputSection), Standard-Layout nach Konvention.

**B) Explizite Klassifikation via Linkerscript/LSL** (GNU-Linkerscript oder TASKING LSL): Der Nutzer (oder das Board-Support-Package) definiert Wildcard-Regeln, die Sections in benannte Gruppen einsortieren:

```
/* GNU-Linkerscript-Stil */
SECTIONS {
  .text : { *(.text .text.*) } > FLASH
  .data : { *(.data .data.*) } > RAM AT> FLASH
}
```

```
/* TASKING-LSL-Stil */
group code_flash (ordered, run_addr = mem:pflash0[0x80000000])
{
    select ".text.*";
    select ".rodata.*";
}
```

Die Implementierung dahinter ist in beiden Fällen ein **Wildcard-Pattern-Matcher**:

```c
int wildcard_match(const char *pattern, const char *name) {
    /* vereinfachtes Glob-Matching: unterstützt '*' als Platzhalter */
    while (*pattern && *name) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return 1;               // Pattern endet mit '*': matcht Rest
            while (*name) {
                if (wildcard_match(pattern, name)) return 1;
                name++;
            }
            return 0;
        }
        if (*pattern != *name) return 0;
        pattern++; name++;
    }
    return (*pattern == '\0' && *name == '\0');
}

OutputGroup *classify_section(ObjSection *sec, OutputGroup *groups, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t p = 0; p < groups[i].pattern_count; p++)
            if (wildcard_match(groups[i].select_patterns[p], sec->name))
                return &groups[i];
    return NULL;  /* -> Warnung: Section wird keiner Gruppe zugeordnet */
}
```

Bei Embedded-Multicore-Systemen (AURIX etc.) trägt bereits der **Compiler** die Core-Zugehörigkeit im Sectionnamen ein (z. B. `.bss.private1.modul.variable`), sodass der Klassifikator ganz normal per Wildcard arbeiten kann, ohne den eigentlichen Code analysieren zu müssen — wichtig, wenn Objektdateien aus Drittanbieter-Bibliotheken ohne Sourcecode stammen.

---

## 7. Placement / Address Assignment

Der Locator ist im Kern ein **First-Fit-Bin-Packing-Algorithmus** pro Speicherregion:

```c
uint64_t align_up(uint64_t value, uint32_t alignment) {
    return (value + alignment - 1) & ~((uint64_t)alignment - 1);
}

int place_group(MemoryRegion *region, OutputGroup *group) {
    uint64_t addr = align_up(region->cursor, group_required_alignment(group));

    /* Reserved-Bereiche berücksichtigen, z.B. fixe Interrupt-Vektortabellen */
    ReservedBlock *blk = find_overlapping_reserved(region, addr, group->total_size);
    while (blk) {
        addr = align_up(blk->end_address, group_required_alignment(group));
        blk = find_overlapping_reserved(region, addr, group->total_size);
    }

    if (addr + group->total_size > region->base_address + region->size) {
        emit_error("memory overflow in region '%s': need %llu bytes, only %llu free",
                   region->name, group->total_size,
                   region->base_address + region->size - addr);
        return -1;
    }

    group->run_address = addr;

    /* Jeder InputSection innerhalb der Gruppe eine konkrete Adresse zuweisen */
    uint64_t member_cursor = addr;
    for (uint32_t i = 0; i < group->member_count; i++) {
        member_cursor = align_up(member_cursor, group->members[i]->alignment);
        group->members[i]->final_run_address = member_cursor;
        member_cursor += group->members[i]->size;
    }

    region->cursor = addr + group->total_size;
    return 0;
}
```

**Load-Address vs. Run-Address:** Für initialisierte Daten (`.data`), die im Flash abgelegt aber im RAM ausgeführt/genutzt werden, unterscheidet der Linker zwei Adressen — die Load-Adresse (wo die Bytes im Flash-Image liegen) und die Run-Adresse (wohin sie zur Laufzeit vom Startup-Code kopiert werden). Der Linker generiert dafür üblicherweise Grenzsymbole (z. B. `_lc_ub_data`/`_lc_ue_data` bei TASKING, `_sidata`/`_sdata`/`_edata` bei GNU-basierten Toolchains), die der Crt0-Startup-Code liest, um den Kopiervorgang durchzuführen:

```c
/* vereinfachter Startup-Code, wie ihn der Linker durch Symbolexport ermöglicht */
extern uint32_t _lc_ub_data, _lc_ue_data, _lc_ul_data;  // vom Linker gesetzt

void copy_initialized_data(void) {
    uint32_t size = (uint32_t)&_lc_ue_data - (uint32_t)&_lc_ub_data;
    memcpy(&_lc_ub_data, &_lc_ul_data, size);
}
```

---

## 8. Relocation-Anwendung

Nachdem jede Section eine finale Adresse hat, iteriert der Linker über alle Relocation-Einträge und patcht den Maschinencode direkt im Speicherabbild:

```c
uint32_t compute_relocated_value(Relocation *r, uint64_t symbol_final_addr,
                                   uint64_t patch_location_addr) {
    switch (r->type) {
        case RELOC_ABS32:
            return (uint32_t)(symbol_final_addr + r->addend);

        case RELOC_PCREL32:
            return (uint32_t)(symbol_final_addr + r->addend - patch_location_addr);

        case RELOC_PCREL18_BRANCH:  /* z.B. TriCore-artiger Kurz-Branch */
        {
            int64_t delta = (int64_t)(symbol_final_addr - patch_location_addr);
            if (delta < -(1 << 17) || delta >= (1 << 17))
                emit_error("branch target out of range (18-bit PC-relative)");
            return encode_branch_immediate(delta);
        }
        default:
            emit_error("unsupported relocation type %u", r->type);
            return 0;
    }
}

void apply_relocations(ObjSection *sec, SymbolTable *resolved_symbols) {
    for (uint32_t i = 0; i < sec->reloc_count; i++) {
        Relocation *r = &sec->relocs[i];
        Symbol *target = symtab_lookup_by_index(resolved_symbols, r->symbol_index);

        if (target->state != SYM_DEFINED)
            emit_error("cannot relocate against unresolved symbol");

        uint64_t patch_addr = sec->final_run_address + r->offset;
        uint32_t value = compute_relocated_value(r, target->final_address, patch_addr);

        memcpy(sec->raw_data + r->offset, &value, sizeof(value));
    }
}
```

Wichtig: Manche Relocation-Typen können **fehlschlagen**, wenn die Zieladresse zu weit entfernt liegt (z. B. ein 18-Bit-PC-relativer Branch bei TriCore kann nicht beliebig weit springen). Das ist eine Klasse von Linker-Fehlern, die architekturspezifisch ist und oft erst nach dem Placement sichtbar wird — in manchen Toolchains löst das sogar eine **Relaxation-Passe** aus (automatisches Umschreiben zu einem längeren Sprungbefehl, falls die Zieldistanz zu groß ist).

---

## 9. Output-Erzeugung

Der Writer erzeugt aus den fertig platzierten und relozierten Sections das Zielformat. Typische Aufgaben:

- Header schreiben (ELF-Header, Program-Header/Segmente, Section-Header-Tabelle)
- Für Firmware-Toolchains zusätzlich: Konvertierung in Intel-HEX oder S-Record für Flash-Programmierer
- **MAP-File** erzeugen: menschenlesbare Cross-Referenz von jedem Symbol/jeder Section zu ihrer finalen Adresse — unverzichtbar fürs Debugging und für die Stack-/Speicherbudget-Analyse
- Debug-Informationen (DWARF) anpassen, falls vorhanden — Adressreferenzen in den Debug-Sections müssen ebenfalls aktualisiert werden

```c
void write_map_file(FILE *out, OutputGroup *groups, uint32_t group_count) {
    fprintf(out, "%-32s %-12s %-10s %s\n", "Symbol/Section", "Address", "Size", "Region");
    for (uint32_t i = 0; i < group_count; i++) {
        fprintf(out, "%-32s 0x%08llx %-10llu %s\n",
                groups[i].name, groups[i].run_address,
                groups[i].total_size, groups[i].target_region->name);
        for (uint32_t m = 0; m < groups[i].member_count; m++)
            fprintf(out, "  %-30s 0x%08llx %-10llu\n",
                    groups[i].members[m]->name,
                    groups[i].members[m]->final_run_address,
                    groups[i].members[m]->size);
    }
}
```

---

## 10. Sonderfälle

### Archive-Dateien (.a / .lib)

Ein Archiv ist im Kern nur eine Sammlung von Objektdateien plus einem Index (Symbolname → welches Member definiert es). Der Linker liest zunächst nur den Index; einzelne Member werden erst extrahiert und vollständig gelesen, wenn der Resolver ein passendes Symbol tatsächlich braucht (siehe Abschnitt 5, Lazy-Symbol-Mechanismus).

### Link-Time Optimization (LTO)

Statt fertigem Maschinencode enthalten die Objektdateien eine Zwischendarstellung (z. B. LLVM Bitcode). Der Resolver funktioniert identisch (Symbolauflösung auf Bitcode-Ebene), aber bevor Placement/Relocation stattfindet, wird eine zusätzliche Codegen-Phase eingeschoben, die alle Bitcode-Module gemeinsam zu echtem Maschinencode kompiliert — das ermöglicht Optimierungen über Objektdateigrenzen hinweg (Inlining zwischen Übersetzungseinheiten etc.).

### Multicore-Placement (Embedded)

Bei Systemen wie AURIX TriCore mit mehreren CPU-Kernen und jeweils eigenem lokalem Speicher (Scratchpad-RAM) muss der Linker:

1. Pro Core einen eigenen Adressraum/eigene MemoryRegion-Menge verwalten
2. Sections anhand ihrer Core-Zugehörigkeit (im Namen kodiert oder über Metadaten) in die richtige Region einsortieren
3. Für gemeinsam genutzten Speicher (Shared RAM) eigene Synchronisationsregeln beachten (z. B. Cache-Kohärenz-Attribute)
4. Meist wird pro Core ein eigenes Ausgabe-Image erzeugt, mit einem gemeinsamen Architekturmodell als Basis

---

## 11. Vergleich zweier realer Architekturen

| Aspekt | LLVM lld (ELF) | TASKING ltc (TriCore/AURIX) |
|---|---|---|
| Quellcode | Offen (github.com/llvm/llvm-project/tree/main/lld) | Closed-Source |
| Layout-Beschreibung | GNU-Linkerscript-kompatible Syntax | Eigene DSL: LSL (Linker Script Language) |
| Zieldomäne | Betriebssysteme mit virtuellem Speicher | Bare-Metal Firmware, kein virtueller Speicher |
| Adressvergabe | Meist relativ frei, ASLR-fähig | Vollständig statisch, jede Adresse final |
| Multicore | Kein eingebautes Konzept | Zentrales Konzept: Core-Association pro Section |
| Archiv-Strategie | „Alle Symbole merken", sofortige Extraktion bei Bedarf | Ähnliches Prinzip, herstellerspezifisch implementiert |
| Performance-Fokus | Extrem groß (Chrome-Link in 15s bei 2GB Output) | Kleinere Programme (Firmware im MB-Bereich), Fokus eher auf Korrektheit der Speicherplatzierung |
| Fehlerklassen | Multiple Definition, Undefined Reference, Segment Overlap | Zusätzlich: LSL-Syntaxfehler, Core-Zuordnungsfehler (F014), Speicherüberlauf pro Core-Region |

Strukturell sind beide Linker nahezu identisch aufgebaut (siehe Abschnitt 3), der Hauptunterschied liegt im Layout-Modell (Schritt 2/4/5): lld nutzt ein vergleichsweise einfaches, lineares Speichermodell mit optionalem Script; TASKING braucht wegen der Multicore-/Bare-Metal-Anforderungen ein deutlich reichhaltigeres, hierarchisches Architekturmodell (Bus → Core → Memory-Space).

---

## 12. Ein lauffähiges Minimalmodell in C

Das folgende Snippet zeigt den kompletten Ablauf an einem winzigen, aber vollständigen Beispiel (zwei fiktive Objektdateien, ein Ziel-Speicherbereich) — keine Produktionsqualität, aber jeder Schritt aus Abschnitt 3–8 ist enthalten:

```c
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ---- Minimalversion der Strukturen aus Abschnitt 4 ---- */
typedef struct { char name[32]; int defined; uint64_t addr; } Sym;
typedef struct { char name[32]; uint64_t size; uint64_t addr; } Sec;

int main(void) {
    /* Schritt 1: "eingelesene" Objektdateien (hartkodiert fürs Beispiel) */
    Sym symbols[] = {
        { "main",       1, 0 },   /* definiert in obj1 */
        { "helper_fn",  1, 0 },   /* definiert in obj2 */
        { "g_counter",  1, 0 },   /* definiert in obj2, .bss */
    };
    Sec sections[] = {
        { ".text.obj1", 64,  0 },
        { ".text.obj2", 32,  0 },
        { ".bss.obj2",  4,   0 },
    };

    /* Schritt 5: Placement -- simples First-Fit ab Basisadresse */
    uint64_t flash_cursor = 0x80000000;  /* Zielregion: FLASH */
    uint64_t ram_cursor   = 0x60000000;  /* Zielregion: RAM   */

    for (int i = 0; i < 2; i++) {        /* .text.* -> FLASH */
        sections[i].addr = flash_cursor;
        flash_cursor += sections[i].size;
    }
    sections[2].addr = ram_cursor;       /* .bss.* -> RAM */
    ram_cursor += sections[2].size;

    /* Symbole an ihre jeweilige Section-Adresse binden (vereinfachte 1:1-Zuordnung) */
    symbols[0].addr = sections[0].addr;
    symbols[1].addr = sections[1].addr;
    symbols[2].addr = sections[2].addr;

    /* Schritt 9: Output / MAP-File */
    printf("%-12s %-10s\n", "Symbol", "Adresse");
    for (int i = 0; i < 3; i++)
        printf("%-12s 0x%08llx\n", symbols[i].name,
               (unsigned long long)symbols[i].addr);

    return 0;
}
```

Ausgabe:
```
Symbol       Adresse
main         0x80000000
helper_fn    0x80000040
g_counter    0x60000000
```

Dieses Minimalbeispiel lässt bewusst Symbolkonfliktauflösung, Relocation-Patching und Wildcard-Klassifikation weg, um den reinen Placement-Kern sichtbar zu machen. Alle fehlenden Teile sind in den Code-Beispielen der Abschnitte 5–8 vollständig ausgearbeitet und lassen sich direkt einsetzen.

---

## 13. Performance-Überlegungen

Bei sehr großen Links (Beispiel Chrome: 17.000 Dateien, 6,3 Mio. Symbole, 13 Mio. Relocations, 2 GB Output in 15 Sekunden mit lld) entscheiden folgende Designprinzipien über die Geschwindigkeit:

- **Lazy I/O**: Section-Inhalte erst lesen, wenn sie wirklich gebraucht werden (nicht beim Einlesen der Objektdatei)
- **Ein Hash-Lookup pro Symbol, nicht mehrere**: Jede zusätzliche Hashtable-Operation pro Symbol skaliert linear mit der Symbolzahl — bei Millionen Symbolen macht das den Unterschied zwischen Sekunden und Minuten
- **mmap statt Lesen in Puffer**: Objektdateien werden direkt in den Adressraum gemapped statt in Heap-Puffer kopiert
- **Parallelisierung**: Reading und Relocation-Anwendung lassen sich pro Datei bzw. pro Section parallelisieren, da sie größtenteils unabhängig sind (Ausnahme: der globale Symboltabellen-Zugriff braucht Synchronisation)

Bei Embedded-Linkern (kleinere Programme, oft < 10 MB Output) spielt Performance eine untergeordnete Rolle gegenüber **Korrektheit der Speicherplatzierung** — ein einziges falsch platziertes Byte in einer Multicore-Firmware kann zu einem Hard Fault führen, der sich nur schwer debuggen lässt.

---

## 14. Glossar

- **Symbol**: Benannte Referenz auf eine Adresse (Funktion oder Variable)
- **Section**: Zusammenhängender Block von Code oder Daten mit gemeinsamen Eigenschaften (z. B. `.text` = ausführbarer Code)
- **Relocation**: Anweisung, wie eine Adressreferenz nach der finalen Platzierung gepatcht werden muss
- **Locate/Placement**: Vergabe konkreter finaler Adressen an Sections
- **Load-Address vs. Run-Address**: Unterschied zwischen Speicherort im Firmware-Image (z. B. Flash) und Ort der tatsächlichen Nutzung zur Laufzeit (z. B. RAM)
- **Archiv/Lazy-Symbol**: Symbol, das erst bei tatsächlichem Bedarf aus einer Bibliothek geladen wird
- **LSL (Linker Script Language)**: TASKING-eigene DSL zur Beschreibung von Speicherarchitektur und Section-Layout
- **Linkerscript**: GNU-Pendant zu LSL, einfacheres, lineares Modell
- **MAP-File**: Menschenlesbare Ausgabe mit finalen Adressen aller Symbole/Sections
- **COMDAT/Weak Symbol**: Symbol, das in mehreren Objektdateien identisch vorkommen darf, ohne einen "multiple definition"-Fehler auszulösen
- **ICF (Identical Code/COMDAT Folding)**: Optimierung, die inhaltsgleiche Sections zusammenlegt, um Ausgabegröße zu reduzieren
