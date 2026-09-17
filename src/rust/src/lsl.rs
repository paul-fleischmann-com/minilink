//! TASKING-LSL-Reader (--lsl) -- stark vereinfachte Teilmenge.
//!
//! Gelesen wird:
//!   memory <id> {
//!       type = rom | ram | nvram;
//!       size = <zahl>[k|M|G];
//!       map ( ... dest_offset = <adresse> ... );
//!   }
//!   section_layout [ ::<name> ] {
//!       group [ ( ... run_addr = mem:<id> ... ) ] {
//!           select "<pattern>";   // '*' am Ende = Praefix-Wildcard
//!           ...
//!       }
//!   }
//!
//! Alles Uebrige (architecture, bus, derivative, core, section_setup, ...)
//! wird per Klammer-Skip ueberlesen. Kommentare: // und C-Bloecke.

#[derive(Clone, Debug, Default)]
pub struct LslMem {
    pub id: String,
    pub is_ram: bool,
    pub addr: u64,
    pub size: u64,
}

#[derive(Clone, Debug, Default)]
pub struct LslGroup {
    pub run_mem: String,
    pub sel: Vec<String>,
}

#[derive(Clone, Copy, PartialEq, Debug)]
enum TokKind {
    Eof,
    Id,
    Num,
    Str,
    Punc,
}

#[derive(Clone, Debug)]
struct Tok {
    kind: TokKind,
    text: String,
    num: u64,
}

fn lsl_num(s: &str) -> u64 {
    let s = s.trim();
    let (digits, mult) = if let Some(stripped) = s.strip_suffix(|c| matches!(c, 'k' | 'K')) {
        (stripped, 1024u64)
    } else if let Some(stripped) = s.strip_suffix(|c| matches!(c, 'm' | 'M')) {
        (stripped, 1024u64 * 1024)
    } else if let Some(stripped) = s.strip_suffix(|c| matches!(c, 'g' | 'G')) {
        (stripped, 1024u64 * 1024 * 1024)
    } else {
        (s, 1u64)
    };
    let v = if let Some(hex) = digits.strip_prefix("0x").or_else(|| digits.strip_prefix("0X")) {
        u64::from_str_radix(hex, 16).unwrap_or(0)
    } else if digits.len() > 1 && digits.starts_with('0') {
        u64::from_str_radix(&digits[1..], 8).unwrap_or(0)
    } else {
        digits.parse::<u64>().unwrap_or(0)
    };
    v * mult
}

struct Lexer<'a> {
    buf: &'a [u8],
    pos: usize,
}

impl<'a> Lexer<'a> {
    fn new(buf: &'a [u8]) -> Self {
        Lexer { buf, pos: 0 }
    }

    fn peek(&self) -> u8 {
        if self.pos < self.buf.len() {
            self.buf[self.pos]
        } else {
            0
        }
    }
    fn peek_at(&self, off: usize) -> u8 {
        let p = self.pos + off;
        if p < self.buf.len() {
            self.buf[p]
        } else {
            0
        }
    }

    fn skip_ws(&mut self) {
        loop {
            while self.pos < self.buf.len() && self.buf[self.pos] <= b' ' {
                self.pos += 1;
            }
            if self.peek() == b'/' && self.peek_at(1) == b'/' {
                while self.pos < self.buf.len() && self.buf[self.pos] != b'\n' {
                    self.pos += 1;
                }
                continue;
            }
            if self.peek() == b'/' && self.peek_at(1) == b'*' {
                self.pos += 2;
                while self.pos < self.buf.len() && !(self.peek() == b'*' && self.peek_at(1) == b'/') {
                    self.pos += 1;
                }
                if self.pos < self.buf.len() {
                    self.pos += 2;
                }
                continue;
            }
            break;
        }
    }

    fn next(&mut self) -> Tok {
        self.skip_ws();
        if self.pos >= self.buf.len() {
            return Tok { kind: TokKind::Eof, text: String::new(), num: 0 };
        }
        let c = self.buf[self.pos];
        if c == b'"' {
            self.pos += 1;
            let start = self.pos;
            while self.pos < self.buf.len() && self.buf[self.pos] != b'"' {
                self.pos += 1;
            }
            let text = String::from_utf8_lossy(&self.buf[start..self.pos]).into_owned();
            if self.pos < self.buf.len() {
                self.pos += 1;
            }
            Tok { kind: TokKind::Str, text, num: 0 }
        } else if c.is_ascii_digit() {
            let start = self.pos;
            while self.pos < self.buf.len() {
                let ch = self.buf[self.pos];
                if ch.is_ascii_alphanumeric() {
                    self.pos += 1;
                } else {
                    break;
                }
            }
            let text = String::from_utf8_lossy(&self.buf[start..self.pos]).into_owned();
            let num = lsl_num(&text);
            Tok { kind: TokKind::Num, text, num }
        } else if c.is_ascii_alphabetic() || c == b'_' || c == b'.' {
            let start = self.pos;
            while self.pos < self.buf.len() {
                let ch = self.buf[self.pos];
                if ch.is_ascii_alphanumeric() || ch == b'_' || ch == b'.' || ch == b'*' {
                    self.pos += 1;
                } else {
                    break;
                }
            }
            let text = String::from_utf8_lossy(&self.buf[start..self.pos]).into_owned();
            Tok { kind: TokKind::Id, text, num: 0 }
        } else {
            self.pos += 1;
            Tok { kind: TokKind::Punc, text: (c as char).to_string(), num: 0 }
        }
    }
}

pub struct LslResult {
    pub mems: Vec<LslMem>,
    pub groups: Vec<LslGroup>,
}

struct Parser<'a> {
    lx: Lexer<'a>,
    tok: Tok,
}

impl<'a> Parser<'a> {
    fn new(buf: &'a [u8]) -> Self {
        let mut lx = Lexer::new(buf);
        let tok = lx.next();
        Parser { lx, tok }
    }

    fn adv(&mut self) {
        self.tok = self.lx.next();
    }

    fn is_punc(&self, p: char) -> bool {
        self.tok.kind == TokKind::Punc && self.tok.text.as_bytes()[0] == p as u8
    }
    fn is_id(&self, s: &str) -> bool {
        self.tok.kind == TokKind::Id && self.tok.text == s
    }

    /// Ab der aktuellen Position bis hinter das passende '}' bzw. bis ';'
    /// auf Tiefe 0 (fuer nicht unterstuetzte Konstrukte).
    fn skip_unknown(&mut self) {
        let mut depth = 0i32;
        let mut saw_brace = false;
        while self.tok.kind != TokKind::Eof {
            if self.is_punc('{') {
                depth += 1;
                saw_brace = true;
                self.adv();
                continue;
            }
            if self.is_punc('}') {
                depth -= 1;
                self.adv();
                if saw_brace && depth <= 0 {
                    return;
                }
                continue;
            }
            if self.is_punc(';') && depth == 0 && !saw_brace {
                self.adv();
                return;
            }
            self.adv();
        }
    }

    fn parse_memory(&mut self, mems: &mut Vec<LslMem>) {
        self.adv(); // -> <id>
        let mut m = LslMem::default();
        if self.tok.kind == TokKind::Id {
            m.id = self.tok.text.clone();
            self.adv();
        }
        if !self.is_punc('{') {
            self.skip_unknown();
            return;
        }
        self.adv();

        let mut depth = 1;
        while depth > 0 && self.tok.kind != TokKind::Eof {
            if self.is_punc('{') {
                depth += 1;
                self.adv();
                continue;
            }
            if self.is_punc('}') {
                depth -= 1;
                self.adv();
                continue;
            }

            if depth == 1 && self.is_id("type") {
                self.adv();
                if self.is_punc('=') {
                    self.adv();
                }
                if self.tok.kind == TokKind::Id {
                    m.is_ram = self.tok.text == "ram" || self.tok.text == "nvram";
                }
                self.adv();
                continue;
            }
            if depth == 1 && self.is_id("size") {
                self.adv();
                if self.is_punc('=') {
                    self.adv();
                }
                if self.tok.kind == TokKind::Num {
                    m.size = self.tok.num;
                }
                self.adv();
                continue;
            }
            if self.is_id("map") {
                self.adv(); // '(' ... ')' scannen
                let mut pd = 0i32;
                while self.tok.kind != TokKind::Eof {
                    if self.is_punc('(') {
                        pd += 1;
                        self.adv();
                        continue;
                    }
                    if self.is_punc(')') {
                        pd -= 1;
                        self.adv();
                        if pd <= 0 {
                            break;
                        }
                        continue;
                    }
                    if self.is_id("dest_offset") {
                        self.adv();
                        if self.is_punc('=') {
                            self.adv();
                        }
                        if self.tok.kind == TokKind::Num {
                            m.addr = self.tok.num;
                        }
                        continue;
                    }
                    self.adv();
                }
                continue;
            }
            self.adv();
        }

        if !m.id.is_empty() {
            mems.push(m);
        }
    }

    fn parse_group(&mut self, mems: &[LslMem], groups: &mut Vec<LslGroup>) {
        self.adv(); // nach 'group'
        let mut g = LslGroup::default();

        if self.is_punc('(') {
            self.adv();
            while !self.is_punc(')') && self.tok.kind != TokKind::Eof {
                if self.is_id("run_addr") || self.is_id("load_addr") {
                    let is_run = self.is_id("run_addr");
                    self.adv();
                    if self.is_punc('=') {
                        self.adv();
                    }
                    if self.is_id("mem") {
                        self.adv();
                        if self.is_punc(':') {
                            self.adv();
                        }
                        if is_run && self.tok.kind == TokKind::Id {
                            g.run_mem = self.tok.text.clone();
                        }
                        if self.tok.kind == TokKind::Id {
                            self.adv();
                        }
                    } else {
                        self.adv();
                    }
                    continue;
                }
                self.adv();
            }
            if self.is_punc(')') {
                self.adv();
            }
        }

        if !self.is_punc('{') {
            self.skip_unknown();
            return;
        }
        self.adv();
        while !self.is_punc('}') && self.tok.kind != TokKind::Eof {
            if self.is_id("select") {
                self.adv();
                if self.tok.kind == TokKind::Str || self.tok.kind == TokKind::Id {
                    g.sel.push(self.tok.text.clone());
                    self.adv();
                }
                if self.is_punc(';') {
                    self.adv();
                }
            } else if self.is_id("group") {
                eprintln!("minilink: LSL: verschachtelte group wird ignoriert");
                self.adv();
                self.skip_unknown();
            } else {
                self.adv();
            }
        }
        if self.is_punc('}') {
            self.adv();
        }

        if !g.run_mem.is_empty() {
            let found = mems.iter().any(|m| m.id == g.run_mem);
            if !found {
                eprintln!("minilink: LSL: group run_addr = mem:{} -- unbekannte Region", g.run_mem);
            }
        }
        if !g.sel.is_empty() {
            groups.push(g);
        }
    }

    fn parse_section_layout(&mut self, mems: &[LslMem], groups: &mut Vec<LslGroup>) {
        self.adv(); // nach 'section_layout'
        while self.is_punc(':') {
            self.adv();
        }
        if self.tok.kind == TokKind::Id {
            self.adv();
        }
        if !self.is_punc('{') {
            self.skip_unknown();
            return;
        }
        self.adv();
        while !self.is_punc('}') && self.tok.kind != TokKind::Eof {
            if self.is_id("group") {
                self.parse_group(mems, groups);
            } else if self.is_id("select") {
                self.adv();
                if self.tok.kind == TokKind::Str || self.tok.kind == TokKind::Id {
                    self.adv();
                }
                if self.is_punc(';') {
                    self.adv();
                }
            } else {
                self.adv();
            }
        }
        if self.is_punc('}') {
            self.adv();
        }
    }
}

pub fn parse(buf: &[u8]) -> LslResult {
    let mut mems = Vec::new();
    let mut groups = Vec::new();
    let mut p = Parser::new(buf);
    while p.tok.kind != TokKind::Eof {
        if p.is_id("memory") {
            p.parse_memory(&mut mems);
        } else if p.is_id("section_layout") {
            p.parse_section_layout(&mems, &mut groups);
        } else if p.tok.kind == TokKind::Id {
            p.adv();
            p.skip_unknown();
        } else {
            p.adv();
        }
    }
    LslResult { mems, groups }
}

/// '*' am Ende = Praefix-Match, sonst exakter Vergleich.
pub fn sel_match(pat: &str, name: &str) -> bool {
    if let Some(prefix) = pat.strip_suffix('*') {
        name.starts_with(prefix)
    } else {
        pat == name
    }
}
