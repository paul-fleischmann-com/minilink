#!/usr/bin/env bash
#
# build_and_test.sh — baut minilink, linkt das Testprogramm mit dem
# eigenen Linker (einmal per -T-Script, einmal per --lsl-Script) und
# prueft jeweils Ausgabe + Exit-Code.
#
# Setzt die Schritte aus README.md ("Build & Test") 1:1 um.

set -euo pipefail

# Ins Projektverzeichnis wechseln (Verzeichnis dieses Scripts)
cd "$(dirname "$0")"

CC="${CC:-gcc}"

EXPECTED_OUT="Hello from mini-linker!
Hello from mini-linker 2 Hello from mini-linker 2 !
Hello from mini-linker 2 Hello from mini-linker 3 !
Hello from mini-linker!
Hello from mini-linker 2 Hello from mini-linker 2 !
Hello from mini-linker 2 Hello from mini-linker 3 !"
EXPECTED_RC=2

FAIL=0

# prueft: $1 = Beschreibung, $2 = erzeugtes Executable
check_program() {
	local desc="$1" prog="$2" out rc
	set +e
	out="$("$prog")"
	rc=$?
	set -e
	echo "--- $desc: Ausgabe ---"
	echo "$out"
	echo "--- $desc: Exit-Code: $rc ---"
	if [ "$out" != "$EXPECTED_OUT" ]; then
		echo "FEHLER ($desc): Ausgabe weicht ab. Erwartet:"
		echo "$EXPECTED_OUT"
		FAIL=1
	fi
	if [ "$rc" -ne "$EXPECTED_RC" ]; then
		echo "FEHLER ($desc): Exit-Code $rc, erwartet $EXPECTED_RC"
		FAIL=1
	fi
}

echo "==> [1/5] Testprogramm kompilieren (test/main.o, test/msg.o)"
"$CC" -c -ffreestanding -fno-pie -fno-stack-protector -O0 -o test/main.o test/main.c
"$CC" -c -ffreestanding -fno-pie -fno-stack-protector -O0 -o test/msg.o  test/msg.c

echo "==> [2/5] minilink bauen (build/minilink)"
mkdir -p build
"$CC" -O0 -g -Wall -o build/minilink src/minilink.c

echo "==> [3/5] Linken mit minilink -- Variante A: -T test/default.ldl"
./build/minilink -T test/default.ldl test/main.o test/msg.o -o test/program

echo "==> [4/5] Linken mit minilink -- Variante B: --lsl test/tc27x.lsl"
./build/minilink --lsl test/tc27x.lsl test/main.o test/msg.o -o test/program_lsl

echo "==> [5/5] Beide Executables ausfuehren und pruefen"
check_program "-T"   ./test/program
check_program "--lsl" ./test/program_lsl

if [ "$FAIL" -ne 0 ]; then
	echo "==> TEST FEHLGESCHLAGEN"
	exit 1
fi

echo "==> TEST OK"
