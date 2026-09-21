#!/bin/sh
# Fetches and builds a PINNED mbedTLS as static libraries - the only third-
# party dependency Machino has, needed exclusively for the WebRTC DTLS
# handshake (certificate, fingerprint, use_srtp, key exporter). SRTP itself,
# STUN, SDP and RTP are Machino's own vector-tested code.
#
#   tools/fetch-mbedtls.sh <build-dir> [CC] [AR]
#
# Leaves headers in <build-dir>/mbedtls-<ver>/include and the three .a in
# <build-dir>/mbedtls-<ver>/library. Idempotent: a finished build is reused.
set -e
VER=3.6.2
SHA256=8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca
DIR=${1:?usage: fetch-mbedtls.sh <build-dir> [CC] [AR]}
CC=${2:-cc}
AR=${3:-ar}
ROOT="$DIR/mbedtls-$VER"
STAMP="$ROOT/.machino-built-$(basename "$CC")"
if [ -f "$STAMP" ]; then echo "mbedtls $VER already built for $CC"; exit 0; fi
mkdir -p "$DIR"
TAR="$DIR/mbedtls-$VER.tar.bz2"
if [ ! -f "$TAR" ]; then
    curl -fL --retry 3 -o "$TAR" \
        "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$VER/mbedtls-$VER.tar.bz2"
fi
echo "$SHA256  $TAR" | sha256sum -c -
# unpack once; per-CC builds (host + cross) share the source tree
[ -d "$ROOT" ] || tar xjf "$TAR" -C "$DIR"
# Build without make (not every dev host has one): every library/*.c into one
# archive. DTLS-SRTP is off in the default config; everything else default.
OBJ="$ROOT/obj-$(basename "$CC")"
mkdir -p "$OBJ"
for f in "$ROOT"/library/*.c; do
    "$CC" -O2 -DMBEDTLS_SSL_DTLS_SRTP -I"$ROOT/include" -I"$ROOT/library" \
        -c "$f" -o "$OBJ/$(basename "${f%.c}").o"
done
"$AR" rcs "$ROOT/libmbedall-$(basename "$CC").a" "$OBJ"/*.o
touch "$STAMP"
echo "mbedtls $VER built: $ROOT/libmbedall-$(basename "$CC").a"
