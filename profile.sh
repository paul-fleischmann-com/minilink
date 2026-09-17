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
# $GITHUB_STEP_SUMMARY, falls gesetzt. Zusaetzlich strukturiertes JSON
# (Gesamt-Instruktionen + alle von callgrind_annotate gelisteten
# Funktionen) nach profile-results/current.json -- Grundlage fuer den
# Vorher/Nachher-Vergleich in changelog_entry.sh.

set -euo pipefail
cd "$(dirname "$0")"

command -v jq >/dev/null 2>&1 || {
	echo "profile.sh: jq nicht gefunden (apt install jq)" >&2
	exit 1
}

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

RESULTS_DIR="profile-results"
mkdir -p "$RESULTS_DIR"
C_JSON="$TMP/c.json"
RUST_JSON="$TMP/rust.json"

# callgrind_annotate gibt mehrere Abschnitte aus (globale Funktionstabelle,
# danach je Quelldatei annotierter Sourcecode -- dort tauchen dieselben
# Funktionsnamen nochmal mit voellig anderen (inklusiven) Zahlen auf). Diese
# State-Machine schneidet NUR die erste Tabelle heraus (zwischen den beiden
# Trennlinien direkt nach der "Ir  file:function"-Kopfzeile).
annotate_table() {
	awk '
		BEGIN { state = 0 }
		state == 0 && /file:function/ { state = 1; next }
		state == 1 && /^-+$/          { state = 2; next }
		state == 2 && /^-+$/          { state = 3; next }
		state == 2                    { print }
	'
}

profile_one() {
	local name="$1" bin="$2" json_out="$3"
	local out="$TMP/cg_$name.out"

	echo "==> Profiling: $name"
	valgrind --tool=callgrind --callgrind-out-file="$out" --quiet \
		"$bin" -T test/default.ldl --debug \
		test/c/g/main.o test/c/g/msg.o -o "$TMP/out_$name" >/dev/null

	# awk liest bewusst bis EOF (kein "exit", das die Pipe vorzeitig
	# schliesst) -- sonst bricht callgrind_annotate mit SIGPIPE ab, was
	# set -o pipefail als Fehler werten wuerde.
	local total_raw total top functions_tsv
	total_raw="$(callgrind_annotate "$out" 2>/dev/null | awk '/PROGRAM TOTALS/ && !d {print $1; d=1}')"
	total="${total_raw//,/}"
	top="$(callgrind_annotate "$out" 2>/dev/null | annotate_table | awk '{if(n<15){print; n++}}')"

	# Alle von callgrind_annotate in der ersten Tabelle gelisteten
	# Funktionen (nicht nur Top 15) als "instruktionen<TAB>funktionsname"
	# -- fuer den JSON-Export/Vergleich. Funktionsname = Text nach dem
	# LETZTEN ':' vor einem optionalen " [binaerpfad]"-Suffix (Dateipfade
	# koennen selbst ':' enthalten).
	functions_tsv="$(callgrind_annotate "$out" 2>/dev/null \
		| annotate_table \
		| sed -E 's/^[[:space:]]*([0-9,]+)[^:]*:([^ ]+).*/\1\t\2/' \
		| awk -F'\t' 'NF==2{gsub(",","",$1); print $1"\t"$2}')"

	# Gleicher Funktionsname kann mehrfach auftauchen (z.B. an mehreren
	# Inlining-/Aufrufstellen) -- Werte je Name aufsummieren statt den
	# letzten Treffer zu behalten.
	jq -n --argjson total "$total" \
		--arg tsv "$functions_tsv" \
		'{total_ir: $total,
		  functions: (
		    [$tsv | split("\n")[] | select(length>0) | split("\t") | {name: .[1], ir: (.[0]|tonumber)}]
		    | group_by(.name)
		    | map({(.[0].name): (map(.ir) | add)})
		    | add // {}
		  )}' \
		> "$json_out"

	emit "### $name (insgesamt $total_raw Instruktionen)"
	emit ""
	emit '```'
	emit "$top"
	emit '```'
	emit ""
}

profile_one "C"    "$C_BIN" "$C_JSON"
profile_one "Rust" "$RS_BIN" "$RUST_JSON"

commit="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
generated_at="$(date -u +%FT%TZ)"
jq -n --arg commit "$commit" --arg generated_at "$generated_at" \
	--slurpfile c "$C_JSON" --slurpfile rust "$RUST_JSON" \
	'{commit: $commit, generated_at: $generated_at, linkers: {C: $c[0], Rust: $rust[0]}}' \
	> "$RESULTS_DIR/current.json"

echo "==> Ergebnis: $RESULTS_DIR/current.json"
