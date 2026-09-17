#!/usr/bin/env bash
#
# bench.sh — vergleicht die Ausfuehrungszeit von C- und Rust-minilink beim
# Linken der drei Testvarianten (none/lsl/g) mit hyperfine
# (https://github.com/sharkdp/hyperfine).
#
# perf stat/record funktionieren in stark sandboxed Umgebungen (u.a. dieser
# Dev-Container, viele CI-Runner mit eingeschraenktem perf_event_paranoid)
# oft nicht (perf_event_open wird verweigert) -- hyperfine misst stattdessen
# per Wall-Clock ueber viele Wiederholungen und braucht keine besonderen
# Kernel-Rechte.
#
# Voraussetzung: ./build_and_test.sh wurde bereits erfolgreich ausgefuehrt
# (baut build/minilink und src/rust/target/release/minilink sowie die
# Testvarianten unter test/c/).
#
# Ergebnis geht nach stdout (Markdown-Tabelle je Variante) und zusaetzlich
# nach $GITHUB_STEP_SUMMARY, falls gesetzt (GitHub-Actions-Job-Summary).

set -euo pipefail
cd "$(dirname "$0")"

command -v hyperfine >/dev/null 2>&1 || {
	echo "bench.sh: hyperfine nicht gefunden (apt install hyperfine / brew install hyperfine)" >&2
	exit 1
}

C_BIN="$(pwd)/build/minilink"
RS_BIN="$(pwd)/src/rust/target/release/minilink"
[ -x "$C_BIN" ]  || { echo "bench.sh: $C_BIN fehlt -- zuerst ./build_and_test.sh ausfuehren" >&2; exit 1; }
[ -x "$RS_BIN" ] || { echo "bench.sh: $RS_BIN fehlt -- zuerst ./build_and_test.sh ausfuehren" >&2; exit 1; }

SUMMARY="${GITHUB_STEP_SUMMARY:-}"

emit() {
	echo "$1"
	[ -n "$SUMMARY" ] && echo "$1" >> "$SUMMARY"
	return 0
}

emit "## minilink: C vs. Rust -- Linkzeit (hyperfine)"
emit ""
emit "Gleiche Eingabe-Objektdateien (test/c/\$variante/{main,msg}.o) fuer"
emit "beide Linker; Ausgabe jeweils in ein temporaeres Verzeichnis."
emit ""

bench_variant() {
	local v="$1"; shift
	local tmp; tmp="$(mktemp -d)"
	local main_o="$(pwd)/test/c/$v/main.o"
	local msg_o="$(pwd)/test/c/$v/msg.o"
	local extra_args="$*"

	local c_cmd="$C_BIN $extra_args $main_o $msg_o -o $tmp/out_c"
	local rs_cmd="$RS_BIN $extra_args $main_o $msg_o -o $tmp/out_rs"

	emit "### Variante \`$v\` (\`$extra_args\`)"
	emit ""

	hyperfine --warmup 20 -N \
		-n "C"    "$c_cmd" \
		-n "Rust" "$rs_cmd" \
		--export-markdown "$tmp/bench.md"

	cat "$tmp/bench.md"
	[ -n "$SUMMARY" ] && cat "$tmp/bench.md" >> "$SUMMARY"
	emit ""

	rm -rf "$tmp"
}

bench_variant none -T test/default.ldl
bench_variant lsl  --lsl test/tc27x.lsl
bench_variant g    -T test/default.ldl --debug
