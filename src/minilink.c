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
    uint64_t    merged_offset; /* Offset innerhalb der gemergten Output-Section */
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

#define BASE_ADDR      0x400000UL
#define PAGE_SIZE      0x1000UL

static uint64_t align_up(uint64_t v, uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}

typedef struct {
    uint64_t text_vaddr, text_size;
    uint64_t rodata_vaddr, rodata_size;
    uint64_t data_vaddr, data_size;
    uint64_t bss_vaddr, bss_size;
    uint64_t rw_segment_file_start_vaddr; /* rodata+data zusammen im File-Image */
} Layout;

static Layout place_sections(void) {
    Layout L = {0};

    /* Kopfbereich: ELF-Header + 1 Program-Header reservieren wir vorab
       (wird in write_output() exakt berechnet; hier grob genug fuer Alignment) */
    uint64_t cursor = BASE_ADDR + PAGE_SIZE;   /* .text beginnt an einer Page-Grenze */

    L.text_vaddr = cursor;
    for (int i = 0; i < g_section_count; i++) {
        if (g_sections[i].kind != KIND_TEXT) continue;
        cursor = align_up(cursor, g_sections[i].align);
        g_sections[i].merged_offset = cursor - L.text_vaddr;
        cursor += g_sections[i].size;
    }
    L.text_size = cursor - L.text_vaddr;

    /* Naechstes Segment (RW: .rodata + .data) beginnt an neuer Page */
    cursor = align_up(cursor, PAGE_SIZE);
    L.rw_segment_file_start_vaddr = cursor;

    L.rodata_vaddr = cursor;
    for (int i = 0; i < g_section_count; i++) {
        if (g_sections[i].kind != KIND_RODATA) continue;
        cursor = align_up(cursor, g_sections[i].align);
        g_sections[i].merged_offset = cursor - L.rodata_vaddr;
        cursor += g_sections[i].size;
    }
    L.rodata_size = cursor - L.rodata_vaddr;

    L.data_vaddr = cursor;
    for (int i = 0; i < g_section_count; i++) {
        if (g_sections[i].kind != KIND_DATA) continue;
        cursor = align_up(cursor, g_sections[i].align);
        g_sections[i].merged_offset = cursor - L.data_vaddr;
        cursor += g_sections[i].size;
    }
    L.data_size = cursor - L.data_vaddr;

    /* .bss folgt direkt danach im virtuellen Adressraum, hat aber KEINEN
       Platz im File-Image (das ist der Sinn von SHT_NOBITS / memsz > filesz) */
    L.bss_vaddr = cursor;
    for (int i = 0; i < g_section_count; i++) {
        if (g_sections[i].kind != KIND_BSS) continue;
        cursor = align_up(cursor, g_sections[i].align);
        g_sections[i].merged_offset = cursor - L.bss_vaddr;
        cursor += g_sections[i].size;
    }
    L.bss_size = cursor - L.bss_vaddr;

    return L;
}

/* Nachdem Sections platziert sind: jedem Symbol seine finale Adresse geben */
static void assign_symbol_addresses(const Layout *L) {
    (void)L;
    for (int i = 0; i < g_symbol_count; i++) {
        Sym *s = &g_symbols[i];
        if (!s->is_defined) continue;
        if (s->section_id < 0) { s->resolved = 1; s->final_address = 0; continue; } /* abs. Symbol, hier ungenutzt */

        InSection *sec = &g_sections[s->section_id];
        uint64_t section_base;
        switch (sec->kind) {
            case KIND_TEXT:   section_base = L->text_vaddr;   break;
            case KIND_RODATA: section_base = L->rodata_vaddr; break;
            case KIND_DATA:   section_base = L->data_vaddr;   break;
            case KIND_BSS:    section_base = L->bss_vaddr;    break;
            default: continue;
        }
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
           (Section-Basis im Layout + Merge-Offset innerhalb der Zielgruppe + lokaler Offset) */
        uint64_t section_base;
        switch (sec->kind) {
            case KIND_TEXT:   section_base = g_layout_for_reloc.text_vaddr;   break;
            case KIND_RODATA: section_base = g_layout_for_reloc.rodata_vaddr; break;
            case KIND_DATA:   section_base = g_layout_for_reloc.data_vaddr;   break;
            default: continue;
        }
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

static void write_section_group(FILE *out, SectionKind kind) {
    for (int i = 0; i < g_section_count; i++)
        if (g_sections[i].kind == kind && g_sections[i].data)
            fwrite(g_sections[i].data, 1, g_sections[i].size, out);
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
    ph_text.p_type = PT_LOAD;
    ph_text.p_flags = PF_R | PF_X;
    ph_text.p_offset = 0;                 /* Segment beginnt am Dateianfang (inkl. Header) */
    ph_text.p_vaddr = BASE_ADDR;
    ph_text.p_paddr = BASE_ADDR;
    ph_text.p_filesz = PAGE_SIZE + L->text_size;
    ph_text.p_memsz  = PAGE_SIZE + L->text_size;
    ph_text.p_align  = PAGE_SIZE;

    uint64_t rw_file_offset = PAGE_SIZE + L->text_size;
    rw_file_offset = align_up(rw_file_offset, PAGE_SIZE);
    uint64_t rw_filesz = (L->rodata_size + L->data_size);

    Elf64_Phdr ph_rw = {0};
    ph_rw.p_type = PT_LOAD;
    ph_rw.p_flags = PF_R | PF_W;
    ph_rw.p_offset = rw_file_offset;
    ph_rw.p_vaddr = L->rw_segment_file_start_vaddr;
    ph_rw.p_paddr = L->rw_segment_file_start_vaddr;
    ph_rw.p_filesz = rw_filesz;                         /* .bss zaehlt NICHT zu filesz */
    ph_rw.p_memsz  = rw_filesz + L->bss_size;            /* aber zu memsz -> Kernel nullt den Rest */
    ph_rw.p_align  = PAGE_SIZE;

    fwrite(&eh, sizeof(eh), 1, out);
    fwrite(&ph_text, sizeof(ph_text), 1, out);
    fwrite(&ph_rw, sizeof(ph_rw), 1, out);

    /* Padding bis zur .text-Page-Grenze */
    uint64_t pos = sizeof(eh) + 2 * sizeof(Elf64_Phdr);
    while (pos < PAGE_SIZE) { fputc(0, out); pos++; }

    write_section_group(out, KIND_TEXT);
    pos = PAGE_SIZE + L->text_size;

    while (pos < rw_file_offset) { fputc(0, out); pos++; }

    write_section_group(out, KIND_RODATA);
    write_section_group(out, KIND_DATA);
    /* .bss wird NICHT in die Datei geschrieben (SHT_NOBITS-Prinzip) */

    fclose(out);
    chmod(path, 0755);
}

/* ---------------------------------------------------------------------
 * Debug-Ausgabe: MAP-File-artige Uebersicht (siehe Dokument Abschnitt 9)
 * ------------------------------------------------------------------- */

static void print_map(const Layout *L) {
    printf("\n=== minilink MAP ===\n");
    printf("%-20s 0x%08lx  size=0x%-8lx\n", ".text",   L->text_vaddr,   L->text_size);
    printf("%-20s 0x%08lx  size=0x%-8lx\n", ".rodata", L->rodata_vaddr, L->rodata_size);
    printf("%-20s 0x%08lx  size=0x%-8lx\n", ".data",   L->data_vaddr,   L->data_size);
    printf("%-20s 0x%08lx  size=0x%-8lx  (nicht im File-Image)\n", ".bss", L->bss_vaddr, L->bss_size);
    printf("\n%-20s %-12s %s\n", "Symbol", "Adresse", "Quelle");
    for (int i = 0; i < g_symbol_count; i++) {
        if (!g_symbols[i].is_defined || !g_symbols[i].is_global) continue;
        printf("%-20s 0x%08lx    %s\n", g_symbols[i].name, g_symbols[i].final_address,
               g_files[g_symbols[i].file_index].filename);
    }
    printf("=====================\n\n");
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
    if (argc < 4 || strcmp(argv[argc-2], "-o") != 0) {
        fprintf(stderr, "usage: %s <input1.o> [input2.o ...] -o <output>\n", argv[0]);
        return 1;
    }
    const char *output_path = argv[argc - 1];
    int n_inputs = argc - 3;

    printf("minilink: linke %d Objektdatei(en) -> %s\n", n_inputs, output_path);

    /* [1] Reader */
    int file_indices[MAX_INPUT_FILES];
    for (int i = 0; i < n_inputs; i++)
    {
      file_indices[i] = load_object_file(argv[1 + i]);
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
    Layout L = place_sections();
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

    print_map(&L);
    printf("minilink: fertig. Einsprungpunkt _start @ 0x%08lx\n", entry_addr);
    return 0;
}
