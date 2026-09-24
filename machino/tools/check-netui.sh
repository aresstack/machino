#!/bin/sh
# Die Netzwerkseite ist ein einziger String in C++ -- niemand parst sie.
#
# Der Compiler sieht ein Zeichenkettenliteral und ist zufrieden. Ein fehlendes
# Komma im JavaScript zerlegt aber nicht die neue Schaltflaeche, sondern die
# GANZE Seite: es ist ein einziger <script>-Block, und ein Syntaxfehler darin
# fuehrt dazu, dass gar nichts davon laeuft. Das faellt erst in einem Browser
# auf, und dieser Browser ist in diesem Projekt selten zur Hand.
#
# Zwei Pruefungen, beide billig:
#
#   1. Parst das JavaScript ueberhaupt.
#   2. Hat jedes $("...") ein Element mit dieser id. Ein Tippfehler dort gibt
#      null zurueck und wirft erst zur Laufzeit, an einer Stelle, die mit der
#      Ursache nichts zu tun hat.
#
# Was hier NICHT geprueft wird: ob die Seite richtig aussieht oder sich richtig
# verhaelt. Dafuer braucht es einen Browser, und das steht als
# PENDING_BROWSER in docs/pending-physical.md.
set -eu

SRC=${1:-src/app/http/netui.cpp}
[ -r "$SRC" ] || { echo "check-netui: $SRC nicht lesbar" >&2; exit 1; }

TMP=${TMPDIR:-/tmp}/netui-check.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT INT TERM

awk '/<script>/{f=1;next} /<\/script>/{f=0} f' "$SRC" > "$TMP/page.js"
[ -s "$TMP/page.js" ] || { echo "check-netui: kein <script>-Block gefunden" >&2; exit 1; }

fail=0

if command -v node >/dev/null 2>&1; then
    if node --check "$TMP/page.js"; then
        echo "check-netui: JavaScript parst"
    else
        echo "check-netui: das JavaScript der Seite hat einen Syntaxfehler" >&2
        fail=1
    fi
else
    echo "check-netui: kein node - Syntaxpruefung uebersprungen" >&2
fi

# Jede id, die das HTML vergibt.
grep -o 'id="[A-Za-z0-9_-]*"' "$SRC" | sed 's/id="//; s/"//' | sort -u > "$TMP/have"
# Jede id, die das Skript nachschlaegt. Nur die literale Form $("...") --
# berechnete Namen kann diese Pruefung nicht sehen und behauptet es auch nicht.
grep -o '\$("[A-Za-z0-9_-]*")' "$TMP/page.js" | sed 's/\$("//; s/")//' | sort -u > "$TMP/want"

missing=$(comm -13 "$TMP/have" "$TMP/want")
if [ -n "$missing" ]; then
    echo "check-netui: das Skript sucht Elemente, die es nicht gibt:" >&2
    echo "$missing" | sed 's/^/  /' >&2
    fail=1
else
    echo "check-netui: alle $(wc -l < "$TMP/want") nachgeschlagenen ids existieren"
fi

exit $fail
