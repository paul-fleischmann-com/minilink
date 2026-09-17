#!/usr/bin/env bash
#
# profile.sh — instruktionsgenaues Profiling (Valgrind/Callgrind) von C- und
# Rust-minilink, pro Funktion aufgeschluesselt.
#
# perf record/report funktionieren in vielen sandboxed Umgebungen nicht
# (u.a. dieser Dev-Container, manche CI-Runner mit eingeschraenktem
# perf_event_paranoid): perf_event_open wird verweigert. Callgrind braucht
# keine besonderen Kernel-Rechte und ist ausserdem deterministisch --
# es zaehlt ausgefuehrte Instruktionen (Ir) statt per Wall-Clock zu samplen,
# daher reproduzierbar unabhaengig von Rechnerlast.
#
# Voraussetzung: ./build_and_test.sh wurde bereits erfolgreich ausgefuehrt
# (baut build/minilink [mit -g] und die Testvarianten unter test/c/).
#
# Baut zusaetzlich eine ungestrippte Rust-Profiling-Variante in einem
# temporaeren target-dir (das normale Release-Binary ist strip = true,
# siehe src/rust/Cargo.toml) -- sonst zeigt callgrind_annotate fuer Rust nur
# rohe Adressen statt Funktionsnamen.
#
# Ergebnis: Top-15-Funktionen je Linker nach Instruktionsanteil (Variante
# g, d.h. -T test/default.ldl --debug), nach stdout und zusaetzlich nach
# $GITHUB_STEP_SUMMARY, falls gesetzt.

set -euo pipefail
cd "$(dirname "$0")"

command -v valgrind >/dev/null 2>&1 || {
	echo "profile.sh: valgrind nicht gefunden (apt install valgrind)" >&2
	exit 1
}
command -v callgrind_annotate >/dev/null 2>&1 || {
	echo "profile.sh: callgrind_annotate nicht gefunden (Teil von valgrind)" >&2
	exit 1
}

C_BIN="$(pwd)/build/minilink"
[ -x "$C_BIN" ] || { echo "profile.sh: $C_BIN fehlt -- zuerst ./build_and_test.sh ausfuehren" >&2; exit 1; }
[ -f test/c/g/main.o ] || { echo "profile.sh: test/c/g/*.o fehlt -- zuerst ./build_and_test.sh ausfuehren" >&2; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "==> Rust-Profiling-Build (mit Debug-Symbolen, ungestrippt)"
RUSTFLAGS="-Cstrip=none -Cdebuginfo=2" cargo build --release \
	--manifest-path src/rust/Cargo.toml --target-dir "$TMP/rs-target" >&2
RS_BIN="$TMP/rs-target/release/minilink"

SUMMARY="${GITHUB_STEP_SUMMARY:-}"

emit() {
	echo "$1"
	[ -n "$SUMMARY" ] && echo "$1" >> "$SUMMARY"
	return 0
}

emit "## minilink: C vs. Rust — Instruktionsprofil (valgrind --tool=callgrind)"
emit ""
emit "Top-15-Funktionen nach Anteil an den insgesamt ausgefuehrten"
emit "Instruktionen (\`Ir\`), Variante \`g\` (\`-T test/default.ldl --debug\`)."
emit "Deterministisch (Instruktionszaehler statt Wall-Clock-Sampling)."
emit ""

profile_one() {
	local name="$1" bin="$2"
	local out="$TMP/cg_$name.out"

	echo "==> Profiling: $name"
	valgrind --tool=callgrind --callgrind-out-file="$out" --quiet \
		"$bin" -T test/default.ldl --debug \
		test/c/g/main.o test/c/g/msg.o -o "$TMP/out_$name" >/dev/null

	# awk liest bewusst bis EOF (kein "exit"/kein "head", das die Pipe
	# vorzeitig schliesst) -- sonst bricht callgrind_annotate mit SIGPIPE
	# ab, was set -o pipefail als Fehler werten wuerde.
	local total top
	total="$(callgrind_annotate "$out" 2>/dev/null | awk '/PROGRAM TOTALS/ && !d {print $1; d=1}')"
	top="$(callgrind_annotate "$out" 2>/dev/null | awk '/file:function/{f=1;next} f{if(n<15){print; n++}}')"

	emit "### $name (insgesamt $total Instruktionen)"
	emit ""
	emit '```'
	emit "$top"
	emit '```'
	emit ""
}

profile_one "C"    "$C_BIN"
profile_one "Rust" "$RS_BIN"
