#!/usr/bin/env bash
#
# changelog_entry.sh <baseline.json> <current.json> <md|adoc>
#
# Vergleicht zwei profile.sh-Ergebnisse (siehe profile.sh: Gesamt-
# Instruktionen + Funktionsliste je Linker) und gibt EINEN
# Changelog-Eintrag im gewuenschten Format (Markdown oder AsciiDoc) auf
# stdout aus: Datum/Commits, Gesamt-Delta je Linker (C/Rust) mit
# Besser/Schlechter-Kennzeichnung, sowie ein Delta je Funktion fuer alle
# Funktionen, die in mindestens einem der beiden Laeufe auftauchen (neue
# Funktionen als "neu", verschwundene als "entfernt").
#
# Fehlt/leert baseline.json (z.B. beim allerersten Lauf), wird ein
# Kurzeintrag ohne Vergleich erzeugt.
#
# "Besser" heisst: weniger Instruktionen (Ir) -- niedriger ist schneller.

set -euo pipefail

BASELINE="${1:?usage: changelog_entry.sh <baseline.json> <current.json> <md|adoc>}"
CURRENT="${2:?usage: changelog_entry.sh <baseline.json> <current.json> <md|adoc>}"
FORMAT="${3:?usage: changelog_entry.sh <baseline.json> <current.json> <md|adoc>}"

case "$FORMAT" in
	md|adoc) ;;
	*) echo "changelog_entry.sh: Format muss 'md' oder 'adoc' sein (bekommen: $FORMAT)" >&2; exit 1 ;;
esac

command -v jq >/dev/null 2>&1 || { echo "changelog_entry.sh: jq nicht gefunden" >&2; exit 1; }
[ -s "$CURRENT" ] || { echo "changelog_entry.sh: $CURRENT fehlt oder ist leer" >&2; exit 1; }

date_str="$(date -u +%F)"
new_commit="$(jq -r '.commit // "?"' "$CURRENT")"
have_baseline=0
[ -s "$BASELINE" ] && have_baseline=1

# --- Ausgabe-Helfer: einmal je Format definiert, danach formatneutral. ---
if [ "$FORMAT" = md ]; then
	h2()      { printf '## %s\n\n' "$1"; }
	h3()      { printf '### %s\n\n' "$1"; }
	para()    { printf '%s\n\n' "$1"; }
	tbl_hdr() { printf '| %s |\n' "$(printf '%s | ' "$@" | sed 's/ | $//')"; printf '|%s\n' "$(printf -- '---|%.0s' "$@")"; }
	tbl_row() { printf '| %s |\n' "$(printf '%s | ' "$@" | sed 's/ | $//')"; }
else
	h2()      { printf '== %s\n\n' "$1"; }
	h3()      { printf '=== %s\n\n' "$1"; }
	para()    { printf '%s\n\n' "$1"; }
	tbl_hdr() { printf '[cols="%s",options="header"]\n|===\n' "$(printf '1,%.0s' "$@" | sed 's/,$//')"; printf '| %s\n' "$(printf '%s | ' "$@" | sed 's/ | $//' | sed 's/ | /\n| /g')"; }
	tbl_row() { printf '| %s\n' "$(printf '%s | ' "$@" | sed 's/ | $//' | sed 's/ | /\n| /g')"; }
	tbl_end() { printf '|===\n\n'; }
fi
[ "$FORMAT" = md ] && tbl_end() { printf '\n'; }

h2 "$date_str -- Commit \`$new_commit\`"

if [ "$have_baseline" -eq 0 ]; then
	para "_Erstlauf (kein vorheriges Profiling-Ergebnis im Cache) -- kein Vergleich moeglich._"
	for linker in C Rust; do
		total="$(jq -r --arg l "$linker" '.linkers[$l].total_ir // "?"' "$CURRENT")"
		para "**$linker**: $total Instruktionen (Ir), Variante \`g\`."
	done
	exit 0
fi

old_commit="$(jq -r '.commit // "?"' "$BASELINE")"
para "Vergleich zu Commit \`$old_commit\`."

for linker in C Rust; do
	old_total="$(jq -r --arg l "$linker" '.linkers[$l].total_ir // empty' "$BASELINE")"
	new_total="$(jq -r --arg l "$linker" '.linkers[$l].total_ir // empty' "$CURRENT")"
	if [ -z "$old_total" ] || [ -z "$new_total" ]; then
		h3 "$linker"
		para "_Keine Daten in einem der beiden Laeufe._"
		continue
	fi

	delta=$((new_total - old_total))
	pct="$(awk -v o="$old_total" -v d="$delta" 'BEGIN{ if (o==0) printf "n/a"; else printf "%+.2f%%", (d/o)*100 }')"
	if   [ "$delta" -lt 0 ]; then mark="🟢 besser"
	elif [ "$delta" -gt 0 ]; then mark="🔴 schlechter"
	else                          mark="⚪ unveraendert"; fi

	h3 "$linker -- Gesamt: $mark"
	tbl_hdr "Metrik" "vorher" "jetzt" "Delta"
	tbl_row "Instruktionen (Ir)" "$old_total" "$new_total" "$delta ($pct)"
	tbl_end

	# Vereinigung aller Funktionsnamen aus beiden Laeufen (jq mit zwei
	# Dateien wertet den Filter nacheinander je Datei aus -> Keys aus
	# beiden Laeufen, sort -u dedupliziert).
	fn_list="$(jq -r --arg l "$linker" '(.linkers[$l].functions // {}) | keys[]' "$BASELINE" "$CURRENT" 2>/dev/null | sort -u)"

	rows="$(
		while IFS= read -r fn; do
			[ -z "$fn" ] && continue
			ov="$(jq -r --arg l "$linker" --arg f "$fn" '.linkers[$l].functions[$f] // empty' "$BASELINE")"
			nv="$(jq -r --arg l "$linker" --arg f "$fn" '.linkers[$l].functions[$f] // empty' "$CURRENT")"
			if [ -n "$ov" ] && [ -n "$nv" ]; then
				d=$((nv - ov))
				[ "$d" -eq 0 ] && continue
				if [ "$d" -lt 0 ]; then m="🟢"; else m="🔴"; fi
				printf '%d\t%s\t%s\t%s\t%s\t%s\n' "$d" "$fn" "$ov" "$nv" "$d" "$m"
			elif [ -n "$nv" ]; then
				printf '%d\t%s\t%s\t%s\t%s\t%s\n' "999999999" "$fn" "-" "$nv" "neu" "🆕"
			else
				printf '%d\t%s\t%s\t%s\t%s\t%s\n' "-999999999" "$fn" "$ov" "-" "entfernt" "➖"
			fi
		done <<< "$fn_list" | sort -t $'\t' -k1,1nr
	)"

	if [ -z "$rows" ]; then
		para "_Keine Aenderung bei den erfassten Funktionen._"
	else
		h3="Funktionen mit Aenderung"
		if [ "$FORMAT" = md ]; then printf '#### %s\n\n' "$h3"; else printf '==== %s\n\n' "$h3"; fi
		tbl_hdr "Funktion" "vorher" "jetzt" "Delta" ""
		while IFS=$'\t' read -r _sort fn ov nv d m; do
			tbl_row "\`$fn\`" "$ov" "$nv" "$d" "$m"
		done <<< "$rows"
		tbl_end
	fi
done
