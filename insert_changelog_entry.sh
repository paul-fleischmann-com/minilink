#!/usr/bin/env bash
#
# insert_changelog_entry.sh <changelog-datei> <entry-datei> <marker-text>
#
# Fuegt den Inhalt von <entry-datei> direkt NACH der ersten Zeile ein, die
# <marker-text> enthaelt (die Marker-Zeile selbst bleibt stehen, damit der
# naechste Lauf an derselben Stelle wieder einfuegen kann -- neueste
# Eintraege landen so immer oben).

set -euo pipefail

FILE="${1:?usage: insert_changelog_entry.sh <changelog-datei> <entry-datei> <marker-text>}"
ENTRY="${2:?usage: insert_changelog_entry.sh <changelog-datei> <entry-datei> <marker-text>}"
MARKER="${3:?usage: insert_changelog_entry.sh <changelog-datei> <entry-datei> <marker-text>}"

[ -f "$FILE" ]  || { echo "insert_changelog_entry.sh: $FILE fehlt" >&2; exit 1; }
[ -f "$ENTRY" ] || { echo "insert_changelog_entry.sh: $ENTRY fehlt" >&2; exit 1; }

tmp="$(mktemp)"
awk -v entryfile="$ENTRY" -v marker="$MARKER" '
	{ print }
	index($0, marker) {
		while ((getline line < entryfile) > 0) print line
	}
' "$FILE" > "$tmp"
mv "$tmp" "$FILE"
