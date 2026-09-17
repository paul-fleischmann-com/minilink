//! Minimale ELF64-Struct-Definitionen + Byte-Serialisierung.
//!
//! Es wird bewusst kein Crate wie `object`/`goblin` benutzt -- genau wie die
//! C-Referenzimplementierung (die nur <elf.h> nutzt) sollen hier die
//! rohen ELF64-Structs von Hand gelesen/geschrieben werden.

pub const ELFMAG: &[u8; 4] = b"\x7fELF";
pub const ELFCLASS64: u8 = 2;
pub const ELFDATA2LSB: u8 = 1;
pub const EV_CURRENT: u8 = 1;
pub const ELFOSABI_SYSV: u8 = 0;

pub const ET_REL: u16 = 1;
pub const ET_EXEC: u16 = 2;
pub const EM_X86_64: u16 = 62;

pub const SHT_NULL: u32 = 0;
pub const SHT_PROGBITS: u32 = 1;
pub const SHT_SYMTAB: u32 = 2;
pub const SHT_STRTAB: u32 = 3;
pub const SHT_RELA: u32 = 4;
pub const SHT_NOBITS: u32 = 8;

pub const SHF_WRITE: u64 = 1 << 0;
pub const SHF_ALLOC: u64 = 1 << 1;
pub const SHF_EXECINSTR: u64 = 1 << 2;

pub const STT_NOTYPE: u8 = 0;
pub const STT_OBJECT: u8 = 1;
pub const STT_FUNC: u8 = 2;
pub const STT_SECTION: u8 = 3;
pub const STT_FILE: u8 = 4;

pub const STB_LOCAL: u8 = 0;
pub const STB_GLOBAL: u8 = 1;

pub const SHN_UNDEF: u16 = 0;

pub const PT_LOAD: u32 = 1;
pub const PF_X: u32 = 1;
pub const PF_W: u32 = 2;
pub const PF_R: u32 = 4;

pub const R_X86_64_64: u32 = 1;
pub const R_X86_64_PC32: u32 = 2;
pub const R_X86_64_PLT32: u32 = 4;
pub const R_X86_64_32: u32 = 10;
pub const R_X86_64_32S: u32 = 11;

pub fn st_bind(info: u8) -> u8 {
    info >> 4
}
pub fn st_type(info: u8) -> u8 {
    info & 0xf
}
pub fn st_info(bind: u8, ty: u8) -> u8 {
    (bind << 4) | (ty & 0xf)
}

fn u16le(b: &[u8], off: usize) -> u16 {
    u16::from_le_bytes(b[off..off + 2].try_into().unwrap())
}
fn u32le(b: &[u8], off: usize) -> u32 {
    u32::from_le_bytes(b[off..off + 4].try_into().unwrap())
}
fn u64le(b: &[u8], off: usize) -> u64 {
    u64::from_le_bytes(b[off..off + 8].try_into().unwrap())
}

/// Liest einen NUL-terminierten String ab `off` aus `buf`.
pub fn cstr_at(buf: &[u8], off: usize) -> String {
    if off >= buf.len() {
        return String::new();
    }
    let end = buf[off..].iter().position(|&b| b == 0).map(|p| off + p).unwrap_or(buf.len());
    String::from_utf8_lossy(&buf[off..end]).into_owned()
}

#[derive(Clone, Debug)]
pub struct Ehdr {
    pub e_ident: [u8; 16],
    pub e_type: u16,
    pub e_machine: u16,
    pub e_version: u32,
    pub e_entry: u64,
    pub e_phoff: u64,
    pub e_shoff: u64,
    pub e_flags: u32,
    pub e_ehsize: u16,
    pub e_phentsize: u16,
    pub e_phnum: u16,
    pub e_shentsize: u16,
    pub e_shnum: u16,
    pub e_shstrndx: u16,
}

impl Ehdr {
    pub const SIZE: usize = 64;

    pub fn parse(b: &[u8]) -> Ehdr {
        let mut e_ident = [0u8; 16];
        e_ident.copy_from_slice(&b[0..16]);
        Ehdr {
            e_ident,
            e_type: u16le(b, 16),
            e_machine: u16le(b, 18),
            e_version: u32le(b, 20),
            e_entry: u64le(b, 24),
            e_phoff: u64le(b, 32),
            e_shoff: u64le(b, 40),
            e_flags: u32le(b, 48),
            e_ehsize: u16le(b, 52),
            e_phentsize: u16le(b, 54),
            e_phnum: u16le(b, 56),
            e_shentsize: u16le(b, 58),
            e_shnum: u16le(b, 60),
            e_shstrndx: u16le(b, 62),
        }
    }

    pub fn write(&self, out: &mut Vec<u8>) {
        out.extend_from_slice(&self.e_ident);
        out.extend_from_slice(&self.e_type.to_le_bytes());
        out.extend_from_slice(&self.e_machine.to_le_bytes());
        out.extend_from_slice(&self.e_version.to_le_bytes());
        out.extend_from_slice(&self.e_entry.to_le_bytes());
        out.extend_from_slice(&self.e_phoff.to_le_bytes());
        out.extend_from_slice(&self.e_shoff.to_le_bytes());
        out.extend_from_slice(&self.e_flags.to_le_bytes());
        out.extend_from_slice(&self.e_ehsize.to_le_bytes());
        out.extend_from_slice(&self.e_phentsize.to_le_bytes());
        out.extend_from_slice(&self.e_phnum.to_le_bytes());
        out.extend_from_slice(&self.e_shentsize.to_le_bytes());
        out.extend_from_slice(&self.e_shnum.to_le_bytes());
        out.extend_from_slice(&self.e_shstrndx.to_le_bytes());
        debug_assert_eq!(out.len(), Self::SIZE);
    }
}

#[derive(Clone, Debug, Default)]
pub struct Shdr {
    pub sh_name: u32,
    pub sh_type: u32,
    pub sh_flags: u64,
    pub sh_addr: u64,
    pub sh_offset: u64,
    pub sh_size: u64,
    pub sh_link: u32,
    pub sh_info: u32,
    pub sh_addralign: u64,
    pub sh_entsize: u64,
}

impl Shdr {
    pub const SIZE: usize = 64;

    pub fn parse(b: &[u8]) -> Shdr {
        Shdr {
            sh_name: u32le(b, 0),
            sh_type: u32le(b, 4),
            sh_flags: u64le(b, 8),
            sh_addr: u64le(b, 16),
            sh_offset: u64le(b, 24),
            sh_size: u64le(b, 32),
            sh_link: u32le(b, 40),
            sh_info: u32le(b, 44),
            sh_addralign: u64le(b, 48),
            sh_entsize: u64le(b, 56),
        }
    }

    pub fn write(&self, out: &mut Vec<u8>) {
        out.extend_from_slice(&self.sh_name.to_le_bytes());
        out.extend_from_slice(&self.sh_type.to_le_bytes());
        out.extend_from_slice(&self.sh_flags.to_le_bytes());
        out.extend_from_slice(&self.sh_addr.to_le_bytes());
        out.extend_from_slice(&self.sh_offset.to_le_bytes());
        out.extend_from_slice(&self.sh_size.to_le_bytes());
        out.extend_from_slice(&self.sh_link.to_le_bytes());
        out.extend_from_slice(&self.sh_info.to_le_bytes());
        out.extend_from_slice(&self.sh_addralign.to_le_bytes());
        out.extend_from_slice(&self.sh_entsize.to_le_bytes());
        debug_assert_eq!(out.len() % Self::SIZE, 0);
    }
}

#[derive(Clone, Copy, Debug, Default)]
pub struct Sym {
    pub st_name: u32,
    pub st_info: u8,
    pub st_other: u8,
    pub st_shndx: u16,
    pub st_value: u64,
    pub st_size: u64,
}

impl Sym {
    pub const SIZE: usize = 24;

    pub fn parse(b: &[u8]) -> Sym {
        Sym {
            st_name: u32le(b, 0),
            st_info: b[4],
            st_other: b[5],
            st_shndx: u16le(b, 6),
            st_value: u64le(b, 8),
            st_size: u64le(b, 16),
        }
    }

    pub fn write(&self, out: &mut Vec<u8>) {
        out.extend_from_slice(&self.st_name.to_le_bytes());
        out.push(self.st_info);
        out.push(self.st_other);
        out.extend_from_slice(&self.st_shndx.to_le_bytes());
        out.extend_from_slice(&self.st_value.to_le_bytes());
        out.extend_from_slice(&self.st_size.to_le_bytes());
    }
}

#[derive(Clone, Copy, Debug)]
pub struct Rela {
    pub r_offset: u64,
    pub r_info: u64,
    pub r_addend: i64,
}

impl Rela {
    pub const SIZE: usize = 24;

    pub fn parse(b: &[u8]) -> Rela {
        Rela {
            r_offset: u64le(b, 0),
            r_info: u64le(b, 8),
            r_addend: i64::from_le_bytes(b[16..24].try_into().unwrap()),
        }
    }

    pub fn r_sym(&self) -> u32 {
        (self.r_info >> 32) as u32
    }
    pub fn r_type(&self) -> u32 {
        (self.r_info & 0xffffffff) as u32
    }
}

#[derive(Clone, Copy, Debug, Default)]
pub struct Phdr {
    pub p_type: u32,
    pub p_flags: u32,
    pub p_offset: u64,
    pub p_vaddr: u64,
    pub p_paddr: u64,
    pub p_filesz: u64,
    pub p_memsz: u64,
    pub p_align: u64,
}

impl Phdr {
    pub const SIZE: usize = 56;

    pub fn write(&self, out: &mut Vec<u8>) {
        out.extend_from_slice(&self.p_type.to_le_bytes());
        out.extend_from_slice(&self.p_flags.to_le_bytes());
        out.extend_from_slice(&self.p_offset.to_le_bytes());
        out.extend_from_slice(&self.p_vaddr.to_le_bytes());
        out.extend_from_slice(&self.p_paddr.to_le_bytes());
        out.extend_from_slice(&self.p_filesz.to_le_bytes());
        out.extend_from_slice(&self.p_memsz.to_le_bytes());
        out.extend_from_slice(&self.p_align.to_le_bytes());
    }
}
