# minilink (Rust-Portierung)

1:1-Portierung von `../c/minilink.c` nach Rust — gleiche CLI, gleiches
Verhalten, gleiche bewussten Vereinfachungen (siehe `../../README.md`). Es
wird bewusst kein ELF-Crate (`object`, `goblin`, ...) verwendet: `src/elf.rs`
parst/schreibt die rohen ELF64-Structs von Hand, genau wie die C-Version nur
`<elf.h>` nutzt.

## Aufbau

```
src/rust/
├── Cargo.toml
├── build_and_test.sh   Baut (release) + linkt (none / lsl / g) + prüft
└── src/
    ├── main.rs          Reader/Resolver/Merger/Placement/Relocations/Writer/MAP/Driver
    ├── elf.rs            ELF64-Struct-Definitionen (Ehdr/Shdr/Sym/Rela/Phdr)
    └── lsl.rs             TASKING-LSL-Reader (--lsl)
```

Die Testquellen (`main.c`, `msg.c`) und Scripts (`default.ldl`, `tc27x.lsl`)
liegen im gemeinsamen `../../test/`-Ordner und werden von beiden
Implementierungen (C und Rust) benutzt. Die gelinkten Testvarianten dieser
Portierung landen unter `../../test/rust/{none,lsl,g}/` (die C-Varianten
entsprechend unter `../../test/c/`).

## Build & Test

```bash
./build_and_test.sh
```

Baut `minilink` (Rust, `cargo build --release`) und linkt das Testprogramm in
denselben drei Varianten wie die C-Version, führt sie aus und prüft Ausgabe,
Exit-Code sowie (für die `g`-Variante) `addr2line`/`readelf --debug-dump`.

Von Hand:

```bash
cargo build --release
./target/release/minilink -T ../../test/default.ldl \
    ../../test/rust/none/main.o ../../test/rust/none/msg.o \
    -o ../../test/rust/none/out/program
```
