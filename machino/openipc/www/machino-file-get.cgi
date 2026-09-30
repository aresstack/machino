#!/bin/sh
# Datei-Download fuer OpenIPCs File Manager unter Machino.
#
# majestic liefert eine Datei ausserhalb des Webroots direkt ueber ihren
# absoluten Pfad aus (GET /mnt/sda1/x.bin) -- so laedt der File Manager
# herunter und so liest sein Editor. busybox httpd, das unter Machino die
# WebUI auf :85 serviert, kennt nur /var/www. Also schreibt Machinos
# Front-Door einen solchen GET auf DIESES CGI um (file_get_rewrite in
# http_parse.cpp: nur wenn der Pfad nicht im Webroot liegt UND eine regulaere
# Datei ist), und das Relay streamt die Antwort mit Backpressure zurueck --
# ohne Groessen- und Zeitdeckel des Relays, denn die Laenge steht im Kopf.
#
# Ein plain-sh-CGI mit EIGENER HTTP-Antwort (wie machino-cleanup-dl.cgi).
# Riegel: absoluter Pfad, kein "..", keine Pseudo-Dateisysteme (/proc, /sys,
# /dev -- cat /dev/zero endet nie), nur regulaere Dateien. Alles andere ist
# die Kamera des Betreibers, wie bei majestic; die Anmeldung davor ist
# dieselbe wie fuer jedes /cgi-bin (httpd.conf). Medien inline, damit der
# File Manager sie zeigt; alles andere als attachment, damit der Browser
# nichts aus der Kamera-Origin ausfuehrt (majestic pinnt das genauso).
#
# Kein Range: ein Video laeuft progressiv, Spulen laedt neu. Keine
# OpenIPC-Datei wird veraendert; uninstall entfernt diese Datei.

_fail() {
	printf 'HTTP/1.1 %s\nContent-Type: text/plain\nCache-Control: no-store\nConnection: close\n\n%s\n' "$1" "$2"
	exit 0
}

[ "$REQUEST_METHOD" = "GET" ] || [ "$REQUEST_METHOD" = "HEAD" ] || _fail "405 Method Not Allowed" "GET only"

# path= aus QUERY_STRING, %XX-dekodiert (KEIN '+' -> Leerzeichen: das ist ein
# Pfad, kein Formularfeld; ein '+' im Dateinamen bleibt ein '+').
_raw=$QUERY_STRING
case "$_raw" in
	*path=*) _raw=${_raw#*path=}; _raw=${_raw%%&*} ;;
	*) _fail "400 Bad Request" "no path" ;;
esac
_path=$(printf '%s' "$_raw" | awk '
	function h2d(s,   i,c,n){n=0;for(i=1;i<=length(s);i++){c=index("0123456789abcdef",tolower(substr(s,i,1)))-1;if(c<0)return -1;n=n*16+c}return n}
	{out="";n=length($0);i=1;while(i<=n){c=substr($0,i,1);if(c=="%"&&i+2<=n){d=h2d(substr($0,i+1,2));if(d>=0){out=out sprintf("%c",d);i+=3;continue}}out=out c;i++}printf "%s",out}')

case "$_path" in
	/*) ;;
	*) _fail "400 Bad Request" "path must be absolute" ;;
esac
case "$_path" in *..*) _fail "400 Bad Request" "path must not contain .." ;; esac
case "$_path" in
	/proc|/proc/*|/sys|/sys/*|/dev|/dev/*) _fail "403 Forbidden" "not a file store: $_path" ;;
esac
[ -f "$_path" ] || _fail "404 Not Found" "not a file: $_path"
[ -r "$_path" ] || _fail "403 Forbidden" "not readable: $_path"

_name=$(basename "$_path")
_safe=$(printf '%s' "$_name" | tr -d '"\\\r\n')
_size=$(wc -c < "$_path" 2>/dev/null)
case "$_size" in ''|*[!0-9]*) _fail "500 Internal Server Error" "cannot size file" ;; esac
_ext=$(printf '%s' "$_name" | sed -n 's/.*\.\([A-Za-z0-9]*\)$/\1/p' | tr 'A-Z' 'a-z')
case "$_ext" in
	mp4|m4v)  _ct=video/mp4;         _disp=inline ;;
	mkv)      _ct=video/x-matroska;  _disp=inline ;;
	webm)     _ct=video/webm;        _disp=inline ;;
	mov)      _ct=video/quicktime;   _disp=inline ;;
	avi)      _ct=video/x-msvideo;   _disp=inline ;;
	jpg|jpeg) _ct=image/jpeg;        _disp=inline ;;
	png)      _ct=image/png;         _disp=inline ;;
	gif)      _ct=image/gif;         _disp=inline ;;
	webp)     _ct=image/webp;        _disp=inline ;;
	mp3)      _ct=audio/mpeg;        _disp=inline ;;
	wav)      _ct=audio/wav;         _disp=inline ;;
	aac)      _ct=audio/aac;         _disp=inline ;;
	m4a)      _ct=audio/mp4;         _disp=inline ;;
	ogg|opus) _ct=audio/ogg;         _disp=inline ;;
	*)        _ct=application/octet-stream; _disp=attachment ;;
esac

printf 'HTTP/1.1 200 OK\nContent-Type: %s\nContent-Disposition: %s; filename="%s"\nContent-Length: %s\nCache-Control: no-store\nConnection: close\n\n' "$_ct" "$_disp" "$_safe" "$_size"
[ "$REQUEST_METHOD" = "HEAD" ] && exit 0
cat "$_path"
