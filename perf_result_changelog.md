# minilink: Profiling-Changelog (C vs. Rust)

Automatisch generiert vom woechentlichen `Weekly Profiling`-Workflow
(`.github/workflows/profile.yml`, siehe auch `profile.sh` und
`changelog_entry.sh`). Misst per `valgrind --tool=callgrind` die
ausgefuehrten Instruktionen (`Ir`, deterministisch, kein Wall-Clock-Sampling)
beim Linken der Testvariante `g` (`-T test/default.ldl --debug`) mit
`build/minilink` (C) bzw. `src/rust/target/release/minilink` (Rust) und
vergleicht das Ergebnis mit dem vorherigen Lauf. Niedriger ist besser
(weniger Instruktionen).

Neueste Eintraege stehen oben.

<!-- NEUE EINTRAEGE WERDEN HIER VON changelog_entry.sh EINGEFUEGT -->
## 2026-10-12 -- Commit `3c01eb5`

Vergleich zu Commit `3c01eb5`.

### C -- Gesamt: ⚪ unveraendert

| Metrik | vorher | jetzt | Delta |
|---|---|---|---|
| Instruktionen (Ir) | 1016121 | 1016121 | 0 (+0.00%) |

_Keine Aenderung bei den erfassten Funktionen._

### Rust -- Gesamt: 🟢 besser

| Metrik | vorher | jetzt | Delta |
|---|---|---|---|
| Instruktionen (Ir) | 1277821 | 1268442 | -9379 (-0.73%) |

#### Funktionen mit Aenderung

| Funktion | vorher | jetzt | Delta |  |
|---|---|---|---|---|
| `<<alloc::io::buffered::bufwriter::BufWriter<_>>::flush_buf::BufGuard` | - | 1481 | neu | 🆕 |
| `<<alloc::io::buffered::bufwriter::BufWriter<_>>::flush_buf::BufGuard>::remaining` | - | 560 | neu | 🆕 |
| `<alloc::io::buffered::bufwriter::BufWriter<std::io::stdio::StdoutRaw>>::flush_buf` | - | 3314 | neu | 🆕 |
| `<core::io::write::default_write_fmt::Adapter<std::fs::File>` | - | 16735 | neu | 🆕 |
| `<core::io::write::default_write_fmt::Adapter<std::io::stdio::StdoutLock>` | - | 30948 | neu | 🆕 |
| `alloc::io::read::default_read_to_end::<std::fs::File>` | - | 228 | neu | 🆕 |
| `core::io::write::default_write_fmt::<std::fs::File>` | - | 459 | neu | 🆕 |
| `core::slice::memchr::memrchr_aligned` | - | 77662 | neu | 🆕 |
| `core::fmt::write` | 54508 | 55084 | 576 | 🔴 |
| `<u64` | 800 | 1108 | 308 | 🔴 |
| `minilink::elf::cstr_at` | 10644 | 10725 | 81 | 🔴 |
| `std::sys::fs::unix::try_statx` | 252 | 332 | 80 | 🔴 |
| `<alloc::vec::Vec<usize>` | 1230 | 1302 | 72 | 🔴 |
| `____strtoul_l_internal` | 21974 | 21992 | 18 | 🔴 |
| `<hashbrown::raw::RawTable<(usize,` | 348 | 360 | 12 | 🔴 |
| `minilink::main` | 138729 | 138740 | 11 | 🔴 |
| `std::fs::write::inner` | 100 | 104 | 4 | 🔴 |
| `__rustc::__rdl_dealloc` | 291 | 290 | -1 | 🟢 |
| `__rustc::__rust_dealloc` | 291 | 290 | -1 | 🟢 |
| `core::ptr::drop_glue::<minilink::Linker>` | 683 | 682 | -1 | 🟢 |
| `<hashbrown::map::HashMap<usize,` | 1614 | 1608 | -6 | 🟢 |
| `<u8` | 3848 | 3840 | -8 | 🟢 |
| `core::str::converts::from_utf8` | 978 | 969 | -9 | 🟢 |
| `<std::sys::stdio::unix::Stdout` | 144 | 132 | -12 | 🟢 |
| `<u16` | 508 | 496 | -12 | 🟢 |
| `__memcmp_avx2_movbe` | 611 | 590 | -21 | 🟢 |
| `__memcpy_avx_unaligned_erms` | 43357 | 43331 | -26 | 🟢 |
| `free` | 9207 | 9176 | -31 | 🟢 |
| `_dl_relocate_object` | 46053 | 46014 | -39 | 🟢 |
| `_int_free` | 24143 | 24068 | -75 | 🟢 |
| `strcmp` | 15563 | 15462 | -101 | 🟢 |
| `getauxval` | 292 | 146 | -146 | 🟢 |
| `<std::io::stdio::StdoutLock` | 69758 | 68452 | -1306 | 🟢 |
| `<core::fmt::Formatter>::pad_integral` | 29823 | 28030 | -1793 | 🟢 |
| `<core::fmt::Formatter>::pad` | 52516 | 50358 | -2158 | 🟢 |
| `<minilink::elf::Ehdr>::parse` | 86 | - | entfernt | ➖ |
| `<std::io::buffered::bufwriter::BufWriter<std::io::stdio::StdoutRaw>>::flush_buf` | 4080 | - | entfernt | ➖ |
| `<std::io::default_write_fmt::Adapter<std::fs::File>` | 16735 | - | entfernt | ➖ |
| `<std::io::default_write_fmt::Adapter<std::io::stdio::StdoutLock>` | 30948 | - | entfernt | ➖ |
| `<u64>::_fmt_inner` | 996 | - | entfernt | ➖ |
| `core::slice::memchr::memrchr` | 82622 | - | entfernt | ➖ |
| `std::io::default_read_to_end::<std::fs::File>` | 150 | - | entfernt | ➖ |
| `std::io::default_write_fmt::<std::fs::File>` | 459 | - | entfernt | ➖ |

