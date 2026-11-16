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
## 2026-11-16 -- Commit `3c01eb5`

Vergleich zu Commit `3c01eb5`.

### C -- Gesamt: 🟢 besser

| Metrik | vorher | jetzt | Delta |
|---|---|---|---|
| Instruktionen (Ir) | 940796 | 940784 | -12 (-0.00%) |

#### Funktionen mit Aenderung

| Funktion | vorher | jetzt | Delta |  |
|---|---|---|---|---|
| `_IO_file_fopen@@GLIBC_2.2.5` | 385 | 373 | -12 | 🟢 |

### Rust -- Gesamt: 🟢 besser

| Metrik | vorher | jetzt | Delta |
|---|---|---|---|
| Instruktionen (Ir) | 1225905 | 1225902 | -3 (-0.00%) |

#### Funktionen mit Aenderung

| Funktion | vorher | jetzt | Delta |  |
|---|---|---|---|---|
| `_IO_file_fopen@@GLIBC_2.2.5` | 108 | 105 | -3 | 🟢 |

