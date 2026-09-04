#!/usr/bin/env bash
#
# build_and_test.sh — baut minilink und linkt das Testprogramm in drei
# Varianten, jede in ihrem eigenen Unterordner unter test/:
#
#   test/none/  -T test/default.ldl                (kein Debug)
#   test/lsl/   --lsl test/tc27x.lsl               (mehrere PT_LOAD)
#   test/g/     -T test/default.ldl --debug        (Debug-Info behalten, -g)
#
# Objektdateien liegen direkt im Varianten-Ordner, die fertigen
# Executables unter test/<variante>/out/.
# Prueft je Variante Ausgabe + Exit-Code, bei --debug zusaetzlich DWARF.

set -euo pipefail
cd "$(dirname "$0")"

CC="${CC:-gcc}"
CFLAGS="-ffreestanding -fno-pie -fno-stack-protector -O0"

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
	mkdir -p "test/$v/out"
	gflag=""; [ "$v" = g ] && gflag="-g"
	# shellcheck disable=SC2086
	"$CC" -c $CFLAGS $gflag -o "test/$v/main.o" test/main.c
	# shellcheck disable=SC2086
	"$CC" -c $CFLAGS $gflag -o "test/$v/msg.o"  test/msg.c
done

echo "==> [2/5] minilink bauen (build/minilink)"
mkdir -p build
"$CC" -O0 -g -Wall -o build/minilink src/minilink.c

echo "==> [3/5] Linken -- test/none/out  (-T test/default.ldl)"
./build/minilink -T test/default.ldl            test/none/main.o test/none/msg.o -o test/none/out/program

echo "==> [4/5] Linken -- test/lsl/out (--lsl)  und  test/g/out (-T --debug)"
./build/minilink --lsl test/tc27x.lsl           test/lsl/main.o  test/lsl/msg.o  -o test/lsl/out/program
./build/minilink -T test/default.ldl --debug    test/g/main.o    test/g/msg.o    -o test/g/out/program

echo "==> [5/5] Ausfuehren und pruefen"
check_program "none" ./test/none/out/program
check_program "lsl"  ./test/lsl/out/program
check_program "g"    ./test/g/out/program

echo "--- g: DWARF pruefen ---"
readelf -SW test/g/out/program | grep -q '\.debug_info' || { echo "FEHLER (g): .debug_info fehlt"; FAIL=1; }
a2l="$(addr2line -e test/g/out/program -f 0x40101d | head -1 || true)"
echo "addr2line 0x40101d -> $a2l"
[ "$a2l" = "_start" ] || { echo "FEHLER (g): addr2line liefert '$a2l' statt '_start'"; FAIL=1; }

if [ "$FAIL" -ne 0 ]; then
	echo "==> TEST FEHLGESCHLAGEN"
	exit 1
fi
echo "==> TEST OK"
