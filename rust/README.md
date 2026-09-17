# minilink (Rust-Portierung)

1:1-Portierung von `../src/minilink.c` nach Rust — gleiche CLI, gleiches
Verhalten, gleiche bewussten Vereinfachungen (siehe `../README.md`). Es wird
bewusst kein ELF-Crate (`object`, `goblin`, ...) verwendet: `src/elf.rs`
parst/schreibt die rohen ELF64-Structs von Hand, genau wie die C-Version nur
`<elf.h>` nutzt.

## Aufbau

```
rust/
├── Cargo.toml
├── build_and_test.sh   Baut (release) + linkt (none / lsl / g) + prüft
├── src/
│   ├── main.rs          Reader/Resolver/Merger/Placement/Relocations/Writer/MAP/Driver
│   ├── elf.rs            ELF64-Struct-Definitionen (Ehdr/Shdr/Sym/Rela/Phdr)
│   └── lsl.rs             TASKING-LSL-Reader (--lsl)
└── test/
    ├── none/  main.o msg.o  out/program     (-T)
    ├── lsl/   main.o msg.o  out/program     (--lsl)
    └── g/     main.o msg.o  out/program     (-T --debug, mit -g)
```

Die Testquellen (`main.c`, `msg.c`) und Scripts (`default.ldl`, `tc27x.lsl`)
liegen im gemeinsamen `../test/`-Ordner und werden von beiden
Implementierungen (C und Rust) benutzt.

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
./target/release/minilink -T ../test/default.ldl \
    test/none/main.o test/none/msg.o -o test/none/out/program
```
