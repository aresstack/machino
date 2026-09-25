#!/bin/sh
# Das CGI-Shim fuer OpenIPCs JSON-Backends unter cgi-bin/j/.
#
# WARUM ES DEN GIBT
#
# Die Stock-Skripte /var/www/cgi-bin/j/*.cgi lesen ihre Parameter NICHT aus
# QUERY_STRING, sondern aus Umgebungsvariablen GET_<schluessel> (Query) und
# POST_<schluessel> (urlencodeter Body). Das setzt majestics eigener httpd;
# busybox httpd (der unter machino die interne WebUI auf :85 serviert) setzt nur
# QUERY_STRING/REQUEST_METHOD/CONTENT_* und reicht den Body auf stdin. Folge
# unter machino (gemessen 2026-09-26): files.cgi ignoriert cd=<dir> und listet
# stur /, download.cgi antwortet "not a file" (GET_path leer) -- der File
# Manager ist unbenutzbar, die Console laeuft (run.cgi braucht nur QUERY_STRING).
#
# Der saubere Interpreter-Weg (*.cgi:<runner> in httpd.conf) fiel aus: dieses
# busybox httpd kennt die Direktive nicht und ignoriert sie ("config error").
# Also ein Shim: machinos :80-Front-Door schreibt /cgi-bin/j/<x>.cgi auf
# /cgi-bin/machino-cgi-run.cgi/j/<x>.cgi um (PATH_INFO). busybox fuehrt DIESES
# Skript als ganz normale cgi-bin-CGI aus, fuellt GET_/POST_ und exec't das
# UNVERAENDERTE Zielskript.
#
# WARUM DAS DIE FORK-REGEL EINHAELT
#
# machinod (:80) darf bei lebendem IMP nicht forken (OOM 2026-09-22, stark
# gestuetzt). Das tut es hier auch nicht: es schreibt nur die relayte Anfrage
# um. Geforkt wird unter busybox httpd auf :85 -- einem kleinen Prozess, der
# ohnehin pro Anfrage forkt (er fuehrt so alle haserl-Seiten aus).
#
# Eine machino-eigene Datei in /var/www/cgi-bin/, genau wie die machino-*.cgi
# Seiten. Keine OpenIPC-Datei wird veraendert; uninstall entfernt sie.

CGI_DIR=${MACHINO_CGI_DIR:-/var/www/cgi-bin}

_fail() {
    printf 'HTTP/1.1 %s\nContent-Type: text/plain\nCache-Control: no-store\n\n%s\n' "$1" "$2"
    exit 0
}

# Das Ziel kommt aus PATH_INFO (/j/<name>.cgi). Genau eine Ebene unter j/,
# nichts mit .. oder doppelten Slashes -- ein Name aus dem Netz wird nie zu
# einem Pfad ausserhalb von cgi-bin/j/.
_t=$PATH_INFO
case "$_t" in
    /j/*.cgi) ;;
    *) _fail "400 Bad Request" "machino-cgi-run: unexpected target '$_t'" ;;
esac
case "$_t" in
    */../*|*/..|*//*|/j/*/*) _fail "400 Bad Request" "machino-cgi-run: bad target '$_t'" ;;
esac
_script="$CGI_DIR$_t"
[ -f "$_script" ] || _fail "404 Not Found" "machino-cgi-run: no such cgi '$_t'"

# Ein urlencodetes Feld dekodieren. Reihenfolge im sed: erst vorhandene
# Backslashes verdoppeln (sonst deutet printf %b sie), dann + -> Leerzeichen,
# dann %XX -> \xXX.
_decode() {
    printf '%b' "$(printf '%s' "$1" | sed 's/\\/\\\\/g; s/+/ /g; s/%\([0-9a-fA-F][0-9a-fA-F]\)/\\x\1/g')"
}

# key=value-Paare aus einer &-getrennten Zeichenkette in <prefix>_<key> setzen.
# Nur syntaktisch gueltige Schluessel (Buchstabe/_/Ziffer) -- ein Feldname aus
# dem Netz wird nie zu Code.
_parse() {
    _pfx=$1; _data=$2
    [ -n "$_data" ] || return 0
    _oldifs=$IFS
    IFS='&'
    for _pair in $_data; do
        IFS=$_oldifs
        case "$_pair" in
            *=*) _k=${_pair%%=*}; _v=${_pair#*=} ;;
            *)   _k=$_pair;       _v= ;;
        esac
        case "$_k" in
            ""|*[!A-Za-z0-9_]*) IFS='&'; continue ;;   # ungueltiger Name: ignorieren
        esac
        _v=$(_decode "$_v")
        eval "${_pfx}_${_k}=\$_v"
        export "${_pfx}_${_k}"
        IFS='&'
    done
    IFS=$_oldifs
}

_parse GET "$QUERY_STRING"

# POST_-Felder NUR bei urlencodetem Body. Alles andere (text/plain wie bei
# save.cgi, octet-stream, multipart) bleibt UNANGETASTET auf stdin -- diese
# Skripte lesen den Rohbody selbst (`cat > $f`).
if [ "$REQUEST_METHOD" = "POST" ]; then
    case "$CONTENT_TYPE" in
        *application/x-www-form-urlencoded*)
            _len=${CONTENT_LENGTH:-0}
            case "$_len" in ''|*[!0-9]*) _len=0 ;; esac
            if [ "$_len" -gt 0 ]; then
                _body=$(head -c "$_len" 2>/dev/null)
                _parse POST "$_body"
            fi
            ;;
    esac
fi

exec "$_script"
