#!/bin/sh
# Modell-Upload fuer die KI-Seite (machino-ai.cgi).
#
# Ein plain-sh-CGI, das den rohen POST-Body (ein .tgz Modell-Bundle) von stdin
# liest -- wie OpenIPCs save.cgi, nicht ueber machino-cgi-run. Es prueft das
# Bundle STRENG, bevor irgendetwas nach /etc/machino/models wandert:
#   * kein Pfad-Traversal (".."/absolut) im Archiv -> sonst koennte tar aus dem
#     Temp-Verzeichnis ausbrechen,
#   * ein manifest.json mit backend "venus-nna" und nnaGeneration "nna1"
#     (machinos NNA-Detektor lehnt fremde Modelle sonst per Manifest-Gate ab),
#   * eine Modell-.bin,
#   * genug Overlay-Platz (das Overlay ist eng -- sonst Hinweis auf Storage).
#
# Ziel: /etc/machino/models -- oder mit ?dest=<Mountpunkt> die gemountete
# Karte (<Mountpunkt>/models), was die KI-Seite vorbelegt, sobald ein Medium
# unter /mnt haengt: der 7,6-MB-yolov5s passt auf das Overlay nur, wenn man
# vorher Platz schafft, auf die Karte immer. dest muss ein Mountpunkt unter
# /mnt aus /proc/mounts sein, rw -- kein Pfad aus dem Netz wird zum Ziel.
#
# Laeuft als CGI (root) unter busybox httpd :85; machinos :80-Front-Door relayt
# /cgi-bin/* dorthin. Keine OpenIPC-Datei wird veraendert; uninstall entfernt es.

_reply() { printf 'HTTP/1.1 %s\nContent-Type: text/plain\nCache-Control: no-store\nConnection: close\n\n%s\n' "$1" "$2"; exit 0; }

[ "$REQUEST_METHOD" = "POST" ] || _reply "405 Method Not Allowed" "POST a .tgz model bundle."
_len=${CONTENT_LENGTH:-0}
case "$_len" in ''|*[!0-9]*) _reply "411 Length Required" "missing Content-Length" ;; esac
[ "$_len" -gt 0 ] || _reply "400 Bad Request" "empty upload"
_MAX=$((8 * 1024 * 1024))
[ "$_len" -le "$_MAX" ] || _reply "413 Payload Too Large" "model bundle too large (max 8 MB)"

MODELS=/etc/machino/models
_where="the overlay"
_dest=""
case "$QUERY_STRING" in
	*dest=*) _dest=${QUERY_STRING#*dest=}; _dest=${_dest%%&*} ;;
esac
if [ -n "$_dest" ]; then
	_dest=$(printf '%s' "$_dest" | awk '
		function h2d(s,   i,c,n){n=0;for(i=1;i<=length(s);i++){c=index("0123456789abcdef",tolower(substr(s,i,1)))-1;if(c<0)return -1;n=n*16+c}return n}
		{out="";n=length($0);i=1;while(i<=n){c=substr($0,i,1);if(c=="%"&&i+2<=n){d=h2d(substr($0,i+1,2));if(d>=0){out=out sprintf("%c",d);i+=3;continue}}out=out c;i++}printf "%s",out}')
	case "$_dest" in
		/mnt/*) ;;
		*) _reply "400 Bad Request" "dest must be a mount point under /mnt" ;;
	esac
	case "$_dest" in *..*|*' '*) _reply "400 Bad Request" "dest is not a valid mount point" ;; esac
	# MACHINO_MOUNTS: Testnaht (Host-Test ohne echtes Medium).
	awk -v d="$_dest" '$2==d && $4 ~ /(^|,)rw(,|$)/ {f=1} END{exit !f}' "${MACHINO_MOUNTS:-/proc/mounts}" ||
		_reply "400 Bad Request" "$_dest is not a mounted, writable medium (see Storage)"
	MODELS="$_dest/models"
	_where="the card at $_dest"
fi
TMP=/tmp/machino-model-up
rm -rf "$TMP"; mkdir -p "$TMP/x" || _reply "500 Internal Server Error" "no temp space"

# Genau CONTENT_LENGTH Bytes von stdin in die Datei.
head -c "$_len" > "$TMP/up.tgz" 2>/dev/null
[ -s "$TMP/up.tgz" ] || { rm -rf "$TMP"; _reply "400 Bad Request" "empty upload"; }

# Erst die Archiv-EINTRAEGE ansehen (busybox tar hat kein -z -> gunzip davor).
# Traversal/absolute Pfade => Ablehnen, BEVOR extrahiert wird.
_names=$(gunzip -c "$TMP/up.tgz" 2>/dev/null | tar t 2>/dev/null)
[ -n "$_names" ] || { rm -rf "$TMP"; _reply "400 Bad Request" "not a valid .tgz archive"; }
if printf '%s\n' "$_names" | grep -qE '(^|/)\.\.(/|$)|^/'; then
	rm -rf "$TMP"; _reply "400 Bad Request" "archive contains unsafe paths"
fi

gunzip -c "$TMP/up.tgz" 2>/dev/null | tar x -C "$TMP/x" 2>/dev/null || { rm -rf "$TMP"; _reply "400 Bad Request" "could not extract archive"; }

_mf=$(find "$TMP/x" -type f -name manifest.json 2>/dev/null | head -1)
[ -f "$_mf" ] || { rm -rf "$TMP"; _reply "400 Bad Request" "bundle has no manifest.json"; }
grep -q '"backend"[^,}]*venus-nna' "$_mf" || { rm -rf "$TMP"; _reply "400 Bad Request" "manifest backend is not venus-nna (incompatible model)"; }
grep -q '"nnaGeneration"[^,}]*nna1' "$_mf" || { rm -rf "$TMP"; _reply "400 Bad Request" "manifest NNA generation is not nna1 (incompatible model)"; }

_bin=$(find "$TMP/x" -type f -name '*.bin' 2>/dev/null | head -1)
[ -f "$_bin" ] || { rm -rf "$TMP"; _reply "400 Bad Request" "bundle has no model .bin"; }

# Passt es auf das Ziel? (Das Overlay ist eng -- ehrlich absagen statt es
# vollzuschreiben; eine Karte hat Platz, aber auch das wird gemessen.)
_need=$(du -k "$_bin" "$_mf" 2>/dev/null | awk '{s+=$1} END{print s+64}')
_free=$(df -k "${_dest:-/}" 2>/dev/null | awk 'NR==2{print $4}')
[ "${_free:-0}" -gt "${_need:-0}" ] || { rm -rf "$TMP"; _reply "507 Insufficient Storage" "not enough space on $_where (need ${_need} kB, free ${_free:-0} kB)$([ -z "$_dest" ] && echo '; free some in System > Storage, or upload to the card')"; }

mkdir -p "$MODELS" || { rm -rf "$TMP"; _reply "500 Internal Server Error" "cannot create $MODELS"; }
cp "$_bin" "$MODELS/$(basename "$_bin")" && cp "$_mf" "$MODELS/manifest.json" || { rm -rf "$TMP"; _reply "500 Internal Server Error" "could not write the model to $MODELS"; }
_prov=$(find "$TMP/x" -type f -name provenance.txt 2>/dev/null | head -1)
[ -f "$_prov" ] && cp "$_prov" "$MODELS/provenance.txt"
rm -rf "$TMP"
_reply "200 OK" "Installed model $(basename "$_bin") on $_where ($MODELS). Press Use next to it, then enable the detector."
