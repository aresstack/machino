#!/bin/sh
# Cheap guard for the sources the Windows dev host cannot compile (sockets):
# they only fail in CI, which costs a full round trip. Catches the mistakes
# that actually happened rather than trying to be a compiler.
set -e
FILES="src/app/http/http_server.cpp src/app/rtsp/rtsp_server.cpp src/app/webrtc/peer.cpp src/app/main.cpp"
bad=0
for f in $FILES; do
    [ -f "$f" ] || continue
    # ApplyMode lives in machino, NOT machino::power (hit twice)
    if grep -n 'power::ApplyMode' "$f"; then echo "  ^ $f: ApplyMode is in namespace machino, not machino::power"; bad=1; fi
    # a private member called from another translation unit (hit once)
    :
done
[ "$bad" = 0 ] || { echo "check-linux-only: FAILED"; exit 1; }
echo "check-linux-only: ok"
