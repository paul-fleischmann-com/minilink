//! minilink — ein echter, minimaler ELF64-Linker für x86-64 Linux (Rust-Portierung).
//!
//! Implementiert die vollstaendige Pipeline eines Locate-Linkers:
//!   [1] Object-File-Reader   (ELF64 .o parsen)
//!   [2] Symbol-Resolver      (globale Symboltabelle ueber alle Eingabedateien)
//!   [3] Section-Merger       (.text/.data/.bss/.rodata ueber alle Dateien zusammenfassen)
//!   [4] Placement/Locator    (finale Adressen vergeben, statisches Layout)
//!   [5] Relocation-Engine    (R_X86_64_PC32, R_X86_64_PLT32, R_X86_64_64, R_X86_64_32S)
//!   [6] Output-Writer        (lauffaehiges statisches ELF64-Executable)
//!
//! 1:1-Portierung der C-Referenzimplementierung (`src/minilink.c`) -- gleiche
//! CLI, gleiches Verhalten, gleiche bewussten Vereinfachungen (siehe README).

#![allow(dead_code)]

mod elf;
mod lsl;

use std::collections::HashMap;
use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::process;

// -------------------------------------------------------------------------
// [1] Object-File-Reader: interne Repraesentation
// -------------------------------------------------------------------------

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum SectionKind {
    Text,
    Rodata,
    Data,
    Bss,
    Ignored,
}

fn classify_section(name: &str) -> SectionKind {
    if name.starts_with(".text") {
        SectionKind::Text
    } else if name.starts_with(".rodata") {
        SectionKind::Rodata
    } else if name.starts_with(".data") {
        SectionKind::Data
    } else if name.starts_with(".bss") {
        SectionKind::Bss
    } else {
        SectionKind::Ignored
    }
}

fn kind_name(k: SectionKind) -> &'static str {
    match k {
        SectionKind::Text => ".text",
        SectionKind::Rodata => ".rodata",
        SectionKind::Data => ".data",
        SectionKind::Bss => ".bss",
        SectionKind::Ignored => "?",
    }
}

struct InSection {
    name: String,
    kind: SectionKind,
    data: Option<Vec<u8>>, // None bei .bss
    size: u64,
    align: u64,
    file_index: usize,
    orig_shndx: usize,
    seg_index: usize,     // Index in Layout.seg (nach Placement)
    merged_offset: u64,   // Offset ab Anfang des enthaltenden PT_LOAD-Segments
}

struct Sym {
    name: String,
    file_index: usize,
    orig_symidx: usize,
    section_id: Option<usize>, // Index in Linker.sections
    keep_id: Option<usize>,    // Index in Linker.keep (nur --debug)
    value: u64,                // Offset innerhalb der Section (vor Placement)
    is_global: bool,
    is_defined: bool,
    final_address: u64, // erst nach Placement gueltig
    resolved: bool,
}

#[derive(Clone, Copy)]
struct Reloc {
    section_id: Option<usize>,
    keep_id: Option<usize>,
    keep_base: u64,
    offset: u64,
    sym_index: usize,
    rtype: u32,
    addend: i64,
}

/// --debug: nicht ladbare Sections (.debug_*), die wir 1:1 durchreichen,
/// ueber alle Eingabedateien konkateniert.
struct KeepSec {
    name: String,
    sh_type: u32,
    align: u64,
    data: Vec<u8>,
    chunk_off: HashMap<usize, u64>, // file_index -> Start des Datei-Chunks in data
}

struct InputFile {
    filename: String,
    raw: Vec<u8>,
    ehdr: elf::Ehdr,
    shdrs: Vec<elf::Shdr>,
    shstrtab: Vec<u8>,
    symtab: Vec<elf::Sym>,
    strtab: Vec<u8>,
}

fn align_up(v: u64, a: u64) -> u64 {
    if a < 2 {
        v
    } else {
        (v + a - 1) & !(a - 1)
    }
}

/// Roh-Substring-Suche in einem Byte-Puffer.
fn buf_contains(hay: &[u8], needle: &str) -> bool {
    let n = needle.as_bytes();
    if n.is_empty() || hay.len() < n.len() {
        return false;
    }
    hay.windows(n.len()).any(|w| w == n)
}

/// Heuristik: wurde diese Objektdatei mit einem TASKING-Toolset erzeugt?
fn detect_tasking(raw: &[u8], shdrs: &[elf::Shdr], shstrtab: &[u8]) -> Option<(String, String)> {
    const MARKERS: [&str; 3] = ["TASKING", "VX-toolset", "Altium"];
    for sh in shdrs {
        if sh.sh_type != elf::SHT_PROGBITS {
            continue;
        }
        let name = elf::cstr_at(shstrtab, sh.sh_name as usize);
        if name != ".comment" && !name.starts_with(".debug_str") && !name.starts_with(".debug_info") {
            continue;
        }
        if sh.sh_offset as usize > raw.len() || sh.sh_size as usize > raw.len() - sh.sh_offset as usize {
            continue;
        }
        let data = &raw[sh.sh_offset as usize..(sh.sh_offset + sh.sh_size) as usize];
        for m in MARKERS {
            if buf_contains(data, m) {
                return Some((name, m.to_string()));
            }
        }
    }
    None
}

fn print_ehdr(path: &str, e: &elf::Ehdr, raw: &[u8]) {
    println!("minilink: ELF-Header von {}", path);
    println!(
        "    e_ident   : {:02x} {:02x} {:02x} {:02x}  class={} data={} version={} osabi={}",
        e.e_ident[0], e.e_ident[1], e.e_ident[2], e.e_ident[3], e.e_ident[4], e.e_ident[5], e.e_ident[6], e.e_ident[7]
    );
    let tyname = match e.e_type {
        1 => "ET_REL",
        2 => "ET_EXEC",
        3 => "ET_DYN",
        _ => "?",
    };
    println!("    e_type    : 0x{:04x} ({})", e.e_type, tyname);
    println!(
        "    e_machine : 0x{:04x}{}",
        e.e_machine,
        if e.e_machine == elf::EM_X86_64 { " (EM_X86_64)" } else { "" }
    );
    println!("    e_version : {}", e.e_version);
    println!("    e_entry   : 0x{:x}", e.e_entry);
    println!("    e_phoff   : {}", e.e_phoff);
    println!("    e_shoff   : {}", e.e_shoff);
    println!("    e_flags   : 0x{:x}", e.e_flags);
    println!("    e_ehsize  : {}", e.e_ehsize);
    println!("    e_phentsize/e_phnum : {} / {}", e.e_phentsize, e.e_phnum);
    println!("    e_shentsize/e_shnum : {} / {}", e.e_shentsize, e.e_shnum);
    println!("    e_shstrndx: {}", e.e_shstrndx);

    println!("minilink: ELF-Header (plain, {} Bytes) von {}", elf::Ehdr::SIZE, path);
    let n = elf::Ehdr::SIZE.min(raw.len());
    let mut i = 0;
    while i < n {
        print!("    {:04x}: ", i);
        let end = (i + 16).min(n);
        for b in &raw[i..end] {
            print!("{:02x} ", b);
        }
        println!();
        i = end;
    }
}

// -------------------------------------------------------------------------
// Placement / Locator
// -------------------------------------------------------------------------

struct OutSeg {
    mem_index: Option<usize>, // Index in Linker.lsl_mem; None im -T-Modus
    is_rw: bool,
    has_header: bool,
    vaddr: u64,
    file_off: u64,
    file_size: u64,
    mem_size: u64,
}

struct Layout {
    seg: Vec<OutSeg>,
}

fn check_overlaps(l: &Layout) {
    for a in 0..l.seg.len() {
        for b in (a + 1)..l.seg.len() {
            let ae = l.seg[a].vaddr + l.seg[a].mem_size;
            let be = l.seg[b].vaddr + l.seg[b].mem_size;
            if l.seg[a].mem_size != 0 && l.seg[b].mem_size != 0 && l.seg[a].vaddr < be && l.seg[b].vaddr < ae {
                eprintln!(
                    "minilink: Segmente ueberlappen (0x{:x}..0x{:x} / 0x{:x}..0x{:x})",
                    l.seg[a].vaddr, ae, l.seg[b].vaddr, be
                );
                process::exit(1);
            }
        }
    }
}

fn parse_c_uint(s: &str) -> u64 {
    let s = s.trim();
    if let Some(hex) = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
        u64::from_str_radix(hex, 16).unwrap_or(0)
    } else if s.len() > 1 && s.starts_with('0') {
        u64::from_str_radix(&s[1..], 8).unwrap_or(0)
    } else {
        s.parse::<u64>().unwrap_or(0)
    }
}

// -------------------------------------------------------------------------
// Linker: gesamter Zustand (Aequivalent der C-Globals)
// -------------------------------------------------------------------------

struct Linker {
    files: Vec<InputFile>,
    shndx_map: Vec<Vec<Option<usize>>>, // [file_index][orig_shndx] -> sections-Index
    keep_map: Vec<Vec<Option<usize>>>,  // [file_index][orig_shndx] -> keep-Index

    sections: Vec<InSection>,
    symbols: Vec<Sym>,
    relocs: Vec<Reloc>,
    keep: Vec<KeepSec>,

    debug_mode: bool,

    base_addr: u64,
    page_size: u64,
    rw_base: u64,
    use_lsl: bool,
    lsl_mem: Vec<lsl::LslMem>,
    lsl_grp: Vec<lsl::LslGroup>,
}

impl Linker {
    fn new(debug_mode: bool) -> Self {
        Linker {
            files: Vec::new(),
            shndx_map: Vec::new(),
            keep_map: Vec::new(),
            sections: Vec::new(),
            symbols: Vec::new(),
            relocs: Vec::new(),
            keep: Vec::new(),
            debug_mode,
            base_addr: 0,
            page_size: 0,
            rw_base: 0,
            use_lsl: false,
            lsl_mem: Vec::new(),
            lsl_grp: Vec::new(),
        }
    }

    // --- [1] Object-File-Reader -----------------------------------------

    fn load_object_file(&mut self, path: &str) -> usize {
        let raw = fs::read(path).unwrap_or_else(|e| {
            eprintln!("{}: {}", path, e);
            process::exit(1);
        });
        let ehdr = elf::Ehdr::parse(&raw);
        print_ehdr(path, &ehdr, &raw);

        if &ehdr.e_ident[0..4] != elf::ELFMAG {
            eprintln!("minilink: {} ist keine gueltige ELF-Datei", path);
            process::exit(1);
        }
        if ehdr.e_type != elf::ET_REL {
            eprintln!("minilink: {} ist keine relozierbare Objektdatei (ET_REL)", path);
            process::exit(1);
        }

        let mut shdrs = Vec::with_capacity(ehdr.e_shnum as usize);
        for i in 0..ehdr.e_shnum as usize {
            let off = ehdr.e_shoff as usize + i * elf::Shdr::SIZE;
            shdrs.push(elf::Shdr::parse(&raw[off..off + elf::Shdr::SIZE]));
        }
        let shstr_shdr = &shdrs[ehdr.e_shstrndx as usize];
        let shstrtab = raw[shstr_shdr.sh_offset as usize..(shstr_shdr.sh_offset + shstr_shdr.sh_size) as usize].to_vec();

        if let Some((sec, mark)) = detect_tasking(&raw, &shdrs, &shstrtab) {
            println!(
                "minilink: Hinweis: {} wurde offenbar mit einem TASKING-Toolset erzeugt (Marker \"{}\" in Section {})",
                path, mark, sec
            );
        }

        let mut symtab = Vec::new();
        let mut strtab = Vec::new();
        for sh in &shdrs {
            if sh.sh_type == elf::SHT_SYMTAB {
                let count = (sh.sh_size / elf::Sym::SIZE as u64) as usize;
                symtab.clear();
                for i in 0..count {
                    let off = sh.sh_offset as usize + i * elf::Sym::SIZE;
                    symtab.push(elf::Sym::parse(&raw[off..off + elf::Sym::SIZE]));
                }
                let str_sh = &shdrs[sh.sh_link as usize];
                strtab = raw[str_sh.sh_offset as usize..(str_sh.sh_offset + str_sh.sh_size) as usize].to_vec();
            }
        }

        let n_shdrs = shdrs.len();
        let file_index = self.files.len();
        self.files.push(InputFile {
            filename: path.to_string(),
            raw,
            ehdr,
            shdrs,
            shstrtab,
            symtab,
            strtab,
        });
        self.shndx_map.push(vec![None; n_shdrs]);
        self.keep_map.push(vec![None; n_shdrs]);
        file_index
    }

    /// Haengt den Section-Inhalt an die passende keep-Section an (--debug) und
    /// merkt sich den Chunk-Offset dieser Datei. Legt die keep-Section bei
    /// Bedarf an. Gibt den Index in self.keep zurueck.
    fn keep_append(&mut self, file_index: usize, name: &str, sh: &elf::Shdr, bytes: &[u8]) -> usize {
        let ki = match self.keep.iter().position(|k| k.name == name) {
            Some(x) => x,
            None => {
                self.keep.push(KeepSec {
                    name: name.to_string(),
                    sh_type: sh.sh_type,
                    align: if sh.sh_addralign != 0 { sh.sh_addralign } else { 1 },
                    data: Vec::new(),
                    chunk_off: HashMap::new(),
                });
                self.keep.len() - 1
            }
        };
        if sh.sh_addralign > self.keep[ki].align {
            self.keep[ki].align = sh.sh_addralign;
        }
        let a = if self.keep[ki].align > 0 { self.keep[ki].align } else { 1 };
        let base = align_up(self.keep[ki].data.len() as u64, a);
        self.keep[ki].data.resize(base as usize, 0);
        self.keep[ki].data.extend_from_slice(bytes);
        self.keep[ki].chunk_off.insert(file_index, base);
        ki
    }

    fn import_sections(&mut self, file_index: usize) {
        let raw = self.files[file_index].raw.clone();
        let shdrs = self.files[file_index].shdrs.clone();
        let shstrtab = self.files[file_index].shstrtab.clone();
        let debug_mode = self.debug_mode;

        for i in 0..shdrs.len() {
            let sh = &shdrs[i];
            let name = elf::cstr_at(&shstrtab, sh.sh_name as usize);
            let kind = classify_section(&name);

            // --debug: .debug_*-Sections behalten (nicht ladbar -> nicht in sections)
            if debug_mode && kind == SectionKind::Ignored && sh.sh_type == elf::SHT_PROGBITS && name.starts_with(".debug") {
                let bytes = raw[sh.sh_offset as usize..(sh.sh_offset + sh.sh_size) as usize].to_vec();
                let ki = self.keep_append(file_index, &name, sh, &bytes);
                self.keep_map[file_index][i] = Some(ki);
                continue;
            }

            if kind == SectionKind::Ignored {
                continue;
            }
            if sh.sh_type != elf::SHT_PROGBITS && sh.sh_type != elf::SHT_NOBITS {
                continue;
            }
            if sh.sh_flags & elf::SHF_ALLOC == 0 {
                continue; // keine Loader-relevante Section
            }

            let data = if kind == SectionKind::Bss {
                None
            } else {
                Some(raw[sh.sh_offset as usize..(sh.sh_offset + sh.sh_size) as usize].to_vec())
            };

            let sec = InSection {
                name,
                kind,
                size: sh.sh_size,
                align: if sh.sh_addralign != 0 { sh.sh_addralign } else { 1 },
                data,
                file_index,
                orig_shndx: i,
                seg_index: 0,
                merged_offset: 0,
            };

            self.shndx_map[file_index][i] = Some(self.sections.len());
            self.sections.push(sec);
        }
    }

    // --- [2] Symbol-Resolver ----------------------------------------------

    fn find_symbol_by_name(&self, name: &str) -> Option<usize> {
        self.symbols.iter().position(|s| s.is_defined && s.name == name)
    }

    fn find_symbol_by_origin(&self, file_index: usize, orig_symidx: usize) -> Option<usize> {
        self.symbols.iter().position(|s| s.file_index == file_index && s.orig_symidx == orig_symidx)
    }

    fn import_symbols(&mut self, file_index: usize) {
        let symtab = self.files[file_index].symtab.clone();
        let strtab = self.files[file_index].strtab.clone();
        let filename = self.files[file_index].filename.clone();

        for i in 0..symtab.len() {
            let es = symtab[i];
            let ty = elf::st_type(es.st_info);
            let bind = elf::st_bind(es.st_info);
            if ty == elf::STT_FILE {
                continue;
            }

            let raw_name = elf::cstr_at(&strtab, es.st_name as usize);

            // SECTION-Symbole (leerer Name) werden von Relocations referenziert,
            // die relativ zum Sectionanfang adressieren.
            let (name, sec_id, keep_id): (String, Option<usize>, Option<usize>);
            if ty == elf::STT_SECTION {
                let sid = self.shndx_map[file_index].get(es.st_shndx as usize).copied().flatten();
                let kid = self.keep_map[file_index].get(es.st_shndx as usize).copied().flatten();
                let tn = if let Some(s) = sid {
                    self.sections[s].name.clone()
                } else if let Some(k) = kid {
                    self.keep[k].name.clone()
                } else {
                    "?".to_string()
                };
                name = format!("<section:{}>", tn);
                sec_id = sid;
                keep_id = kid;
            } else if raw_name.is_empty() {
                continue;
            } else {
                name = raw_name;
                sec_id = None;
                keep_id = None;
            }

            let is_defined = es.st_shndx != elf::SHN_UNDEF;

            if is_defined {
                if bind == elf::STB_GLOBAL {
                    if self.find_symbol_by_name(&name).is_some() {
                        eprintln!("minilink: multiple definition of '{}' (in {} und vorheriger Datei)", name, filename);
                        process::exit(1);
                    }
                }
                let section_id = if ty == elf::STT_SECTION {
                    sec_id
                } else {
                    self.shndx_map[file_index].get(es.st_shndx as usize).copied().flatten()
                };
                let keep_id2 = if ty == elf::STT_SECTION { keep_id } else { None };
                self.symbols.push(Sym {
                    name,
                    file_index,
                    orig_symidx: i,
                    section_id,
                    keep_id: keep_id2,
                    value: es.st_value,
                    is_global: bind == elf::STB_GLOBAL,
                    is_defined: true,
                    final_address: 0,
                    resolved: false,
                });
            } else {
                self.symbols.push(Sym {
                    name,
                    file_index,
                    orig_symidx: i,
                    section_id: None,
                    keep_id: None,
                    value: 0,
                    is_global: true,
                    is_defined: false,
                    final_address: 0,
                    resolved: false,
                });
            }
        }
    }

    fn dump_symbols(&self, path: &str) {
        let mut s = String::new();
        s.push_str(&format!("# minilink Symboltabelle ({} Eintraege, vor Placement)\n", self.symbols.len()));
        s.push_str(&format!(
            "# {:<32} {:<9} {:<7} {:<18} {:<16} {}\n",
            "Name", "Bindung", "Status", "Section", "Offset", "Quelldatei"
        ));
        for sym in &self.symbols {
            let bind = if sym.is_global { "GLOBAL" } else { "LOCAL" };
            let status = if sym.is_defined { "DEF" } else { "UNDEF" };
            let section = sym.section_id.map(|i| self.sections[i].name.as_str()).unwrap_or("-");
            s.push_str(&format!(
                "  {:<32} {:<9} {:<7} {:<18} 0x{:014x}   {}\n",
                sym.name, bind, status, section, sym.value, self.files[sym.file_index].filename
            ));
        }
        if fs::write(path, s).is_ok() {
            println!("minilink: {} Symbol(e) nach {} geschrieben", self.symbols.len(), path);
        }
    }

    fn resolve_all_symbols(&self) {
        self.dump_symbols("symbols.txt");

        let mut errors = 0;
        for s in &self.symbols {
            if s.is_defined {
                continue;
            }
            if self.find_symbol_by_name(&s.name).is_none() {
                eprintln!("minilink: undefined reference to '{}'", s.name);
                errors += 1;
            }
        }
        if errors > 0 {
            eprintln!("minilink: {} undefined symbol(s), Linking abgebrochen", errors);
            process::exit(1);
        }
    }

    // --- [3] Relocations importieren --------------------------------------

    fn import_relocations(&mut self, file_index: usize) {
        let raw = self.files[file_index].raw.clone();
        let shdrs = self.files[file_index].shdrs.clone();

        for sh in &shdrs {
            if sh.sh_type != elf::SHT_RELA {
                continue;
            }
            let target_shndx = sh.sh_info as usize;
            let target_section_id = self.shndx_map[file_index].get(target_shndx).copied().flatten();
            let target_keep_id = self.keep_map[file_index].get(target_shndx).copied().flatten();
            if target_section_id.is_none() && target_keep_id.is_none() {
                continue; // Ziel-Section wurde verworfen
            }

            let count = (sh.sh_size / elf::Rela::SIZE as u64) as usize;
            for r in 0..count {
                let off = sh.sh_offset as usize + r * elf::Rela::SIZE;
                let rela = elf::Rela::parse(&raw[off..off + elf::Rela::SIZE]);
                let elf_symidx = rela.r_sym() as usize;
                let sym_index = match self.find_symbol_by_origin(file_index, elf_symidx) {
                    Some(x) => x,
                    None => {
                        eprintln!("minilink: interner Fehler: Relocation-Symbol nicht gefunden");
                        process::exit(1);
                    }
                };
                let keep_base = if let Some(k) = target_keep_id {
                    *self.keep[k].chunk_off.get(&file_index).unwrap()
                } else {
                    0
                };
                self.relocs.push(Reloc {
                    section_id: target_section_id,
                    keep_id: target_keep_id,
                    keep_base,
                    offset: rela.r_offset,
                    sym_index,
                    rtype: rela.r_type(),
                    addend: rela.r_addend,
                });
            }
        }
    }

    // --- Linkerscripts ------------------------------------------------------

    fn load_ldl_script(&mut self, path: &str) {
        let content = fs::read_to_string(path).unwrap_or_else(|e| {
            eprintln!("{}: {}", path, e);
            process::exit(1);
        });

        let mut hits = 0;
        let mut in_block_comment = false;
        for (lineno0, line) in content.lines().enumerate() {
            let lineno = lineno0 + 1;
            let bytes = line.as_bytes();
            let mut clean = String::new();
            let mut i = 0;
            while i < bytes.len() {
                if in_block_comment {
                    if bytes[i] == b'*' && i + 1 < bytes.len() && bytes[i + 1] == b'/' {
                        in_block_comment = false;
                        i += 2;
                        continue;
                    }
                    i += 1;
                    continue;
                }
                if bytes[i] == b'/' && i + 1 < bytes.len() && bytes[i + 1] == b'*' {
                    in_block_comment = true;
                    i += 2;
                    continue;
                }
                if bytes[i] == b'/' && i + 1 < bytes.len() && bytes[i + 1] == b'/' {
                    break;
                }
                clean.push(bytes[i] as char);
                i += 1;
            }

            let mut it = clean.split_whitespace();
            if it.next() != Some("#define") {
                continue;
            }
            let name = match it.next() {
                Some(n) => n,
                None => continue,
            };
            let value = match it.next() {
                Some(v) => v,
                None => continue,
            };

            let vtrim = match value.find(|c| matches!(c, 'u' | 'U' | 'l' | 'L')) {
                Some(pos) => &value[..pos],
                None => value,
            };
            let v = parse_c_uint(vtrim);

            if name == "BASE_ADDR" {
                self.base_addr = v;
                hits += 1;
            } else if name == "PAGE_SIZE" {
                self.page_size = v;
                hits += 1;
            } else {
                eprintln!("minilink: {}:{}: unbekannter Schluessel '{}' (ignoriert)", path, lineno, name);
            }
        }

        if self.base_addr == 0 {
            eprintln!("minilink: {}: BASE_ADDR fehlt (erwartet: #define BASE_ADDR <adresse>)", path);
            process::exit(1);
        }
        if self.page_size < 0x1000 || (self.page_size & (self.page_size - 1)) != 0 {
            eprintln!("minilink: {}: PAGE_SIZE (0x{:x}) muss eine Zweierpotenz >= 0x1000 sein", path, self.page_size);
            process::exit(1);
        }
        if self.base_addr & (self.page_size - 1) != 0 {
            eprintln!(
                "minilink: {}: BASE_ADDR (0x{:x}) ist nicht page-aligned (PAGE_SIZE 0x{:x}) -- das erzeugte ELF waere nicht ladbar",
                path, self.base_addr, self.page_size
            );
            process::exit(1);
        }

        println!(
            "minilink: LDL-Script {} geladen ({} Wert(e): BASE_ADDR=0x{:x} PAGE_SIZE=0x{:x})",
            path, hits, self.base_addr, self.page_size
        );
    }

    fn load_lsl_script(&mut self, path: &str) {
        let buf = fs::read(path).unwrap_or_else(|e| {
            eprintln!("{}: {}", path, e);
            process::exit(1);
        });
        let result = lsl::parse(&buf);
        self.lsl_mem = result.mems;
        self.lsl_grp = result.groups;

        let mut rom_i = None;
        let mut ram_i = None;
        for (i, m) in self.lsl_mem.iter().enumerate() {
            if !m.is_ram && rom_i.is_none() {
                rom_i = Some(i);
            }
            if m.is_ram && ram_i.is_none() {
                ram_i = Some(i);
            }
        }
        let rom_i = rom_i.unwrap_or_else(|| {
            eprintln!("minilink: {}: keine 'memory' mit type=rom gefunden", path);
            process::exit(1);
        });

        self.base_addr = self.lsl_mem[rom_i].addr;
        self.rw_base = ram_i.map(|i| self.lsl_mem[i].addr).unwrap_or(0);
        self.page_size = 0x1000; // LSL kennt keine Page-Size -> ELF-Standardwert
        self.use_lsl = true;

        if self.base_addr == 0 || (self.base_addr & (self.page_size - 1)) != 0 {
            eprintln!(
                "minilink: {}: rom-Adresse 0x{:x} fehlt oder nicht 0x{:x}-aligned",
                path, self.base_addr, self.page_size
            );
            process::exit(1);
        }
        if self.rw_base != 0 && (self.rw_base & (self.page_size - 1)) != 0 {
            eprintln!("minilink: {}: ram-Adresse 0x{:x} nicht 0x{:x}-aligned", path, self.rw_base, self.page_size);
            process::exit(1);
        }

        println!(
            "minilink: LSL {} geladen: {} memory-Region(en), {} group(s); rom@0x{:x} ram@0x{:x}",
            path,
            self.lsl_mem.len(),
            self.lsl_grp.len(),
            self.base_addr,
            self.rw_base
        );
    }

    // --- [4] Placement / Locator --------------------------------------------

    fn place_run(&mut self, idx: &[usize], seg_index: usize, cursor: &mut u64) {
        for &i in idx {
            let a = self.sections[i].align;
            *cursor = align_up(*cursor, a);
            self.sections[i].seg_index = seg_index;
            self.sections[i].merged_offset = *cursor;
            *cursor += self.sections[i].size;
        }
    }

    fn finish_segment(&mut self, layout: &mut Layout, mut o: OutSeg, prog: &[usize], bss: &[usize], file_cursor: &mut u64) {
        let si = layout.seg.len();
        o.file_off = if si == 0 { 0 } else { align_up(*file_cursor, self.page_size) };
        let mut off = if o.has_header { self.page_size } else { 0 };
        self.place_run(prog, si, &mut off);
        o.file_size = off;
        self.place_run(bss, si, &mut off);
        o.mem_size = off;
        *file_cursor = o.file_off + o.file_size;
        layout.seg.push(o);
    }

    /// Default-Layout (Script per -T): .text -> Segment 0 (R-X);
    /// .rodata/.data/.bss -> Segment 1 (RW), an der naechsten Page-Grenze.
    fn place_sections_default(&mut self) -> Layout {
        let txt: Vec<usize> = (0..self.sections.len()).filter(|&i| self.sections[i].kind == SectionKind::Text).collect();
        let mut rwp: Vec<usize> = (0..self.sections.len()).filter(|&i| self.sections[i].kind == SectionKind::Rodata).collect();
        rwp.extend((0..self.sections.len()).filter(|&i| self.sections[i].kind == SectionKind::Data));
        let bss: Vec<usize> = (0..self.sections.len()).filter(|&i| self.sections[i].kind == SectionKind::Bss).collect();

        let mut layout = Layout { seg: Vec::new() };
        let mut fc = 0u64;
        let s0 = OutSeg {
            mem_index: None,
            is_rw: false,
            has_header: true,
            vaddr: self.base_addr,
            file_off: 0,
            file_size: 0,
            mem_size: 0,
        };
        self.finish_segment(&mut layout, s0, &txt, &[], &mut fc);

        let seg1_vaddr = align_up(self.base_addr + layout.seg[0].file_size, self.page_size);
        let s1 = OutSeg {
            mem_index: None,
            is_rw: true,
            has_header: false,
            vaddr: seg1_vaddr,
            file_off: 0,
            file_size: 0,
            mem_size: 0,
        };
        self.finish_segment(&mut layout, s1, &rwp, &bss, &mut fc);

        check_overlaps(&layout);
        layout
    }

    /// LSL-Layout (--lsl): jede genutzte memory-Region wird ein eigenes PT_LOAD.
    fn place_sections_lsl(&mut self) -> Layout {
        let n_mem = self.lsl_mem.len();
        let mut prog: Vec<Vec<usize>> = vec![Vec::new(); n_mem];
        let mut bssl: Vec<Vec<usize>> = vec![Vec::new(); n_mem];
        let mut claimed = vec![false; self.sections.len()];

        let mut rom0 = None;
        let mut ram0 = None;
        for (m, mem) in self.lsl_mem.iter().enumerate() {
            if !mem.is_ram && rom0.is_none() {
                rom0 = Some(m);
            }
            if mem.is_ram && ram0.is_none() {
                ram0 = Some(m);
            }
        }
        let rom0 = rom0.unwrap_or_else(|| {
            eprintln!("minilink: LSL: keine rom-Region");
            process::exit(1);
        });

        let groups = self.lsl_grp.clone();
        for g in &groups {
            let reg = if !g.run_mem.is_empty() {
                self.lsl_mem.iter().position(|m| m.id == g.run_mem).unwrap_or(rom0)
            } else {
                rom0
            };
            for pat in &g.sel {
                for i in 0..self.sections.len() {
                    if claimed[i] || !lsl::sel_match(pat, &self.sections[i].name) {
                        continue;
                    }
                    claimed[i] = true;
                    if self.sections[i].kind == SectionKind::Bss {
                        bssl[reg].push(i);
                    } else {
                        prog[reg].push(i);
                    }
                }
            }
        }
        for i in 0..self.sections.len() {
            if claimed[i] {
                continue;
            }
            let isdata = matches!(self.sections[i].kind, SectionKind::Data | SectionKind::Bss);
            let reg = if isdata { ram0.unwrap_or(rom0) } else { rom0 };
            eprintln!(
                "minilink: LSL: Section '{}' von keiner group erfasst -- nach '{}'",
                self.sections[i].name, self.lsl_mem[reg].id
            );
            if self.sections[i].kind == SectionKind::Bss {
                bssl[reg].push(i);
            } else {
                prog[reg].push(i);
            }
        }

        // Regionen materialisieren: rom0 immer, sonst nur mit Inhalt.
        let mut order = vec![rom0];
        for m in 0..n_mem {
            if m != rom0 && (!prog[m].is_empty() || !bssl[m].is_empty()) {
                order.push(m);
            }
        }
        if order.len() > 1 {
            let addr = |m: usize| self.lsl_mem[m].addr;
            order[1..].sort_by_key(|&m| addr(m));
        }

        let mut layout = Layout { seg: Vec::new() };
        let mut fc = 0u64;
        for (k, &m) in order.iter().enumerate() {
            let o = OutSeg {
                mem_index: Some(m),
                is_rw: self.lsl_mem[m].is_ram,
                has_header: k == 0,
                vaddr: self.lsl_mem[m].addr,
                file_off: 0,
                file_size: 0,
                mem_size: 0,
            };
            self.finish_segment(&mut layout, o, &prog[m], &bssl[m], &mut fc);
        }
        check_overlaps(&layout);
        layout
    }

    /// Nachdem Sections platziert sind: jedem Symbol seine finale Adresse geben.
    fn assign_symbol_addresses(&mut self, layout: &Layout) {
        for i in 0..self.symbols.len() {
            if !self.symbols[i].is_defined {
                continue;
            }
            if let Some(keep_id) = self.symbols[i].keep_id {
                let fi = self.symbols[i].file_index;
                let base = *self.keep[keep_id].chunk_off.get(&fi).unwrap();
                self.symbols[i].final_address = base + self.symbols[i].value;
                self.symbols[i].resolved = true;
                continue;
            }
            let sec_id = match self.symbols[i].section_id {
                Some(x) => x,
                None => {
                    self.symbols[i].resolved = true;
                    self.symbols[i].final_address = 0;
                    continue;
                }
            };
            let sec = &self.sections[sec_id];
            self.symbols[i].final_address = layout.seg[sec.seg_index].vaddr + sec.merged_offset + self.symbols[i].value;
            self.symbols[i].resolved = true;
        }

        // Undefinierte Symbole auf die jetzt final adressierte Definition mappen.
        for i in 0..self.symbols.len() {
            if self.symbols[i].is_defined {
                continue;
            }
            let name = self.symbols[i].name.clone();
            let def = self.find_symbol_by_name(&name).unwrap();
            let addr = self.symbols[def].final_address;
            self.symbols[i].final_address = addr;
            self.symbols[i].resolved = true;
        }
    }

    // --- [5] Relocation-Engine ----------------------------------------------

    fn apply_relocations(&mut self, layout: &Layout) {
        let relocs = self.relocs.clone();
        for r in &relocs {
            let sym = &self.symbols[r.sym_index];
            let s = sym.final_address;
            let a = r.addend;

            if let Some(keep_id) = r.keep_id {
                let at = (r.keep_base + r.offset) as usize;
                patch_reloc(&mut self.keep[keep_id].data[at..], r.rtype, s, a, 0, &sym.name);
                continue;
            }

            let sec_id = r.section_id.unwrap();
            let (kind, seg_index, merged_offset) = {
                let sec = &self.sections[sec_id];
                (sec.kind, sec.seg_index, sec.merged_offset)
            };
            if kind == SectionKind::Bss {
                continue; // .bss hat keinen File-Inhalt
            }
            let p = layout.seg[seg_index].vaddr + merged_offset + r.offset;
            let data = self.sections[sec_id].data.as_mut().unwrap();
            patch_reloc(&mut data[r.offset as usize..], r.rtype, s, a, p, &sym.name);
        }
    }

    // --- [6] Output-Writer ----------------------------------------------------

    fn build_segment_image(&self, seg_index: usize, span: u64, prefix: Option<&[u8]>) -> Vec<u8> {
        let mut img = vec![0u8; span as usize];
        if let Some(p) = prefix {
            img[..p.len()].copy_from_slice(p);
        }
        for s in &self.sections {
            if s.seg_index != seg_index {
                continue;
            }
            let data = match &s.data {
                Some(d) => d,
                None => continue, // .bss
            };
            if s.merged_offset + s.size > span {
                eprintln!(
                    "minilink: interner Fehler: Section '{}' passt nicht ins Segment-Image (0x{:x}+0x{:x} > 0x{:x})",
                    s.name, s.merged_offset, s.size, span
                );
                process::exit(1);
            }
            img[s.merged_offset as usize..(s.merged_offset + s.size) as usize].copy_from_slice(data);
        }
        img
    }

    fn write_output(&self, path: &str, layout: &Layout, entry_addr: u64) {
        let mut eh = elf::Ehdr {
            e_ident: [0u8; 16],
            e_type: elf::ET_EXEC,
            e_machine: elf::EM_X86_64,
            e_version: elf::EV_CURRENT as u32,
            e_entry: entry_addr,
            e_phoff: elf::Ehdr::SIZE as u64,
            e_shoff: 0,
            e_flags: 0,
            e_ehsize: elf::Ehdr::SIZE as u16,
            e_phentsize: elf::Phdr::SIZE as u16,
            e_phnum: layout.seg.len() as u16,
            e_shentsize: 0,
            e_shnum: 0,
            e_shstrndx: elf::SHN_UNDEF,
        };
        eh.e_ident[0..4].copy_from_slice(elf::ELFMAG);
        eh.e_ident[4] = elf::ELFCLASS64;
        eh.e_ident[5] = elf::ELFDATA2LSB;
        eh.e_ident[6] = elf::EV_CURRENT;
        eh.e_ident[7] = elf::ELFOSABI_SYSV;

        let hdr_len = elf::Ehdr::SIZE + layout.seg.len() * elf::Phdr::SIZE;
        if hdr_len as u64 > self.page_size {
            eprintln!(
                "minilink: {} Segmente -> Header (0x{:x}) > PAGE_SIZE (0x{:x})",
                layout.seg.len(),
                hdr_len,
                self.page_size
            );
            process::exit(1);
        }

        let mut hdr = Vec::with_capacity(hdr_len);
        eh.write(&mut hdr);
        for o in &layout.seg {
            let ph = elf::Phdr {
                p_type: elf::PT_LOAD,
                p_flags: if o.is_rw { elf::PF_R | elf::PF_W } else { elf::PF_R | elf::PF_X },
                p_offset: o.file_off,
                p_vaddr: o.vaddr,
                p_paddr: o.vaddr,
                p_filesz: o.file_size,
                p_memsz: o.mem_size,
                p_align: self.page_size,
            };
            ph.write(&mut hdr);
        }

        let mut out: Vec<u8> = Vec::new();
        for (k, o) in layout.seg.iter().enumerate() {
            while (out.len() as u64) < o.file_off {
                out.push(0);
            }
            let prefix = if k == 0 { Some(hdr.as_slice()) } else { None };
            let img = self.build_segment_image(k, o.file_size, prefix);
            out.extend_from_slice(&img);
        }
        // .bss wird NICHT in die Datei geschrieben (SHT_NOBITS-Prinzip)

        if !self.debug_mode {
            write_exec(path, &out);
            return;
        }

        self.write_debug_sections(path, layout, &mut out, eh);
    }

    fn write_debug_sections(&self, path: &str, layout: &Layout, out: &mut Vec<u8>, mut eh: elf::Ehdr) {
        struct OShdr {
            name: String,
            sh_type: u32,
            sh_flags_lo: u64,
            addr: u64,
            off: u64,
            size: u64,
            align: u64,
            entsize: u64,
            link: u32,
            info: u32,
        }
        impl Default for OShdr {
            fn default() -> Self {
                OShdr {
                    name: String::new(),
                    sh_type: 0,
                    sh_flags_lo: 0,
                    addr: 0,
                    off: 0,
                    size: 0,
                    align: 0,
                    entsize: 0,
                    link: 0,
                    info: 0,
                }
            }
        }

        let mut osec: Vec<OShdr> = vec![OShdr::default()]; // [0] SHT_NULL
        let mut sec_sht = vec![0usize; self.sections.len()];

        const KINDS: [SectionKind; 4] = [SectionKind::Text, SectionKind::Rodata, SectionKind::Data, SectionKind::Bss];
        for seg in 0..layout.seg.len() {
            for &kd in &KINDS {
                let mut any = false;
                let mut lo = 0u64;
                let mut hi = 0u64;
                let mut al = 1u64;
                for s in &self.sections {
                    if s.seg_index != seg || s.kind != kd {
                        continue;
                    }
                    if !any || s.merged_offset < lo {
                        lo = s.merged_offset;
                    }
                    if !any || s.merged_offset + s.size > hi {
                        hi = s.merged_offset + s.size;
                    }
                    if s.align > al {
                        al = s.align;
                    }
                    any = true;
                }
                if !any {
                    continue;
                }
                let idx = osec.len();
                osec.push(OShdr {
                    name: kind_name(kd).to_string(),
                    sh_type: if kd == SectionKind::Bss { elf::SHT_NOBITS } else { elf::SHT_PROGBITS },
                    sh_flags_lo: elf::SHF_ALLOC
                        | if kd == SectionKind::Text { elf::SHF_EXECINSTR } else { 0 }
                        | if kd == SectionKind::Data || kd == SectionKind::Bss { elf::SHF_WRITE } else { 0 },
                    addr: layout.seg[seg].vaddr + lo,
                    off: layout.seg[seg].file_off + lo,
                    size: hi - lo,
                    align: al,
                    entsize: 0,
                    link: 0,
                    info: 0,
                });
                for (i, s) in self.sections.iter().enumerate() {
                    if s.seg_index == seg && s.kind == kd {
                        sec_sht[i] = idx;
                    }
                }
            }
        }

        // .debug_*-Sections hinter die Segmente schreiben
        for k in &self.keep {
            let a = if k.align > 0 { k.align } else { 1 };
            while (out.len() as u64) % a != 0 {
                out.push(0);
            }
            let off = out.len() as u64;
            out.extend_from_slice(&k.data);
            osec.push(OShdr {
                name: k.name.clone(),
                sh_type: k.sh_type,
                sh_flags_lo: 0,
                addr: 0,
                off,
                size: k.data.len() as u64,
                align: a,
                entsize: 0,
                link: 0,
                info: 0,
            });
        }

        // .strtab + .symtab: alle definierten Programm-Symbole (keine
        // synthetischen <section:*>), lokale vor globalen.
        let mut strtab: Vec<u8> = vec![0];
        let mut syms: Vec<elf::Sym> = vec![elf::Sym::default()]; // [0] = STN_UNDEF
        let mut first_global: Option<usize> = None;
        for pass in 0..2 {
            for s in &self.symbols {
                if !s.is_defined || s.section_id.is_none() {
                    continue;
                }
                if s.name.starts_with("<section:") {
                    continue;
                }
                if (pass == 0) == s.is_global {
                    continue;
                }
                if pass == 1 && first_global.is_none() {
                    first_global = Some(syms.len());
                }
                let stt = if self.sections[s.section_id.unwrap()].kind == SectionKind::Text {
                    elf::STT_FUNC
                } else {
                    elf::STT_OBJECT
                };
                let name_off = strtab_add(&mut strtab, &s.name);
                syms.push(elf::Sym {
                    st_name: name_off,
                    st_info: elf::st_info(if s.is_global { elf::STB_GLOBAL } else { elf::STB_LOCAL }, stt),
                    st_other: 0,
                    st_shndx: sec_sht[s.section_id.unwrap()] as u16,
                    st_value: s.final_address,
                    st_size: 0,
                });
            }
        }
        let first_global = first_global.unwrap_or(syms.len());

        while out.len() % 8 != 0 {
            out.push(0);
        }
        let symtab_off = out.len() as u64;
        for sy in &syms {
            sy.write(out);
        }

        let strtab_off = out.len() as u64;
        out.extend_from_slice(&strtab);

        let idx_symtab = osec.len();
        let idx_strtab = idx_symtab + 1;
        let idx_shstr = idx_symtab + 2;
        let n_osec = idx_shstr + 1;

        // .shstrtab: alle bisher vergebenen Section-Namen + die drei folgenden
        let mut shstr: Vec<u8> = vec![0];
        let mut name_off = vec![0u32; n_osec];
        for (i, o) in osec.iter().enumerate().skip(1) {
            name_off[i] = strtab_add(&mut shstr, &o.name);
        }
        name_off[idx_symtab] = strtab_add(&mut shstr, ".symtab");
        name_off[idx_strtab] = strtab_add(&mut shstr, ".strtab");
        name_off[idx_shstr] = strtab_add(&mut shstr, ".shstrtab");

        let shstr_off = out.len() as u64;
        out.extend_from_slice(&shstr);

        while out.len() % 8 != 0 {
            out.push(0);
        }
        let sht_off = out.len() as u64;

        for i in 0..n_osec {
            let mut sh = elf::Shdr::default();
            if i == 0 {
                // SHT_NULL, alles 0
            } else if i == idx_symtab {
                sh.sh_name = name_off[i];
                sh.sh_type = elf::SHT_SYMTAB;
                sh.sh_offset = symtab_off;
                sh.sh_size = syms.len() as u64 * elf::Sym::SIZE as u64;
                sh.sh_link = idx_strtab as u32;
                sh.sh_info = first_global as u32;
                sh.sh_addralign = 8;
                sh.sh_entsize = elf::Sym::SIZE as u64;
            } else if i == idx_strtab {
                sh.sh_name = name_off[i];
                sh.sh_type = elf::SHT_STRTAB;
                sh.sh_offset = strtab_off;
                sh.sh_size = strtab.len() as u64;
                sh.sh_addralign = 1;
            } else if i == idx_shstr {
                sh.sh_name = name_off[i];
                sh.sh_type = elf::SHT_STRTAB;
                sh.sh_offset = shstr_off;
                sh.sh_size = shstr.len() as u64;
                sh.sh_addralign = 1;
            } else {
                let o = &osec[i];
                sh.sh_name = name_off[i];
                sh.sh_type = o.sh_type;
                sh.sh_flags = o.sh_flags_lo;
                sh.sh_addr = o.addr;
                sh.sh_offset = o.off;
                sh.sh_size = o.size;
                sh.sh_addralign = o.align;
                sh.sh_entsize = o.entsize;
                sh.sh_link = o.link;
                sh.sh_info = o.info;
            }
            sh.write(out);
        }

        eh.e_shoff = sht_off;
        eh.e_shentsize = elf::Shdr::SIZE as u16;
        eh.e_shnum = n_osec as u16;
        eh.e_shstrndx = idx_shstr as u16;
        let mut new_hdr = Vec::new();
        eh.write(&mut new_hdr);
        out[0..elf::Ehdr::SIZE].copy_from_slice(&new_hdr);

        write_exec(path, out);
    }

    // --- Debug-Ausgabe: MAP-File-artige Uebersicht --------------------------

    fn sec_vaddr(&self, layout: &Layout, s: &InSection) -> u64 {
        layout.seg[s.seg_index].vaddr + s.merged_offset
    }

    fn emit_map(&self, out: &mut dyn std::io::Write, layout: &Layout, entry_addr: u64, script: &str) {
        let seg_of = |seg_index: usize| if layout.seg[seg_index].is_rw { "RW" } else { "R-X" };

        writeln!(out, "=== minilink MAP ===").unwrap();
        writeln!(out, "Script     : {}", script).unwrap();
        writeln!(out, "Entry      : _start @ 0x{:08x}", entry_addr).unwrap();
        writeln!(out, "Layout     : BASE_ADDR=0x{:08x}  PAGE_SIZE=0x{:x}", self.base_addr, self.page_size).unwrap();

        if self.use_lsl && !self.lsl_mem.is_empty() {
            writeln!(out, "\nMemory-Regionen (LSL)").unwrap();
            writeln!(out, "  {:<10} {:<5} {:<12} {:>10}  {}", "Name", "Typ", "Adresse", "Groesse", "genutzt").unwrap();
            for (i, m) in self.lsl_mem.iter().enumerate() {
                let used = layout.seg.iter().any(|s| s.mem_index == Some(i));
                writeln!(
                    out,
                    "  {:<10} {:<5} 0x{:08x} {:>8} K  {}",
                    m.id,
                    if m.is_ram { "ram" } else { "rom" },
                    m.addr,
                    m.size >> 10,
                    if used { "ja" } else { "-" }
                )
                .unwrap();
            }
        }

        writeln!(out, "\nSegmente (PT_LOAD)").unwrap();
        writeln!(
            out,
            "  {:<3} {:<5} {:<12} {:<12} {:<10} {:<9} {:<9} {}",
            "#", "Flags", "VirtAddr", "EndAddr", "FileOff", "FileSz", "MemSz", "Region"
        )
        .unwrap();
        for (k, o) in layout.seg.iter().enumerate() {
            let rg = o.mem_index.map(|m| self.lsl_mem[m].id.as_str()).unwrap_or("-");
            writeln!(
                out,
                "  {:<3} {:<5} 0x{:08x}   0x{:08x}   0x{:08x} 0x{:07x} 0x{:07x} {}",
                k,
                if o.is_rw { "RW" } else { "R-X" },
                o.vaddr,
                o.vaddr + o.mem_size,
                o.file_off,
                o.file_size,
                o.mem_size,
                rg
            )
            .unwrap();
        }

        writeln!(out, "\nSektionen (nach Adresse; 0-Byte-Sektionen ausgelassen)").unwrap();
        writeln!(
            out,
            "  {:<4} {:<16} {:<12} {:<12} {:<9} {:<6} {}",
            "Seg", "Section", "VirtAddr", "EndAddr", "Size", "Align", "Quelle"
        )
        .unwrap();
        let mut sord: Vec<usize> = (0..self.sections.len()).filter(|&i| self.sections[i].size != 0).collect();
        let skipped = self.sections.len() - sord.len();
        sord.sort_by_key(|&i| self.sec_vaddr(layout, &self.sections[i]));
        for &i in &sord {
            let s = &self.sections[i];
            let va = self.sec_vaddr(layout, s);
            let nm = format!("{}{}", kind_name(s.kind), if s.kind == SectionKind::Bss { " (NOBITS)" } else { "" });
            writeln!(
                out,
                "  {:<4} {:<16} 0x{:08x}   0x{:08x}   0x{:07x} {:<6} {}",
                seg_of(s.seg_index),
                nm,
                va,
                va + s.size,
                s.size,
                s.align,
                self.files[s.file_index].filename
            )
            .unwrap();
        }
        if skipped > 0 {
            writeln!(out, "  ({} leere Section(s) ausgelassen)", skipped).unwrap();
        }

        writeln!(out, "\nSymbole (definiert, nach Adresse)").unwrap();
        writeln!(out, "  {:<12} {:<6} {:<4} {:<10} {:<22} {}", "VirtAddr", "Bind", "Seg", "Section", "Name", "Quelle").unwrap();
        let mut order: Vec<usize> = (0..self.symbols.len())
            .filter(|&i| self.symbols[i].is_defined && !self.symbols[i].name.starts_with("<section:"))
            .collect();
        order.sort_by(|&a, &b| {
            let sa = &self.symbols[a];
            let sb = &self.symbols[b];
            sa.final_address.cmp(&sb.final_address).then_with(|| sa.name.cmp(&sb.name))
        });
        for &i in &order {
            let s = &self.symbols[i];
            let (seg, sec) = match s.section_id {
                Some(sid) => (seg_of(self.sections[sid].seg_index), kind_name(self.sections[sid].kind)),
                None => ("-", "(abs)"),
            };
            writeln!(
                out,
                "  0x{:08x}   {:<6} {:<4} {:<10} {:<22} {}",
                s.final_address,
                if s.is_global { "GLOBAL" } else { "LOCAL" },
                seg,
                sec,
                s.name,
                self.files[s.file_index].filename
            )
            .unwrap();
        }
        writeln!(out, "=====================").unwrap();
    }

    fn print_map(&self, layout: &Layout, entry_addr: u64, script: &str, output_path: &str) {
        println!();
        let mut stdout = std::io::stdout();
        self.emit_map(&mut stdout, layout, entry_addr, script);

        let map_path = format!("{}.map", output_path);
        match fs::File::create(&map_path) {
            Ok(mut f) => {
                self.emit_map(&mut f, layout, entry_addr, script);
                println!("minilink: MAP zusaetzlich geschrieben nach {}\n", map_path);
            }
            Err(e) => {
                eprintln!("{}: {}", map_path, e);
                println!();
            }
        }
    }

    fn dump_files(&self, file_indices: &[usize]) {
        let mut s = String::new();
        s.push_str(&format!("# minilink: {} Eingabedatei(en)\n", file_indices.len()));
        s.push_str(&format!("# {:<10} {}\n", "file_index", "Dateiname"));
        for &fi in file_indices {
            s.push_str(&format!("  {:<10} {}\n", fi, self.files[fi].filename));
        }
        if fs::write("files.txt", s).is_ok() {
            println!("minilink: {} Datei-Index/Indizes nach files.txt geschrieben", file_indices.len());
        }
    }

    fn dump_file_indices(&self, file_indices: &[usize]) {
        let mut s = String::new();
        for &fi in file_indices {
            s.push_str(&format!("{}\n", fi));
        }
        if fs::write("file_indices.txt", s).is_ok() {
            println!("minilink: {} file_index-Wert(e) nach file_indices.txt geschrieben", file_indices.len());
        }
    }
}

fn patch_reloc(dst: &mut [u8], rtype: u32, s: u64, a: i64, p: u64, symname: &str) {
    match rtype {
        elf::R_X86_64_PC32 | elf::R_X86_64_PLT32 => {
            // Bei uns gibt es kein PLT (kein dynamisches Linken) ->
            // PLT32 wird wie ein ganz normaler PC-relativer 32-Bit-Verweis behandelt.
            let value = s as i64 + a - p as i64;
            let v32 = value as i32;
            if value != v32 as i64 {
                eprintln!("minilink: PC32-Relocation ausserhalb des 32-Bit-Bereichs fuer '{}'", symname);
                process::exit(1);
            }
            dst[0..4].copy_from_slice(&v32.to_le_bytes());
        }
        elf::R_X86_64_64 => {
            let value = s.wrapping_add(a as u64);
            dst[0..8].copy_from_slice(&value.to_le_bytes());
        }
        elf::R_X86_64_32S | elf::R_X86_64_32 => {
            let value = s as i64 + a;
            let v32 = value as i32;
            dst[0..4].copy_from_slice(&v32.to_le_bytes());
        }
        _ => {
            eprintln!("minilink: nicht unterstuetzter Relocation-Typ {}", rtype);
            process::exit(1);
        }
    }
}

/// Fuegt name (falls noch nicht vorhanden) an eine Stringtabelle an und
/// gibt den Offset zurueck. buf[0] ist stets '\0'.
fn strtab_add(buf: &mut Vec<u8>, name: &str) -> u32 {
    let mut p = 0usize;
    while p < buf.len() {
        let end = buf[p..].iter().position(|&b| b == 0).map(|o| p + o).unwrap_or(buf.len());
        if &buf[p..end] == name.as_bytes() {
            return p as u32;
        }
        p = end + 1;
    }
    let at = buf.len() as u32;
    buf.extend_from_slice(name.as_bytes());
    buf.push(0);
    at
}

fn write_exec(path: &str, data: &[u8]) {
    if let Err(e) = fs::write(path, data) {
        eprintln!("{}: {}", path, e);
        process::exit(1);
    }
    if let Ok(meta) = fs::metadata(path) {
        let mut perm = meta.permissions();
        perm.set_mode(0o755);
        let _ = fs::set_permissions(path, perm);
    }
}

// -------------------------------------------------------------------------
// Driver
// -------------------------------------------------------------------------

fn usage(prog: &str) -> ! {
    eprintln!("usage: {} (-T <script.ldl> | --lsl <script.lsl>) [--debug] <input1.o> [...] -o <output>", prog);
    process::exit(1);
}

fn main() {
    let argv: Vec<String> = std::env::args().collect();
    let prog = argv.get(0).cloned().unwrap_or_else(|| "minilink".to_string());

    let mut output_path: Option<String> = None;
    let mut ldl_path: Option<String> = None;
    let mut lsl_path: Option<String> = None;
    let mut debug_mode = false;
    let mut inputs: Vec<String> = Vec::new();

    let mut i = 1;
    while i < argv.len() {
        let a = argv[i].clone();
        if a == "-o" {
            i += 1;
            if i >= argv.len() {
                eprintln!("minilink: -o braucht ein Argument");
                process::exit(1);
            }
            output_path = Some(argv[i].clone());
        } else if a == "-T" {
            i += 1;
            if i >= argv.len() {
                eprintln!("minilink: -T braucht ein Argument");
                process::exit(1);
            }
            ldl_path = Some(argv[i].clone());
        } else if let Some(rest) = a.strip_prefix("-T") {
            if !rest.is_empty() {
                ldl_path = Some(rest.to_string());
            }
        } else if a == "--lsl" {
            i += 1;
            if i >= argv.len() {
                eprintln!("minilink: --lsl braucht ein Argument");
                process::exit(1);
            }
            lsl_path = Some(argv[i].clone());
        } else if let Some(v) = a.strip_prefix("--lsl=") {
            lsl_path = Some(v.to_string());
        } else if a == "--debug" || a == "-g" {
            debug_mode = true;
        } else if a.starts_with('-') {
            eprintln!("minilink: unbekannte Option '{}'", a);
            process::exit(1);
        } else {
            inputs.push(a);
        }
        i += 1;
    }

    if output_path.is_none() || inputs.is_empty() {
        usage(&prog);
    }
    let output_path = output_path.unwrap();

    if ldl_path.is_some() && lsl_path.is_some() {
        eprintln!("minilink: -T und --lsl schliessen sich aus");
        process::exit(1);
    }
    if ldl_path.is_none() && lsl_path.is_none() {
        eprintln!(
            "minilink: kein Linkerscript angegeben -- entweder\n  -T <script.ldl>     (#define BASE_ADDR / PAGE_SIZE, z.B. test/default.ldl)\n  --lsl <script.lsl>  (vereinfachtes TASKING-LSL, z.B. test/tc27x.lsl)"
        );
        process::exit(1);
    }

    let mut linker = Linker::new(debug_mode);
    if let Some(p) = &lsl_path {
        linker.load_lsl_script(p);
    } else {
        linker.load_ldl_script(ldl_path.as_ref().unwrap());
    }

    println!("minilink: linke {} Objektdatei(en) -> {}", inputs.len(), output_path);

    let mut file_indices = Vec::new();
    for inp in &inputs {
        file_indices.push(linker.load_object_file(inp));
    }

    linker.dump_files(&file_indices);
    linker.dump_file_indices(&file_indices);

    for &fi in &file_indices {
        linker.import_sections(fi);
        linker.import_symbols(fi);
        linker.import_relocations(fi);
    }

    linker.resolve_all_symbols();

    let layout = if linker.use_lsl { linker.place_sections_lsl() } else { linker.place_sections_default() };
    linker.assign_symbol_addresses(&layout);

    let entry_idx = match linker.find_symbol_by_name("_start") {
        Some(x) => x,
        None => {
            eprintln!("minilink: kein Einsprungpunkt '_start' gefunden");
            process::exit(1);
        }
    };
    let entry_addr = linker.symbols[entry_idx].final_address;

    linker.apply_relocations(&layout);
    linker.write_output(&output_path, &layout, entry_addr);

    let script_desc = if let Some(p) = &lsl_path {
        format!("--lsl {}", p)
    } else {
        format!("-T {}", ldl_path.as_ref().unwrap())
    };
    linker.print_map(&layout, entry_addr, &script_desc, &output_path);
    println!("minilink: fertig. Einsprungpunkt _start @ 0x{:08x}", entry_addr);
}
