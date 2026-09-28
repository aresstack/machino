#!/bin/sh
# Download-Helfer fuer die Storage-Seite (machino-cleanup.cgi).
#
# Ein plain-sh-CGI, das seine EIGENE HTTP-Antwort schreibt (wie
# machino-cgi-run.cgi) -- so kann es eine Binaerdatei mit
# Content-Disposition: attachment ausliefern, was eine haserl-HTML-Seite nicht
# sauber kann. Es gibt AUSSCHLIESSLICH Dateien heraus, die dieselbe strenge
# is_reclaimable-Pruefung wie die Seite passieren: kein "..", nur regulaere
# Dateien, nur die bekannten Aufraeum-Fundstellen. Ein Pfad aus dem Netz wird
# so nie zu /etc/shadow o.ae.
#
# Laeuft als CGI (root) unter busybox httpd :85; machinos :80-Front-Door relayt
# /cgi-bin/* dorthin und streamt die Antwort mit Backpressure zurueck (kein
# Ganz-in-den-RAM-Puffern -- wichtig auf der 48-MB-Kamera). Keine OpenIPC-Datei
# wird veraendert; uninstall entfernt diese Datei.

_fail() {
	printf 'HTTP/1.1 %s\nContent-Type: text/plain\nCache-Control: no-store\nConnection: close\n\n%s\n' "$1" "$2"
	exit 0
}

is_reclaimable() {
	_p=$1
	case "$_p" in *..*) return 1 ;; esac
	[ -f "$_p" ] || return 1
	case "$_p" in
		/etc/machino/backup/*)          return 0 ;;
		/etc/machino/models/*)          return 0 ;;
		/usr/bin/machino.old.*)         return 0 ;;
		/usr/bin/machino.machino-new.*) return 0 ;;
		/usr/bin/machino.*-new.*)       return 0 ;;
		/root/.ash_history)             return 0 ;;
		/root/m11a/*)                   return 0 ;;
	esac
	return 1
}

# path= aus QUERY_STRING ziehen und urldecodieren (awk, wie machino-cgi-run.cgi:
# busybox awk hat kein strtonum, dash kein printf '%b' "\xHH").
_raw=$QUERY_STRING
case "$_raw" in
	*path=*) _raw=${_raw#*path=}; _raw=${_raw%%&*} ;;
	*) _fail "400 Bad Request" "no path" ;;
esac
_path=$(printf '%s' "$_raw" | awk '
	function h2d(s,   i,c,n){n=0;for(i=1;i<=length(s);i++){c=index("0123456789abcdef",tolower(substr(s,i,1)))-1;if(c<0)return -1;n=n*16+c}return n}
	{gsub(/\+/," ");out="";n=length($0);i=1;while(i<=n){c=substr($0,i,1);if(c=="%"&&i+2<=n){d=h2d(substr($0,i+1,2));if(d>=0){out=out sprintf("%c",d);i+=3;continue}}out=out c;i++}printf "%s",out}')

is_reclaimable "$_path" || _fail "404 Not Found" "not a cleanup file"

_name=$(basename "$_path")
_size=$(wc -c < "$_path" 2>/dev/null)
case "$_size" in ''|*[!0-9]*) _fail "500 Internal Server Error" "cannot size file" ;; esac

printf 'HTTP/1.1 200 OK\nContent-Type: application/octet-stream\nContent-Disposition: attachment; filename="%s"\nContent-Length: %s\nCache-Control: no-store\nConnection: close\n\n' "$_name" "$_size"
cat "$_path"
