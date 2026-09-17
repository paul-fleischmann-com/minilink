#!/usr/bin/env bash
#
# build_and_test.sh — baut den Rust-Port von minilink und linkt das
# Testprogramm (../../test/main.c, ../../test/msg.c) in drei Varianten,
# genau wie das C-Original (siehe ../../build_and_test.sh):
#
#   test/rust/none/  -T ../../test/default.ldl          (kein Debug)
#   test/rust/lsl/   --lsl ../../test/tc27x.lsl          (mehrere PT_LOAD)
#   test/rust/g/     -T ../../test/default.ldl --debug   (Debug-Info behalten, -g)
#
# Objektdateien liegen direkt im Varianten-Ordner (unter test/rust/),
# die fertigen Executables unter test/rust/<variante>/out/.
#
# gcc wird dabei aus dem Repo-Root aufgerufen (genau wie im C-Skript
# ../../build_and_test.sh), mit denselben relativen Pfaden (test/main.c
# statt ../../test/main.c) -- sonst landet je nach Aufrufverzeichnis ein
# anderer DWARF comp_dir/Directory-Table-Eintrag in den Objektdateien und
# die --debug-Executables (Variante g) waeren trotz identischem Quellcode
# nicht mehr byte-identisch zur C-Variante.

set -euo pipefail
cd "$(dirname "$0")"

CC="${CC:-gcc}"
CFLAGS="-ffreestanding -fno-pie -fno-stack-protector -O0"
TEST="../../test"
ROOT="../.."

EXPECTED_OUT="Hello from mini-linker!
Hello from mini-linker 2 Hello from mini-linker 2 !
Hello from mini-linker 2 Hello from mini-linker 3 !
Hello from mini-linker!
Hello from mini-linker 2 Hello from mini-linker 2 !
Hello from mini-linker 2 Hello from mini-linker 3 !"
EXPECTED_RC=2
FAIL=0

check_program() {
	local desc="$1" prog="$2" out rc
	set +e
	out="$("$prog")"
	rc=$?
	set -e
	echo "--- $desc: Ausgabe ---"
	echo "$out"
	echo "--- $desc: Exit-Code: $rc ---"
	[ "$out" = "$EXPECTED_OUT" ] || { echo "FEHLER ($desc): Ausgabe weicht ab"; FAIL=1; }
	[ "$rc" -eq "$EXPECTED_RC" ] || { echo "FEHLER ($desc): Exit-Code $rc != $EXPECTED_RC"; FAIL=1; }
}

echo "==> [1/5] Varianten-Ordner + Objektdateien kompilieren"
for v in none lsl g; do
	mkdir -p "$TEST/rust/$v/out"
	gflag=""; [ "$v" = g ] && gflag="-g"
	# aus dem Repo-Root heraus aufrufen (siehe Kommentar oben) -- gleiche
	# relative Pfade wie im C-Skript, damit -g denselben comp_dir einbettet
	# shellcheck disable=SC2086
	(cd "$ROOT" && "$CC" -c $CFLAGS $gflag -o "test/rust/$v/main.o" test/main.c)
	# shellcheck disable=SC2086
	(cd "$ROOT" && "$CC" -c $CFLAGS $gflag -o "test/rust/$v/msg.o"  test/msg.c)
done

echo "==> [2/5] minilink (Rust) bauen (cargo build --release)"
cargo build --release
BIN=target/release/minilink

echo "==> [3/5] Linken -- test/rust/none/out  (-T $TEST/default.ldl)"
"$BIN" -T "$TEST/default.ldl"            "$TEST/rust/none/main.o" "$TEST/rust/none/msg.o" -o "$TEST/rust/none/out/program"

echo "==> [4/5] Linken -- test/rust/lsl/out (--lsl)  und  test/rust/g/out (-T --debug)"
"$BIN" --lsl "$TEST/tc27x.lsl"           "$TEST/rust/lsl/main.o"  "$TEST/rust/lsl/msg.o"  -o "$TEST/rust/lsl/out/program"
"$BIN" -T "$TEST/default.ldl" --debug    "$TEST/rust/g/main.o"    "$TEST/rust/g/msg.o"    -o "$TEST/rust/g/out/program"

echo "==> [5/5] Ausfuehren und pruefen"
check_program "none" "$TEST/rust/none/out/program"
check_program "lsl"  "$TEST/rust/lsl/out/program"
check_program "g"    "$TEST/rust/g/out/program"

echo "--- g: DWARF pruefen ---"
readelf -SW "$TEST/rust/g/out/program" | grep -q '\.debug_info' || { echo "FEHLER (g): .debug_info fehlt"; FAIL=1; }
a2l="$(addr2line -e "$TEST/rust/g/out/program" -f 0x40101d | head -1 || true)"
echo "addr2line 0x40101d -> $a2l"
[ "$a2l" = "_start" ] || { echo "FEHLER (g): addr2line liefert '$a2l' statt '_start'"; FAIL=1; }

if [ "$FAIL" -ne 0 ]; then
	echo "==> TEST FEHLGESCHLAGEN"
	exit 1
fi
echo "==> TEST OK"
