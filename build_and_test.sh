#!/usr/bin/env bash
#
# build_and_test.sh — baut minilink, linkt das Testprogramm mit dem
# eigenen Linker und prueft Ausgabe + Exit-Code.
#
# Setzt die Schritte aus README.md ("Build & Test") 1:1 um.

set -euo pipefail

# Ins Projektverzeichnis wechseln (Verzeichnis dieses Scripts)
cd "$(dirname "$0")"

CC="${CC:-gcc}"

echo "==> [1/5] Testprogramm kompilieren (test/main.o, test/msg.o)"
"$CC" -c -ffreestanding -fno-pie -fno-stack-protector -O0 -o test/main.o test/main.c
"$CC" -c -ffreestanding -fno-pie -fno-stack-protector -O0 -o test/msg.o  test/msg.c

echo "==> [2/5] minilink bauen (build/minilink)"
mkdir -p build
"$CC" -O0 -g -Wall -o build/minilink src/minilink.c

echo "==> [3/5] Linken mit minilink (nicht mit ld!)"
./build/minilink test/main.o test/msg.o -o test/program

echo "==> [4/5] Erzeugtes Executable ausfuehren"
set +e
ACTUAL_OUT="$(./test/program)"
ACTUAL_RC=$?
set -e

EXPECTED_OUT="Hello from mini-linker!
Hello from mini-linker!"
EXPECTED_RC=2

echo "==> [5/5] Ergebnis pruefen"
echo "--- Ausgabe ---"
echo "$ACTUAL_OUT"
echo "--- Exit-Code: $ACTUAL_RC ---"

FAIL=0
if [ "$ACTUAL_OUT" != "$EXPECTED_OUT" ]; then
	echo "FEHLER: Ausgabe weicht ab. Erwartet:"
	echo "$EXPECTED_OUT"
	FAIL=1
fi
if [ "$ACTUAL_RC" -ne "$EXPECTED_RC" ]; then
	echo "FEHLER: Exit-Code $ACTUAL_RC, erwartet $EXPECTED_RC"
	FAIL=1
fi

if [ "$FAIL" -ne 0 ]; then
	echo "==> TEST FEHLGESCHLAGEN"
	exit 1
fi

echo "==> TEST OK"
