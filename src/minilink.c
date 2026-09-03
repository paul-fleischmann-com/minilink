/*
 * minilink — ein echter, minimaler ELF64-Linker für x86-64 Linux.
 *
 * Implementiert die vollstaendige Pipeline eines Locate-Linkers:
 *   [1] Object-File-Reader   (ELF64 .o parsen)
 *   [2] Symbol-Resolver      (globale Symboltabelle ueber alle Eingabedateien)
 *   [3] Section-Merger       (.text/.data/.bss/.rodata ueber alle Dateien zusammenfassen)
 *   [4] Placement/Locator    (finale Adressen vergeben, statisches Layout)
 *   [5] Relocation-Engine    (R_X86_64_PC32, R_X86_64_PLT32, R_X86_64_64, R_X86_64_32S)
 *   [6] Output-Writer        (lauffaehiges statisches ELF64-Executable)
 *
 * Bewusste Vereinfachungen gegenueber einem produktiven Linker:
 *   - kein dynamisches Linken (nur statische Executables)
 *   - keine Archiv-Unterstuetzung (.a) -> keine Lazy-Symbole
 *   - nur eine Handvoll Relocation-Typen
 *   - keine Section-Garbage-Collection, kein ICF, kein LTO
 *   - .eh_frame und Debug-Sections werden ignoriert (nicht fuer Ausfuehrung noetig)
 *
 * Genau diese Vereinfachungen sind es, die aus einem produktiven Linker
 * (lld, TASKING ltc) ein Vielfaches an Code machen -- der Kern-Algorithmus
 * ist aber exakt derselbe.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <elf.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#define MAX_INPUT_FILES   64
#define MAX_SECTIONS      256
#define MAX_SYMBOLS       4096
#define MAX_RELOCS        4096

/* ---------------------------------------------------------------------
 * [1] Object-File-Reader: interne Repraesentation
 * ------------------------------------------------------------------- */

typedef enum { KIND_TEXT, KIND_RODATA, KIND_DATA, KIND_BSS, KIND_IGNORED } SectionKind;

typedef struct {
    char        name[64];
    SectionKind kind;
    uint8_t    *data;          /* NULL bei .bss */
    uint64_t    size;
    uint64_t    align;
    int         file_index;    /* aus welcher Eingabedatei */
    int         orig_shndx;    /* urspruenglicher Section-Index in dieser Datei */
    int         segment;       /* 0 = R-X-PT_LOAD, 1 = RW-PT_LOAD (nach Placement) */
    uint64_t    merged_offset; /* Offset ab Anfang des enthaltenden PT_LOAD-Segments */
} InSection;

typedef struct {
    char     name[128];
    int      file_index;
    int      orig_symidx;
    int      section_id;       /* Index in g_sections, -1 wenn undefiniert/extern */
    uint64_t value;            /* Offset innerhalb der Section (vor Placement) */
    int      is_global;
    int      is_defined;       /* 1 = definiert (auch wenn lokal) */
    uint64_t final_address;    /* erst nach Placement gueltig */
    int      resolved;         /* nach Symbolauflösung: final_address gueltig */
} Sym;

typedef struct {
    int      section_id;       /* welche InSection enthaelt diese Relocation */
    uint64_t offset;           /* Offset innerhalb der Section */
    int      sym_index;        /* Index in g_symbols */
    uint32_t type;             /* R_X86_64_* */
    int64_t  addend;
} Reloc;

typedef struct {
    char      filename[256];
    uint8_t  *raw;              /* komplette Datei im Speicher (mmap-artig via read) */
    size_t    raw_size;
    Elf64_Ehdr *ehdr;
    Elf64_Shdr *shdrs;
    const char *shstrtab;
    Elf64_Sym  *symtab;
    int         symtab_count;
    const char *strtab;
} InputFile;

static InputFile g_files[MAX_INPUT_FILES];
static int       g_file_count = 0;

static InSection g_sections[MAX_SECTIONS];
static int       g_section_count = 0;

static Sym       g_symbols[MAX_SYMBOLS];
static int       g_symbol_count = 0;

static Reloc     g_relocs[MAX_RELOCS];
static int       g_reloc_count = 0;

/* ---------------------------------------------------------------------
 * Hilfsfunktionen: Section-Klassifikation ("select"-Aequivalent aus
 * Abschnitt 6 des Dokuments -- hier fest verdrahtet statt per Wildcard,
 * da wir nur 4 Zielklassen kennen)
 * ------------------------------------------------------------------- */

static SectionKind classify_section(const char *name) {
    if (strncmp(name, ".text", 5) == 0)   return KIND_TEXT;
    if (strncmp(name, ".rodata", 7) == 0) return KIND_RODATA;
    if (strncmp(name, ".data", 5) == 0)   return KIND_DATA;
    if (strncmp(name, ".bss", 4) == 0)    return KIND_BSS;
    return KIND_IGNORED;   /* .eh_frame, .comment, .note.*, Debug-Sections, ... */
}

/* ---------------------------------------------------------------------
 * [1] Object-File-Reader
 * ------------------------------------------------------------------- */

static uint8_t *read_whole_file(const char *path, size_t *out_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); exit(1); }
    struct stat st;
    fstat(fd, &st);
    uint8_t *buf = malloc(st.st_size);
    if (read(fd, buf, st.st_size) != st.st_size) {
        fprintf(stderr, "minilink: short read on %s\n", path);
        exit(1);
    }
    close(fd);
    *out_size = st.st_size;
    return buf;
}

/* ELF64-Header lesbar auf stdout ausgeben (Debug-Hilfe) */
static void print_ehdr(const char *path, const Elf64_Ehdr *e) {
    printf("minilink: ELF-Header von %s\n", path);
    printf("    e_ident   : %02x %02x %02x %02x  class=%u data=%u version=%u osabi=%u\n",
           e->e_ident[0], e->e_ident[1], e->e_ident[2], e->e_ident[3],
           e->e_ident[EI_CLASS], e->e_ident[EI_DATA],
           e->e_ident[EI_VERSION], e->e_ident[EI_OSABI]);
    printf("    e_type    : 0x%04x (%s)\n", e->e_type,
           e->e_type == ET_REL  ? "ET_REL"  :
           e->e_type == ET_EXEC ? "ET_EXEC" :
           e->e_type == ET_DYN  ? "ET_DYN"  : "?");
    printf("    e_machine : 0x%04x%s\n", e->e_machine,
           e->e_machine == EM_X86_64 ? " (EM_X86_64)" : "");
    printf("    e_version : %u\n", e->e_version);
    printf("    e_entry   : 0x%lx\n", (unsigned long)e->e_entry);
    printf("    e_phoff   : %lu\n", (unsigned long)e->e_phoff);
    printf("    e_shoff   : %lu\n", (unsigned long)e->e_shoff);
    printf("    e_flags   : 0x%x\n", e->e_flags);
    printf("    e_ehsize  : %u\n", e->e_ehsize);
    printf("    e_phentsize/e_phnum : %u / %u\n", e->e_phentsize, e->e_phnum);
    printf("    e_shentsize/e_shnum : %u / %u\n", e->e_shentsize, e->e_shnum);
    printf("    e_shstrndx: %u\n", e->e_shstrndx);
}

/* Roh-Substring-Suche in einem Byte-Puffer (kein memmem-Zwang) */
static int buf_contains(const uint8_t *hay, size_t hlen, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || hlen < nlen) return 0;
    for (size_t i = 0; i + nlen <= hlen; i++)
        if (memcmp(hay + i, needle, nlen) == 0) return 1;
    return 0;
}

/* Heuristik: wurde diese Objektdatei mit einem TASKING-Toolset erzeugt?
 *
 * Der ELF-Header selbst verraet den Hersteller nicht (siehe README /
 * elf-aufbau.adoc). Belastbar in einer .o sind:
 *   - die .comment-Section  ("TASKING VX-toolset ... C compiler", "ctc" ...)
 *   - der DWARF-Producer-String in .debug_str / .debug_info
 * Wir scannen diese Sections nach den bekannten Marker-Strings. Ein echter
 * TASKING-Linkat haette zusaetzlich noch "_lc_*"-Symbole aus dem LSL.
 */
static int detect_tasking(const InputFile *f, const char **hit_section, const char **hit_marker) {
    static const char *markers[] = { "TASKING", "VX-toolset", "Altium", NULL };

    for (int i = 0; i < f->ehdr->e_shnum; i++) {
        const Elf64_Shdr *sh = &f->shdrs[i];
        if (sh->sh_type != SHT_PROGBITS) continue;
        const char *name = f->shstrtab + sh->sh_name;
        if (strcmp(name, ".comment") != 0 &&
            strncmp(name, ".debug_str", 10) != 0 &&
            strncmp(name, ".debug_info", 11) != 0)
            continue;

        /* Section-Bereich muss vollstaendig in der Datei liegen, sonst
           liest buf_contains() ueber das Dateiende hinaus. */
        if (sh->sh_offset > f->raw_size ||
            sh->sh_size   > f->raw_size - sh->sh_offset)
            continue;

        const uint8_t *data = f->raw + sh->sh_offset;
        for (int m = 0; markers[m]; m++)
            if (buf_contains(data, sh->sh_size, markers[m])) {
                if (hit_section) *hit_section = name;
                if (hit_marker)  *hit_marker  = markers[m];
                return 1;
            }
    }
    return 0;
}

static int load_object_file(const char *path) {
    InputFile *f = &g_files[g_file_count];
    strncpy(f->filename, path, sizeof(f->filename) - 1);
    f->raw = read_whole_file(path, &f->raw_size);
    f->ehdr = (Elf64_Ehdr *)f->raw;
    print_ehdr(path, f->ehdr);

    /* f->ehdr zusaetzlich "plain" ausgeben: die rohen sizeof(Elf64_Ehdr)
       Bytes als Hex-Dump, genau so wie sie in der Datei stehen */
    printf("minilink: ELF-Header (plain, %zu Bytes) von %s\n",
           sizeof(Elf64_Ehdr), path);
    {
        const unsigned char *p = (const unsigned char *)f->ehdr;
        for (size_t i = 0; i < sizeof(Elf64_Ehdr); i++) {
            if (i % 16 == 0) printf("    %04zx: ", i);
            printf("%02x ", p[i]);
            if (i % 16 == 15 || i + 1 == sizeof(Elf64_Ehdr)) printf("\n");
        }
    }

    if (memcmp(f->ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        fprintf(stderr, "minilink: %s ist keine gueltige ELF-Datei\n", path);
        exit(1);
    }
    if (f->ehdr->e_type != ET_REL) {
        fprintf(stderr, "minilink: %s ist keine relozierbare Objektdatei (ET_REL)\n", path);
        exit(1);
    }

    f->shdrs = (Elf64_Shdr *)(f->raw + f->ehdr->e_shoff);
    f->shstrtab = (const char *)(f->raw + f->shdrs[f->ehdr->e_shstrndx].sh_offset);

    /* Check: mit einem TASKING-Toolset erzeugt? (nur Hinweis, kein Abbruch) */
    const char *tk_sec = NULL, *tk_mark = NULL;
    if (detect_tasking(f, &tk_sec, &tk_mark))
        printf("minilink: Hinweis: %s wurde offenbar mit einem TASKING-Toolset erzeugt "
               "(Marker \"%s\" in Section %s)\n", path, tk_mark, tk_sec);

    /* Symtab- und Strtab-Section finden */
    f->symtab = NULL;
    f->strtab = NULL;
    for (int i = 0; i < f->ehdr->e_shnum; i++) {
        if (f->shdrs[i].sh_type == SHT_SYMTAB) {
            f->symtab = (Elf64_Sym *)(f->raw + f->shdrs[i].sh_offset);
            f->symtab_count = f->shdrs[i].sh_size / sizeof(Elf64_Sym);
            f->strtab = (const char *)(f->raw + f->shdrs[f->shdrs[i].sh_link].sh_offset);
        }
    }

    int this_file_index = g_file_count;
    g_file_count++;
    return this_file_index;
}

/* Section-Header-Index dieser Datei -> Index in unserem globalen g_sections-Array */
static int shndx_map[MAX_INPUT_FILES][MAX_SECTIONS];

static void import_sections(int file_index) {
    InputFile *f = &g_files[file_index];
    for (int i = 0; i < f->ehdr->e_shnum; i++) shndx_map[file_index][i] = -1;

    for (int i = 0; i < f->ehdr->e_shnum; i++) {
        Elf64_Shdr *sh = &f->shdrs[i];
        const char *name = f->shstrtab + sh->sh_name;
        SectionKind kind = classify_section(name);
        if (kind == KIND_IGNORED) continue;
        if (sh->sh_type != SHT_PROGBITS && sh->sh_type != SHT_NOBITS) continue;
        if (!(sh->sh_flags & SHF_ALLOC)) continue;   /* keine Loader-relevante Section */

        InSection *s = &g_sections[g_section_count];
        strncpy(s->name, name, sizeof(s->name) - 1);
        s->kind = kind;
        s->size = sh->sh_size;
        s->align = sh->sh_addralign ? sh->sh_addralign : 1;
        s->file_index = file_index;
        s->orig_shndx = i;
        if (kind == KIND_BSS) {
            s->data = NULL;
        } else {
            s->data = malloc(sh->sh_size);
            memcpy(s->data, f->raw + sh->sh_offset, sh->sh_size);
        }

        shndx_map[file_index][i] = g_section_count;
        g_section_count++;
    }
}

/* ---------------------------------------------------------------------
 * [2] Symbol-Resolver (siehe Dokument Abschnitt 5)
 * ------------------------------------------------------------------- */

static int find_symbol_by_name(const char *name) {
    for (int i = 0; i < g_symbol_count; i++)
        if (g_symbols[i].is_defined && strcmp(g_symbols[i].name, name) == 0)
            return i;
    return -1;
}

static void import_symbols(int file_index) {
    InputFile *f = &g_files[file_index];
    if (!f->symtab) return;

    for (int i = 0; i < f->symtab_count; i++) {
        Elf64_Sym *es = &f->symtab[i];
        int type = ELF64_ST_TYPE(es->st_info);
        int bind = ELF64_ST_BIND(es->st_info);
        if (type == STT_FILE) continue;

        /* SECTION-Symbole (leerer Name) werden von Relocations referenziert,
           die relativ zum Sectionanfang adressieren (typisch fuer statische
           Variablen wie unser "msg" in .rodata). Sie muessen importiert
           werden, damit find_symbol_by_origin() sie fuer die Relocation-
           Phase findet -- auch ohne brauchbaren Namen. */
        const char *raw_name = f->strtab + es->st_name;
        char synth_name[80];  /* "<section:" + name[64] + ">" + '\0' */
        const char *name = raw_name;
        if (type == STT_SECTION) {
            int target_id = shndx_map[file_index][es->st_shndx];
            snprintf(synth_name, sizeof(synth_name), "<section:%s>",
                     target_id >= 0 ? g_sections[target_id].name : "?");
            name = synth_name;
        } else if (!raw_name[0]) {
            continue;
        }

        int is_defined = (es->st_shndx != SHN_UNDEF);

        if (is_defined) {
            /* Multiple-Definition-Check bei globalen Symbolen (Abschnitt 5) */
            if (bind == STB_GLOBAL) {
                int existing = find_symbol_by_name(name);
                if (existing >= 0) {
                    fprintf(stderr,
                        "minilink: multiple definition of '%s' (in %s und vorheriger Datei)\n",
                        name, f->filename);
                    exit(1);
                }
            }
            Sym *s = &g_symbols[g_symbol_count++];
            strncpy(s->name, name, sizeof(s->name) - 1);
            s->file_index = file_index;
            s->orig_symidx = i;
            s->section_id = shndx_map[file_index][es->st_shndx];
            s->value = es->st_value;
            s->is_global = (bind == STB_GLOBAL);
            s->is_defined = 1;
            s->resolved = 0;
        } else {
            /* Undefiniertes Symbol: erstmal nur merken (fuer Relocations),
               Aufloesung passiert in resolve_all_symbols() */
            Sym *s = &g_symbols[g_symbol_count++];
            strncpy(s->name, name, sizeof(s->name) - 1);
            s->file_index = file_index;
            s->orig_symidx = i;
            s->section_id = -1;
            s->value = 0;
            s->is_global = 1;
            s->is_defined = 0;
            s->resolved = 0;
        }
    }
}

/* Gibt den Index in g_symbols zurueck, der (file_index, orig_symidx) entspricht --
   wird von der Relocation-Phase gebraucht, um von einem ELF-Symbolindex auf
   unsere interne Symbolstruktur zu kommen. */
static int find_symbol_by_origin(int file_index, int orig_symidx) {
    for (int i = 0; i < g_symbol_count; i++)
        if (g_symbols[i].file_index == file_index && g_symbols[i].orig_symidx == orig_symidx)
            return i;
    return -1;
}

/* Schreibt alle bekannten Symbole (aus allen Eingabedateien) in eine
   Textdatei. Wird von resolve_all_symbols() aufgerufen -- also VOR dem
   Placement, daher steht final_address hier noch nicht zur Verfuegung. */
static void dump_symbols(const char *path) {
    FILE *sf = fopen(path, "w");
    if (!sf) { perror(path); return; }

    fprintf(sf, "# minilink Symboltabelle (%d Eintraege, vor Placement)\n", g_symbol_count);
    fprintf(sf, "# %-32s %-9s %-7s %-18s %-16s %s\n",
            "Name", "Bindung", "Status", "Section", "Offset", "Quelldatei");

    for (int i = 0; i < g_symbol_count; i++) {
        Sym *s = &g_symbols[i];
        const char *bind   = s->is_global ? "GLOBAL" : "LOCAL";
        const char *status = s->is_defined ? "DEF" : "UNDEF";
        const char *section = (s->section_id >= 0)
                                ? g_sections[s->section_id].name
                                : "-";
        fprintf(sf, "  %-32s %-9s %-7s %-18s 0x%014lx   %s\n",
                s->name, bind, status, section,
                (unsigned long)s->value,
                g_files[s->file_index].filename);
    }

    fclose(sf);
    printf("minilink: %d Symbol(e) nach %s geschrieben\n", g_symbol_count, path);
}

static void resolve_all_symbols(void) {
    dump_symbols("symbols.txt");

    int unresolved_errors = 0;
    for (int i = 0; i < g_symbol_count; i++) {
        if (g_symbols[i].is_defined) continue;   /* wird erst in [4] final adressiert */
        int def = find_symbol_by_name(g_symbols[i].name);
        if (def < 0) {
            fprintf(stderr, "minilink: undefined reference to '%s'\n", g_symbols[i].name);
            unresolved_errors++;
        }
    }
    if (unresolved_errors > 0) {
        fprintf(stderr, "minilink: %d undefined symbol(s), Linking abgebrochen\n", unresolved_errors);
        exit(1);
    }
}

/* ---------------------------------------------------------------------
 * [3] Relocations importieren
 * ------------------------------------------------------------------- */

static void import_relocations(int file_index) {
    InputFile *f = &g_files[file_index];
    for (int i = 0; i < f->ehdr->e_shnum; i++) {
        Elf64_Shdr *sh = &f->shdrs[i];
        if (sh->sh_type != SHT_RELA) continue;

        int target_shndx = sh->sh_info;   /* Section, auf die sich diese Relocations beziehen */
        int target_section_id = shndx_map[file_index][target_shndx];
        if (target_section_id < 0) continue;   /* Ziel-Section wurde ignoriert (z.B. .eh_frame) */

        Elf64_Rela *relas = (Elf64_Rela *)(f->raw + sh->sh_offset);
        int count = sh->sh_size / sizeof(Elf64_Rela);

        for (int r = 0; r < count; r++) {
            int elf_symidx = ELF64_R_SYM(relas[r].r_info);
            int sym_index = find_symbol_by_origin(file_index, elf_symidx);
            if (sym_index < 0) {
                fprintf(stderr, "minilink: interner Fehler: Relocation-Symbol nicht gefunden\n");
                exit(1);
            }
            Reloc *rel = &g_relocs[g_reloc_count++];
            rel->section_id = target_section_id;
            rel->offset = relas[r].r_offset;
            rel->sym_index = sym_index;
            rel->type = ELF64_R_TYPE(relas[r].r_info);
            rel->addend = relas[r].r_addend;
        }
    }
}

/* ---------------------------------------------------------------------
 * [4] Placement / Locator (siehe Dokument Abschnitt 7)
 *
 * Vereinfachtes statisches Layout, angelehnt an ein klassisches
 * "non-PIE" ELF-Executable:
 *   0x400000  ELF-Header + Program-Header
 *   0x401000  .text     (R-X Segment)
 *   ---- Seitengrenze ----
 *   0x...     .rodata, .data (RW Segment)
 *   0x...     .bss      (RW, kein File-Inhalt, nur virtueller Speicher)
 * ------------------------------------------------------------------- */

/* Layout-Parameter. Kein eingebautes Default -- beide MUESSEN aus einem
   LDL-Script (-T) kommen (siehe load_ldl_script()). 0 = noch nicht
   gesetzt; main() bricht dann mit Fehlermeldung ab.
   Ein klassisches non-PIE x86-64-Layout ist BASE_ADDR=0x400000,
   PAGE_SIZE=0x1000 -- siehe test/default.ldl. */
static uint64_t g_base_addr;   /* 0 = ungesetzt */
static uint64_t g_page_size;   /* 0 = ungesetzt */

/* Minimaler LDL-/Linkerscript-Reader.
 *
 * Erkennt Zeilen der Form
 *     #define BASE_ADDR   0x400000UL
 *     #define PAGE_SIZE   0x1000UL
 * (Ganzzahl-Suffixe wie U/L/UL/ULL werden ignoriert, Basis via strtoull
 * mit "0" -> 0x.. / 0.. / dezimal). Alle anderen Zeilen -- Leerzeilen,
 * Kommentare, unbekannte Schluessel -- werden ueberlesen. Das ist bewusst
 * das absolute Minimum: ein echtes LSL/Linkerscript kann Sections
 * platzieren, Speicherbereiche definieren usw. (siehe README, Abschnitt
 * "Was bewusst fehlt"). */
static void load_ldl_script(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }

    char line[256];
    int lineno = 0, hits = 0, in_block_comment = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;

        /* Kommentare entfernen, BEVOR geparst wird: C-Zeilenkommentare
           (doppelter Schraegstrich bis Zeilenende) sowie C-Blockkommentare
           (auch ueber mehrere Zeilen). Ein auskommentiertes #define -- auch
           innerhalb eines Blockkommentars -- darf keine Wirkung haben. */
        char clean[sizeof(line)];
        size_t ci = 0;
        for (size_t i = 0; line[i] && ci + 1 < sizeof(clean); i++) {
            if (in_block_comment) {
                if (line[i] == '*' && line[i + 1] == '/') { in_block_comment = 0; i++; }
                continue;
            }
            if (line[i] == '/' && line[i + 1] == '*') { in_block_comment = 1; i++; continue; }
            if (line[i] == '/' && line[i + 1] == '/') break;
            clean[ci++] = line[i];
        }
        clean[ci] = '\0';

        char name[64], value[64];
        if (sscanf(clean, " #define %63s %63s", name, value) != 2)
            continue;

        /* Integer-Suffix (u/U/l/L) abschneiden */
        for (char *p = value; *p; p++)
            if (*p == 'u' || *p == 'U' || *p == 'l' || *p == 'L') { *p = '\0'; break; }
        uint64_t v = strtoull(value, NULL, 0);

        if (strcmp(name, "BASE_ADDR") == 0)      { g_base_addr = v; hits++; }
        else if (strcmp(name, "PAGE_SIZE") == 0) { g_page_size = v; hits++; }
        else
            fprintf(stderr, "minilink: %s:%d: unbekannter Schluessel '%s' (ignoriert)\n",
                    path, lineno, name);
    }
    fclose(f);

    if (g_base_addr == 0) {
        fprintf(stderr, "minilink: %s: BASE_ADDR fehlt (erwartet: #define BASE_ADDR <adresse>)\n", path);
        exit(1);
    }
    if (g_page_size < 0x1000 || (g_page_size & (g_page_size - 1)) != 0) {
        fprintf(stderr, "minilink: %s: PAGE_SIZE (0x%lx) muss eine Zweierpotenz >= 0x1000 sein\n",
                path, (unsigned long)g_page_size);
        exit(1);
    }
    if (g_base_addr & (g_page_size - 1)) {
        fprintf(stderr, "minilink: %s: BASE_ADDR (0x%lx) ist nicht page-aligned "
                "(PAGE_SIZE 0x%lx) -- das erzeugte ELF waere nicht ladbar\n",
                path, (unsigned long)g_base_addr, (unsigned long)g_page_size);
        exit(1);
    }

    printf("minilink: LDL-Script %s geladen (%d Wert(e): BASE_ADDR=0x%lx PAGE_SIZE=0x%lx)\n",
           path, hits, (unsigned long)g_base_addr, (unsigned long)g_page_size);
}

static uint64_t align_up(uint64_t v, uint64_t a) {
    if (a < 2) return v;
    return (v + a - 1) & ~(a - 1);
}

/* ---------------------------------------------------------------------
 * TASKING-LSL-Reader (--lsl) -- stark vereinfachte Teilmenge
 *
 * Gelesen wird:
 *   memory <id> {
 *       type = rom | ram | nvram;
 *       size = <zahl>[k|M|G];
 *       map ( ... dest_offset = <adresse> ... );
 *   }
 *   section_layout [ ::<name> ] {
 *       group [ ( ... run_addr = mem:<id> ... ) ] {
 *           select "<pattern>";   // '*' am Ende = Praefix-Wildcard
 *           ...
 *       }
 *   }
 *
 * Daraus:
 *   - erste  type=rom-Region  -> Basisadresse des R-X-Segments  (g_base_addr)
 *   - erste  type=ram-Region  -> Basisadresse des RW-Segments   (g_rw_base)
 *   - Reihenfolge + Segment-Zuordnung der Output-Sections aus den groups
 *
 * Alles Uebrige (architecture, bus, derivative, core, section_setup, ...)
 * wird per Klammer-Skip ueberlesen. Kommentare: // und C-Bloecke.
 * ------------------------------------------------------------------- */

#define LSL_MAX_MEM     16
#define LSL_MAX_GROUPS  16
#define LSL_MAX_SEL     32

typedef struct { char id[32]; int is_ram; uint64_t addr; uint64_t size; } LslMem;
typedef struct {
    int  is_rw;                       /* run_addr zeigt auf eine type=ram-Region */
    char run_mem[32];
    char sel[LSL_MAX_SEL][48];
    int  n_sel;
} LslGroup;

static LslMem   g_lsl_mem[LSL_MAX_MEM];    static int g_lsl_mem_n = 0;
static LslGroup g_lsl_grp[LSL_MAX_GROUPS]; static int g_lsl_grp_n = 0;
static uint64_t g_rw_base = 0;             /* RW-Segment-Basis (aus type=ram) */
static int      g_use_lsl = 0;

/* --- Lexer ---------------------------------------------------------- */
enum { TOK_EOF, TOK_ID, TOK_NUM, TOK_STR, TOK_PUNC };
typedef struct { int kind; char text[128]; uint64_t num; } Tok;

static const char *g_lx;          /* Cursor in den (NUL-terminierten) Skriptpuffer */
static Tok         g_tok;         /* aktuelles Token (1 Token Lookahead) */

static uint64_t lsl_num(const char *s) {
    char *end;
    uint64_t v = strtoull(s, &end, 0);
    if      (*end == 'k' || *end == 'K') v *= 1024ULL;
    else if (*end == 'm' || *end == 'M') v *= 1024ULL * 1024;
    else if (*end == 'g' || *end == 'G') v *= 1024ULL * 1024 * 1024;
    return v;
}

static void lx_skip_ws(void) {
    for (;;) {
        while (*g_lx && (unsigned char)*g_lx <= ' ') g_lx++;
        if (g_lx[0] == '/' && g_lx[1] == '/') {
            while (*g_lx && *g_lx != '\n') g_lx++;
            continue;
        }
        if (g_lx[0] == '/' && g_lx[1] == '*') {
            g_lx += 2;
            while (*g_lx && !(g_lx[0] == '*' && g_lx[1] == '/')) g_lx++;
            if (*g_lx) g_lx += 2;
            continue;
        }
        break;
    }
}

static void adv(void) {
    Tok t; t.kind = TOK_EOF; t.text[0] = '\0'; t.num = 0;
    lx_skip_ws();
    unsigned char c = (unsigned char)*g_lx;
    if (!c) { g_tok = t; return; }

    if (c == '"') {
        g_lx++;
        size_t n = 0;
        while (*g_lx && *g_lx != '"' && n + 1 < sizeof t.text) t.text[n++] = *g_lx++;
        t.text[n] = '\0';
        if (*g_lx == '"') g_lx++;
        t.kind = TOK_STR;
    } else if (isdigit(c)) {
        size_t n = 0;
        while ((isalnum((unsigned char)*g_lx) || *g_lx == 'x' || *g_lx == 'X')
               && n + 1 < sizeof t.text)
            t.text[n++] = *g_lx++;
        t.text[n] = '\0';
        t.num = lsl_num(t.text);
        t.kind = TOK_NUM;
    } else if (isalpha(c) || c == '_' || c == '.') {
        size_t n = 0;
        while ((isalnum((unsigned char)*g_lx) || *g_lx == '_' || *g_lx == '.' || *g_lx == '*')
               && n + 1 < sizeof t.text)
            t.text[n++] = *g_lx++;
        t.text[n] = '\0';
        t.kind = TOK_ID;
    } else {
        t.kind = TOK_PUNC;
        t.text[0] = *g_lx++;
        t.text[1] = '\0';
    }
    g_tok = t;
}

static int is_punc(char p) { return g_tok.kind == TOK_PUNC && g_tok.text[0] == p; }
static int is_id(const char *s) { return g_tok.kind == TOK_ID && strcmp(g_tok.text, s) == 0; }

/* Ab der aktuellen Position bis hinter das passende '}' bzw. bis ';'
   auf Tiefe 0 (fuer nicht unterstuetzte Konstrukte). */
static void skip_unknown(void) {
    int depth = 0, saw_brace = 0;
    while (g_tok.kind != TOK_EOF) {
        if (is_punc('{')) { depth++; saw_brace = 1; adv(); continue; }
        if (is_punc('}')) { depth--; adv(); if (saw_brace && depth <= 0) return; continue; }
        if (is_punc(';') && depth == 0 && !saw_brace) { adv(); return; }
        adv();
    }
}

/* --- Parser ------------------------------------------------------- */
static void lsl_parse_memory(void) {
    adv();                                        /* -> <id> */
    LslMem m; memset(&m, 0, sizeof m);
    if (g_tok.kind == TOK_ID) { strncpy(m.id, g_tok.text, sizeof m.id - 1); adv(); }
    if (!is_punc('{')) { skip_unknown(); return; }
    adv();

    int depth = 1;
    while (depth > 0 && g_tok.kind != TOK_EOF) {
        if (is_punc('{')) { depth++; adv(); continue; }
        if (is_punc('}')) { depth--; adv(); continue; }

        if (depth == 1 && is_id("type")) {
            adv(); if (is_punc('=')) adv();
            if (g_tok.kind == TOK_ID)
                m.is_ram = (strcmp(g_tok.text, "ram") == 0 || strcmp(g_tok.text, "nvram") == 0);
            adv();
            continue;
        }
        if (depth == 1 && is_id("size")) {
            adv(); if (is_punc('=')) adv();
            if (g_tok.kind == TOK_NUM) m.size = g_tok.num;
            adv();
            continue;
        }
        if (is_id("map")) {
            adv();                                /* '(' ... ')' scannen */
            int pd = 0;
            while (g_tok.kind != TOK_EOF) {
                if (is_punc('(')) { pd++; adv(); continue; }
                if (is_punc(')')) { pd--; adv(); if (pd <= 0) break; continue; }
                if (is_id("dest_offset")) {
                    adv(); if (is_punc('=')) adv();
                    if (g_tok.kind == TOK_NUM) m.addr = g_tok.num;
                    continue;
                }
                adv();
            }
            continue;
        }
        adv();
    }

    if (m.id[0] && g_lsl_mem_n < LSL_MAX_MEM) g_lsl_mem[g_lsl_mem_n++] = m;
    else if (m.id[0]) fprintf(stderr, "minilink: LSL: zu viele memory-Regionen, '%s' ignoriert\n", m.id);
}

static void lsl_parse_group(void) {
    adv();                                        /* nach 'group' */
    LslGroup g; memset(&g, 0, sizeof g);

    if (is_punc('(')) {
        adv();
        while (!is_punc(')') && g_tok.kind != TOK_EOF) {
            if (is_id("run_addr") || is_id("load_addr")) {
                int is_run = is_id("run_addr");
                adv(); if (is_punc('=')) adv();
                if (is_id("mem")) {
                    adv(); if (is_punc(':')) adv();
                    if (is_run && g_tok.kind == TOK_ID)
                        strncpy(g.run_mem, g_tok.text, sizeof g.run_mem - 1);
                    if (g_tok.kind == TOK_ID) adv();
                } else {
                    adv();
                }
                continue;
            }
            adv();
        }
        if (is_punc(')')) adv();
    }

    if (!is_punc('{')) { skip_unknown(); return; }
    adv();
    while (!is_punc('}') && g_tok.kind != TOK_EOF) {
        if (is_id("select")) {
            adv();
            if (g_tok.kind == TOK_STR || g_tok.kind == TOK_ID) {
                if (g.n_sel < LSL_MAX_SEL)
                    strncpy(g.sel[g.n_sel++], g_tok.text, sizeof g.sel[0] - 1);
                adv();
            }
            if (is_punc(';')) adv();
        } else if (is_id("group")) {
            fprintf(stderr, "minilink: LSL: verschachtelte group wird ignoriert\n");
            adv();
            skip_unknown();
        } else {
            adv();
        }
    }
    if (is_punc('}')) adv();

    if (g.run_mem[0]) {
        int found = 0;
        for (int i = 0; i < g_lsl_mem_n; i++)
            if (strcmp(g_lsl_mem[i].id, g.run_mem) == 0) { g.is_rw = g_lsl_mem[i].is_ram; found = 1; }
        if (!found)
            fprintf(stderr, "minilink: LSL: group run_addr = mem:%s -- unbekannte Region\n", g.run_mem);
    }
    if (g.n_sel > 0 && g_lsl_grp_n < LSL_MAX_GROUPS) g_lsl_grp[g_lsl_grp_n++] = g;
}

static void lsl_parse_section_layout(void) {
    adv();                                        /* nach 'section_layout' */
    while (is_punc(':')) adv();                   /* optionales ::name */
    if (g_tok.kind == TOK_ID) adv();
    if (!is_punc('{')) { skip_unknown(); return; }
    adv();
    while (!is_punc('}') && g_tok.kind != TOK_EOF) {
        if (is_id("group"))       lsl_parse_group();
        else if (is_id("select")) {              /* select ausserhalb group: ignorieren */
            adv(); if (g_tok.kind == TOK_STR || g_tok.kind == TOK_ID) adv();
            if (is_punc(';')) adv();
        } else adv();
    }
    if (is_punc('}')) adv();
}

static void load_lsl_script(const char *path) {
    size_t sz;
    uint8_t *raw = read_whole_file(path, &sz);
    char *buf = malloc(sz + 1);
    if (!buf) { perror("malloc"); exit(1); }
    memcpy(buf, raw, sz);
    buf[sz] = '\0';
    free(raw);

    g_lx = buf;
    adv();
    while (g_tok.kind != TOK_EOF) {
        if (is_id("memory"))              lsl_parse_memory();
        else if (is_id("section_layout")) lsl_parse_section_layout();
        else if (g_tok.kind == TOK_ID)    { adv(); skip_unknown(); }
        else                              adv();
    }
    free(buf);

    int rom_i = -1, ram_i = -1;
    for (int i = 0; i < g_lsl_mem_n; i++) {
        if (!g_lsl_mem[i].is_ram && rom_i < 0) rom_i = i;
        if ( g_lsl_mem[i].is_ram && ram_i < 0) ram_i = i;
    }
    if (rom_i < 0) {
        fprintf(stderr, "minilink: %s: keine 'memory' mit type=rom gefunden\n", path);
        exit(1);
    }

    g_base_addr = g_lsl_mem[rom_i].addr;
    g_rw_base   = (ram_i >= 0) ? g_lsl_mem[ram_i].addr : 0;
    g_page_size = 0x1000;            /* LSL kennt keine Page-Size -> ELF-Standardwert */
    g_use_lsl   = 1;

    if (g_base_addr == 0 || (g_base_addr & (g_page_size - 1))) {
        fprintf(stderr, "minilink: %s: rom-Adresse 0x%lx fehlt oder nicht 0x%lx-aligned\n",
                path, (unsigned long)g_base_addr, (unsigned long)g_page_size);
        exit(1);
    }
    if (g_rw_base && (g_rw_base & (g_page_size - 1))) {
        fprintf(stderr, "minilink: %s: ram-Adresse 0x%lx nicht 0x%lx-aligned\n",
                path, (unsigned long)g_rw_base, (unsigned long)g_page_size);
        exit(1);
    }

    printf("minilink: LSL %s geladen: %d memory-Region(en), %d group(s); rom@0x%lx ram@0x%lx\n",
           path, g_lsl_mem_n, g_lsl_grp_n,
           (unsigned long)g_base_addr, (unsigned long)g_rw_base);
}

/* '*' am Ende = Praefix-Match, sonst exakter Vergleich */
static int sel_match(const char *pat, const char *name) {
    size_t pl = strlen(pat);
    if (pl && pat[pl - 1] == '*') return strncmp(pat, name, pl - 1) == 0;
    return strcmp(pat, name) == 0;
}

typedef struct {
    uint64_t rx_vaddr, rx_file_size;                            /* Segment 0: File-Offset 0, enthaelt ELF-Header */
    uint64_t rw_vaddr, rw_file_off, rw_file_size, rw_mem_size;  /* Segment 1 */
} Layout;

/* Platziert die Sektionen aus idx[0..n) fortlaufend ab *cursor (Offset ab
   Segmentanfang), setzt segment + merged_offset, beachtet Alignment. */
static void place_run(const int *idx, int n, int segment, uint64_t *cursor) {
    for (int k = 0; k < n; k++) {
        InSection *s = &g_sections[idx[k]];
        *cursor = align_up(*cursor, s->align);
        s->segment = segment;
        s->merged_offset = *cursor;
        *cursor += s->size;
    }
}

/* Baut das gemeinsame Layout aus drei Section-Listen (schon in
   Ziel-Reihenfolge): rx[] -> R-X-Segment, rw[] -> RW-PROGBITS,
   bs[] -> RW-NOBITS (.bss). rw_base = 0 -> RW direkt hinter R-X. */
static Layout finalize_layout(const int *rx, int n_rx,
                              const int *rw, int n_rw,
                              const int *bs, int n_bs,
                              uint64_t rw_base) {
    Layout L = {0};

    /* Segment 0 (R-X): erste g_page_size Bytes = ELF-Header + Program-Header */
    uint64_t cur = g_page_size;
    place_run(rx, n_rx, 0, &cur);
    L.rx_vaddr     = g_base_addr;
    L.rx_file_size = cur;

    L.rw_file_off = align_up(L.rx_file_size, g_page_size);
    L.rw_vaddr    = rw_base ? rw_base : align_up(g_base_addr + L.rx_file_size, g_page_size);

    uint64_t rcur = 0;
    place_run(rw, n_rw, 1, &rcur);
    L.rw_file_size = rcur;                 /* .bss traegt NICHT zum File-Image bei */
    place_run(bs, n_bs, 1, &rcur);
    L.rw_mem_size  = rcur;                 /* ... wohl aber zum Speicherabbild     */

    uint64_t rx_end = L.rx_vaddr + L.rx_file_size;
    uint64_t rw_end = L.rw_vaddr + L.rw_mem_size;
    if (L.rw_mem_size && L.rx_vaddr < rw_end && L.rw_vaddr < rx_end) {
        fprintf(stderr, "minilink: R-X- und RW-Bereich ueberlappen "
                "(R-X 0x%lx..0x%lx, RW 0x%lx..0x%lx)\n",
                (unsigned long)L.rx_vaddr, (unsigned long)rx_end,
                (unsigned long)L.rw_vaddr, (unsigned long)rw_end);
        exit(1);
    }
    return L;
}

/* Default-Layout (Script per -T): .text -> R-X; .rodata/.data/.bss -> RW,
   RW-Segment direkt an der naechsten Page-Grenze hinter .text. */
static Layout place_sections_default(void) {
    int rx[MAX_SECTIONS], rw[MAX_SECTIONS], bs[MAX_SECTIONS];
    int n_rx = 0, n_rw = 0, n_bs = 0;

    for (int i = 0; i < g_section_count; i++)
        if (g_sections[i].kind == KIND_TEXT) rx[n_rx++] = i;
    for (int i = 0; i < g_section_count; i++)
        if (g_sections[i].kind == KIND_RODATA) rw[n_rw++] = i;
    for (int i = 0; i < g_section_count; i++)
        if (g_sections[i].kind == KIND_DATA) rw[n_rw++] = i;
    for (int i = 0; i < g_section_count; i++)
        if (g_sections[i].kind == KIND_BSS) bs[n_bs++] = i;

    return finalize_layout(rx, n_rx, rw, n_rw, bs, n_bs, 0);
}

/* LSL-Layout (--lsl): Reihenfolge und Segment aus section_layout/group/
   select; nicht erfasste Sektionen werden nach Typ einsortiert. */
static Layout place_sections_lsl(void) {
    int rx[MAX_SECTIONS], rw[MAX_SECTIONS], bs[MAX_SECTIONS];
    int n_rx = 0, n_rw = 0, n_bs = 0;
    int claimed[MAX_SECTIONS];
    for (int i = 0; i < g_section_count; i++) claimed[i] = 0;

    for (int gi = 0; gi < g_lsl_grp_n; gi++) {
        LslGroup *g = &g_lsl_grp[gi];
        for (int si = 0; si < g->n_sel; si++)
            for (int i = 0; i < g_section_count; i++) {
                if (claimed[i] || !sel_match(g->sel[si], g_sections[i].name)) continue;
                claimed[i] = 1;
                if (g_sections[i].kind == KIND_BSS) bs[n_bs++] = i;
                else if (g->is_rw)                  rw[n_rw++] = i;
                else                                rx[n_rx++] = i;
            }
    }
    for (int i = 0; i < g_section_count; i++) {
        if (claimed[i]) continue;
        fprintf(stderr, "minilink: LSL: Section '%s' von keiner group erfasst -- "
                "nach Typ einsortiert\n", g_sections[i].name);
        switch (g_sections[i].kind) {
            case KIND_TEXT: case KIND_RODATA: rx[n_rx++] = i; break;
            case KIND_DATA:                   rw[n_rw++] = i; break;
            case KIND_BSS:                    bs[n_bs++] = i; break;
            default: break;
        }
    }

    return finalize_layout(rx, n_rx, rw, n_rw, bs, n_bs, g_rw_base);
}

/* Nachdem Sections platziert sind: jedem Symbol seine finale Adresse geben */
static void assign_symbol_addresses(const Layout *L) {
    for (int i = 0; i < g_symbol_count; i++) {
        Sym *s = &g_symbols[i];
        if (!s->is_defined) continue;
        if (s->section_id < 0) { s->resolved = 1; s->final_address = 0; continue; } /* abs. Symbol, hier ungenutzt */

        InSection *sec = &g_sections[s->section_id];
        uint64_t section_base = (sec->segment == 0) ? L->rx_vaddr : L->rw_vaddr;
        s->final_address = section_base + sec->merged_offset + s->value;
        s->resolved = 1;
    }

    /* Undefinierte Symbole (aus Sicht ihrer urspruenglichen Datei) auf die
       jetzt final adressierte Definition mappen */
    for (int i = 0; i < g_symbol_count; i++) {
        Sym *s = &g_symbols[i];
        if (s->is_defined) continue;
        int def = find_symbol_by_name(s->name);
        s->final_address = g_symbols[def].final_address;
        s->resolved = 1;
    }
}

/* ---------------------------------------------------------------------
 * [5] Relocation-Engine (siehe Dokument Abschnitt 8)
 * ------------------------------------------------------------------- */

static Layout g_layout_for_reloc;   /* vom Driver vor apply_relocations() gesetzt */

static void apply_relocations(void) {
    for (int i = 0; i < g_reloc_count; i++) {
        Reloc *r = &g_relocs[i];
        InSection *sec = &g_sections[r->section_id];
        if (sec->kind == KIND_BSS) continue;  /* .bss hat keinen File-Inhalt zum Patchen */

        Sym *sym = &g_symbols[r->sym_index];
        uint64_t S = sym->final_address;   /* Symbolwert (finale Adresse) */
        int64_t  A = r->addend;            /* konstanter Offset aus der Relocation */

        /* P = finale virtuelle Adresse der Patch-Stelle selbst
           (Segment-Basis + Offset der Section im Segment + lokaler Offset) */
        uint64_t section_base = (sec->segment == 0) ? g_layout_for_reloc.rx_vaddr
                                                    : g_layout_for_reloc.rw_vaddr;
        uint64_t P = section_base + sec->merged_offset + r->offset;

        /* sec->data enthaelt die rohen Bytes genau dieser einen InSection;
           r->offset ist relativ dazu (kein zusaetzlicher Merge-Offset noetig,
           da wir hier direkt in die Quellbytes patchen, bevor sie im
           Output-Writer an ihre Merge-Position kopiert werden). */
        uint8_t *patch_loc = sec->data + r->offset;

        switch (r->type) {
            case R_X86_64_PC32:
            case R_X86_64_PLT32: {
                /* Bei uns gibt es kein PLT (kein dynamisches Linken) ->
                   PLT32 wird wie ein ganz normaler PC-relativer 32-Bit-Verweis behandelt */
                int64_t value = (int64_t)S + A - (int64_t)P;
                int32_t v32 = (int32_t)value;
                if (value != (int64_t)v32) {
                    fprintf(stderr, "minilink: PC32-Relocation ausserhalb des 32-Bit-Bereichs fuer '%s'\n",
                            sym->name);
                    exit(1);
                }
                memcpy(patch_loc, &v32, 4);
                break;
            }
            case R_X86_64_64: {
                uint64_t value = S + (uint64_t)A;
                memcpy(patch_loc, &value, 8);
                break;
            }
            case R_X86_64_32S:
            case R_X86_64_32: {
                int64_t value = (int64_t)S + A;
                int32_t v32 = (int32_t)value;
                memcpy(patch_loc, &v32, 4);
                break;
            }
            default:
                fprintf(stderr, "minilink: nicht unterstuetzter Relocation-Typ %u\n", r->type);
                exit(1);
        }
    }
}

/* ---------------------------------------------------------------------
 * [6] Output-Writer: schreibt ein minimales, lauffaehiges statisches
 * ELF64-Executable mit 2 PT_LOAD-Segmenten (R-X und RW).
 * ------------------------------------------------------------------- */

/* Baut das komplette Byte-Image eines PT_LOAD-Segments (`seg` = 0 R-X,
   1 RW) in einem calloc-genullten Puffer von `span` Bytes: jede InSection
   dieses Segments wird an ihren merged_offset kopiert, .bss (kein data)
   uebersprungen, Alignment-Luecken bleiben 0. `prefix`/`prefix_len` wird
   an Offset 0 vorangestellt (fuer den ELF-Header im R-X-Segment). */
static uint8_t *build_segment_image(int seg, uint64_t span,
                                    const void *prefix, size_t prefix_len) {
    uint8_t *img = calloc(1, span ? span : 1);
    if (!img) { perror("calloc"); exit(1); }
    if (prefix && prefix_len) memcpy(img, prefix, prefix_len);

    for (int i = 0; i < g_section_count; i++) {
        InSection *s = &g_sections[i];
        if (s->segment != seg || !s->data) continue;   /* !data => .bss */
        if (s->merged_offset + s->size > span) {
            fprintf(stderr, "minilink: interner Fehler: Section '%s' passt nicht ins "
                    "Segment-Image (0x%lx+0x%lx > 0x%lx)\n", s->name,
                    (unsigned long)s->merged_offset, (unsigned long)s->size,
                    (unsigned long)span);
            exit(1);
        }
        memcpy(img + s->merged_offset, s->data, s->size);
    }
    return img;
}

static void write_output(const char *path, const Layout *L, uint64_t entry_addr) {
    FILE *out = fopen(path, "wb");
    if (!out) { perror(path); exit(1); }

    Elf64_Ehdr eh = {0};
    memcpy(eh.e_ident, ELFMAG, SELFMAG);
    eh.e_ident[EI_CLASS] = ELFCLASS64;
    eh.e_ident[EI_DATA] = ELFDATA2LSB;
    eh.e_ident[EI_VERSION] = EV_CURRENT;
    eh.e_ident[EI_OSABI] = ELFOSABI_SYSV;
    eh.e_type = ET_EXEC;
    eh.e_machine = EM_X86_64;
    eh.e_version = EV_CURRENT;
    eh.e_entry = entry_addr;
    eh.e_phoff = sizeof(Elf64_Ehdr);
    eh.e_shoff = 0;               /* keine Section-Header noetig, um das Binary auszufuehren */
    eh.e_ehsize = sizeof(Elf64_Ehdr);
    eh.e_phentsize = sizeof(Elf64_Phdr);
    eh.e_phnum = 2;                /* R-X Segment + RW Segment */
    eh.e_shentsize = 0;
    eh.e_shnum = 0;
    eh.e_shstrndx = SHN_UNDEF;

    Elf64_Phdr ph_text = {0};
    ph_text.p_type   = PT_LOAD;
    ph_text.p_flags  = PF_R | PF_X;
    ph_text.p_offset = 0;                 /* Segment beginnt am Dateianfang (inkl. Header) */
    ph_text.p_vaddr  = L->rx_vaddr;
    ph_text.p_paddr  = L->rx_vaddr;
    ph_text.p_filesz = L->rx_file_size;
    ph_text.p_memsz  = L->rx_file_size;
    ph_text.p_align  = g_page_size;

    Elf64_Phdr ph_rw = {0};
    ph_rw.p_type   = PT_LOAD;
    ph_rw.p_flags  = PF_R | PF_W;
    ph_rw.p_offset = L->rw_file_off;
    ph_rw.p_vaddr  = L->rw_vaddr;
    ph_rw.p_paddr  = L->rw_vaddr;
    ph_rw.p_filesz = L->rw_file_size;     /* .bss zaehlt NICHT zu filesz ... */
    ph_rw.p_memsz  = L->rw_mem_size;      /* ... aber zu memsz -> Kernel nullt den Rest */
    ph_rw.p_align  = g_page_size;

    /* Header + beide Program-Header als Prefix des R-X-Segment-Images */
    uint8_t hdr[sizeof(Elf64_Ehdr) + 2 * sizeof(Elf64_Phdr)];
    memcpy(hdr, &eh, sizeof eh);
    memcpy(hdr + sizeof eh, &ph_text, sizeof ph_text);
    memcpy(hdr + sizeof eh + sizeof ph_text, &ph_rw, sizeof ph_rw);

    uint8_t *rx = build_segment_image(0, L->rx_file_size, hdr, sizeof hdr);
    fwrite(rx, 1, L->rx_file_size, out);
    free(rx);

    for (uint64_t pos = L->rx_file_size; pos < L->rw_file_off; pos++)
        fputc(0, out);

    if (L->rw_file_size) {
        uint8_t *rw = build_segment_image(1, L->rw_file_size, NULL, 0);
        fwrite(rw, 1, L->rw_file_size, out);
        free(rw);
    }
    /* .bss wird NICHT in die Datei geschrieben (SHT_NOBITS-Prinzip) */

    fclose(out);
    chmod(path, 0755);
}

/* ---------------------------------------------------------------------
 * Debug-Ausgabe: MAP-File-artige Uebersicht (siehe Dokument Abschnitt 9)
 * ------------------------------------------------------------------- */

static const char *kind_name(SectionKind k) {
    switch (k) {
        case KIND_TEXT:   return ".text";
        case KIND_RODATA: return ".rodata";
        case KIND_DATA:   return ".data";
        case KIND_BSS:    return ".bss";
        default:          return "?";
    }
}

/* qsort-Vergleich: definierte Symbole nach finaler Adresse */
static int sym_addr_cmp(const void *a, const void *b) {
    const Sym *x = &g_symbols[*(const int *)a];
    const Sym *y = &g_symbols[*(const int *)b];
    if (x->final_address < y->final_address) return -1;
    if (x->final_address > y->final_address) return  1;
    return strcmp(x->name, y->name);
}

/* qsort-Vergleich: Sektionen nach finaler virtueller Adresse */
static int sec_addr_cmp(const void *a, const void *b) {
    const InSection *x = &g_sections[*(const int *)a];
    const InSection *y = &g_sections[*(const int *)b];
    /* segment 0 liegt (per Layout) immer vor segment 1 */
    if (x->segment != y->segment) return x->segment - y->segment;
    if (x->merged_offset < y->merged_offset) return -1;
    if (x->merged_offset > y->merged_offset) return  1;
    return 0;
}

/* Schreibt die vollstaendige MAP nach `out` (stdout und/oder minilink.map).
   entry_addr = Adresse von _start, script = "-T ..." bzw. "--lsl ...". */
static void emit_map(FILE *out, const Layout *L, uint64_t entry_addr, const char *script) {
    fprintf(out, "=== minilink MAP ===\n");
    fprintf(out, "Script     : %s\n", script);
    fprintf(out, "Entry      : _start @ 0x%08lx\n", (unsigned long)entry_addr);
    if (g_use_lsl)
        fprintf(out, "Layout     : BASE_ADDR=0x%08lx  PAGE_SIZE=0x%lx  RAM_BASE=0x%08lx\n",
                (unsigned long)g_base_addr, (unsigned long)g_page_size,
                (unsigned long)g_rw_base);
    else
        fprintf(out, "Layout     : BASE_ADDR=0x%08lx  PAGE_SIZE=0x%lx\n",
                (unsigned long)g_base_addr, (unsigned long)g_page_size);

    if (g_use_lsl && g_lsl_mem_n > 0) {
        fprintf(out, "\nMemory-Regionen (LSL)\n");
        fprintf(out, "  %-10s %-5s %-12s %10s\n", "Name", "Typ", "Adresse", "Groesse");
        for (int i = 0; i < g_lsl_mem_n; i++)
            fprintf(out, "  %-10s %-5s 0x%08lx %8lu K\n",
                    g_lsl_mem[i].id, g_lsl_mem[i].is_ram ? "ram" : "rom",
                    (unsigned long)g_lsl_mem[i].addr,
                    (unsigned long)(g_lsl_mem[i].size >> 10));
    }

    fprintf(out, "\nSegmente (PT_LOAD)\n");
    fprintf(out, "  %-3s %-5s %-25s %-10s %-9s %-9s\n",
            "#", "Flags", "VirtAddr", "FileOff", "FileSz", "MemSz");
    fprintf(out, "  %-3d %-5s 0x%08lx-0x%08lx    0x%08x 0x%07lx 0x%07lx\n",
            0, "R-X", (unsigned long)L->rx_vaddr,
            (unsigned long)(L->rx_vaddr + L->rx_file_size), 0u,
            (unsigned long)L->rx_file_size, (unsigned long)L->rx_file_size);
    fprintf(out, "  %-3d %-5s 0x%08lx-0x%08lx    0x%08lx 0x%07lx 0x%07lx\n",
            1, "RW", (unsigned long)L->rw_vaddr,
            (unsigned long)(L->rw_vaddr + L->rw_mem_size),
            (unsigned long)L->rw_file_off,
            (unsigned long)L->rw_file_size, (unsigned long)L->rw_mem_size);

    fprintf(out, "\nSektionen (nach Adresse; 0-Byte-Sektionen ausgelassen)\n");
    fprintf(out, "  %-4s %-16s %-12s %-12s %-9s %-6s %s\n",
            "Seg", "Section", "VirtAddr", "EndAddr", "Size", "Align", "Quelle");
    int sord[MAX_SECTIONS], sn = 0, skipped = 0;
    for (int i = 0; i < g_section_count; i++) {
        if (g_sections[i].size == 0) { skipped++; continue; }
        sord[sn++] = i;
    }
    qsort(sord, sn, sizeof sord[0], sec_addr_cmp);
    for (int k = 0; k < sn; k++) {
        InSection *s = &g_sections[sord[k]];
        uint64_t base = (s->segment == 0) ? L->rx_vaddr : L->rw_vaddr;
        uint64_t va = base + s->merged_offset;
        char nm[24];
        snprintf(nm, sizeof nm, "%s%s", kind_name(s->kind),
                 s->kind == KIND_BSS ? " (NOBITS)" : "");
        fprintf(out, "  %-4s %-16s 0x%08lx   0x%08lx   0x%07lx %-6lu %s\n",
                s->segment == 0 ? "R-X" : "RW", nm,
                (unsigned long)va, (unsigned long)(va + s->size),
                (unsigned long)s->size, (unsigned long)s->align,
                g_files[s->file_index].filename);
    }
    if (skipped)
        fprintf(out, "  (%d leere Section(s) ausgelassen)\n", skipped);

    fprintf(out, "\nSymbole (definiert, nach Adresse)\n");
    fprintf(out, "  %-12s %-6s %-4s %-10s %-22s %s\n",
            "VirtAddr", "Bind", "Seg", "Section", "Name", "Quelle");
    int order[MAX_SYMBOLS], n = 0;
    for (int i = 0; i < g_symbol_count; i++)
        if (g_symbols[i].is_defined &&
            strncmp(g_symbols[i].name, "<section:", 9) != 0)   /* synthetische SECTION-Symbole ausblenden */
            order[n++] = i;
    qsort(order, n, sizeof order[0], sym_addr_cmp);
    for (int k = 0; k < n; k++) {
        Sym *s = &g_symbols[order[k]];
        const char *seg = "-", *sec = "(abs)";
        if (s->section_id >= 0) {
            seg = g_sections[s->section_id].segment == 0 ? "R-X" : "RW";
            sec = kind_name(g_sections[s->section_id].kind);
        }
        fprintf(out, "  0x%08lx   %-6s %-4s %-10s %-22s %s\n",
                (unsigned long)s->final_address,
                s->is_global ? "GLOBAL" : "LOCAL", seg, sec, s->name,
                g_files[s->file_index].filename);
    }
    fprintf(out, "=====================\n");
}

static void print_map(const Layout *L, uint64_t entry_addr, const char *script) {
    printf("\n");
    emit_map(stdout, L, entry_addr, script);

    FILE *mf = fopen("minilink.map", "w");
    if (mf) {
        emit_map(mf, L, entry_addr, script);
        fclose(mf);
        printf("minilink: MAP zusaetzlich geschrieben nach minilink.map\n\n");
    } else {
        perror("minilink.map");
        printf("\n");
    }
}

/* Schreibt alle geladenen Datei-Indizes samt Dateiname nach files.txt */
static void dump_files(const int *file_indices, int n_inputs) {
    FILE *ff = fopen("files.txt", "w");
    if (!ff) { perror("files.txt"); return; }

    fprintf(ff, "# minilink: %d Eingabedatei(en)\n", n_inputs);
    fprintf(ff, "# %-10s %s\n", "file_index", "Dateiname");
    for (int i = 0; i < n_inputs; i++)
        fprintf(ff, "  %-10d %s\n",
                file_indices[i], g_files[file_indices[i]].filename);

    fclose(ff);
    printf("minilink: %d Datei-Index/Indizes nach files.txt geschrieben\n", n_inputs);
}

/* Schreibt nur die reinen file_index-Werte (einer pro Zeile) nach
   file_indices.txt */
static void dump_file_indices(const int *file_indices, int n_inputs) {
    FILE *fi = fopen("file_indices.txt", "w");
    if (!fi) { perror("file_indices.txt"); return; }

    for (int i = 0; i < n_inputs; i++)
        fprintf(fi, "%d\n", file_indices[i]);

    fclose(fi);
    printf("minilink: %d file_index-Wert(e) nach file_indices.txt geschrieben\n", n_inputs);
}

/* ---------------------------------------------------------------------
 * Driver (siehe Dokument Abschnitt 11, Rolle "Driver")
 * ------------------------------------------------------------------- */

int main(int argc, char **argv) {
    const char *output_path = NULL;
    const char *ldl_path    = NULL;   /* -T   : #define-Miniscript          */
    const char *lsl_path    = NULL;   /* --lsl: vereinfachtes TASKING-LSL   */
    const char *inputs[MAX_INPUT_FILES];
    int n_inputs = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (++i >= argc) { fprintf(stderr, "minilink: -o braucht ein Argument\n"); return 1; }
            output_path = argv[i];
        } else if (strcmp(argv[i], "-T") == 0) {
            if (++i >= argc) { fprintf(stderr, "minilink: -T braucht ein Argument\n"); return 1; }
            ldl_path = argv[i];
        } else if (strncmp(argv[i], "-T", 2) == 0) {
            ldl_path = argv[i] + 2;                /* -Tscript.ldl (ohne Leerzeichen) */
        } else if (strcmp(argv[i], "--lsl") == 0) {
            if (++i >= argc) { fprintf(stderr, "minilink: --lsl braucht ein Argument\n"); return 1; }
            lsl_path = argv[i];
        } else if (strncmp(argv[i], "--lsl=", 6) == 0) {
            lsl_path = argv[i] + 6;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "minilink: unbekannte Option '%s'\n", argv[i]);
            return 1;
        } else {
            if (n_inputs >= MAX_INPUT_FILES) {
                fprintf(stderr, "minilink: zu viele Eingabedateien (max %d)\n", MAX_INPUT_FILES);
                return 1;
            }
            inputs[n_inputs++] = argv[i];
        }
    }

    if (!output_path || n_inputs == 0) {
        fprintf(stderr,
            "usage: %s (-T <script.ldl> | --lsl <script.lsl>) <input1.o> [...] -o <output>\n",
            argv[0]);
        return 1;
    }

    /* Layout-Quelle: genau EINES von -T / --lsl. */
    if (ldl_path && lsl_path) {
        fprintf(stderr, "minilink: -T und --lsl schliessen sich aus\n");
        return 1;
    }
    if (!ldl_path && !lsl_path) {
        fprintf(stderr, "minilink: kein Linkerscript angegeben -- entweder\n"
                        "  -T <script.ldl>     (#define BASE_ADDR / PAGE_SIZE, z.B. test/default.ldl)\n"
                        "  --lsl <script.lsl>  (vereinfachtes TASKING-LSL, z.B. test/tc27x.lsl)\n");
        return 1;
    }
    if (lsl_path) load_lsl_script(lsl_path);   /* setzt g_base_addr/g_rw_base/g_page_size, g_use_lsl=1 */
    else          load_ldl_script(ldl_path);   /* validiert BASE_ADDR/PAGE_SIZE, bricht sonst ab      */

    printf("minilink: linke %d Objektdatei(en) -> %s\n", n_inputs, output_path);

    /* [1] Reader */
    int file_indices[MAX_INPUT_FILES];
    for (int i = 0; i < n_inputs; i++)
    {
      file_indices[i] = load_object_file(inputs[i]);
    }

    dump_files(file_indices, n_inputs);
    dump_file_indices(file_indices, n_inputs);

    for (int i = 0; i < n_inputs; i++)
    {
        import_sections(file_indices[i]);
        import_symbols(file_indices[i]);
        import_relocations(file_indices[i]);
    }

    /* [2] Symbol-Resolver */
    resolve_all_symbols();

    /* [3]/[4] Placement */
    Layout L = g_use_lsl ? place_sections_lsl() : place_sections_default();
    g_layout_for_reloc = L;
    assign_symbol_addresses(&L);

    int entry_idx = find_symbol_by_name("_start");
    if (entry_idx < 0) {
        fprintf(stderr, "minilink: kein Einsprungpunkt '_start' gefunden\n");
        return 1;
    }
    uint64_t entry_addr = g_symbols[entry_idx].final_address;

    /* [5] Relocation */
    apply_relocations();

    /* [6] Output */
    write_output(output_path, &L, entry_addr);

    char script_desc[300];
    snprintf(script_desc, sizeof script_desc, "%s %s",
             lsl_path ? "--lsl" : "-T", lsl_path ? lsl_path : ldl_path);
    print_map(&L, entry_addr, script_desc);
    printf("minilink: fertig. Einsprungpunkt _start @ 0x%08lx\n", entry_addr);
    return 0;
}
