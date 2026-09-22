#!/bin/sh
# Cheap guard for the sources this dev host cannot compile (they need POSIX
# sockets, so only CI ever sees them). It does not try to be a compiler - it
# catches exactly the two mistakes that have already cost CI round trips.
set -e
FILES="src/app/http/http_server.cpp src/app/rtsp/rtsp_server.cpp src/app/rtsp/rtsp_auth.cpp src/app/webrtc/peer.cpp src/app/main.cpp"
bad=0

# 0. 64-bit atomics anywhere in the tree: MIPS32 has no lock-free 64-bit
#    atomics, so these need libatomic and fail the cross link. Hit twice.
if grep -rn --include=*.hpp --include=*.cpp -E "atomic<[[:space:]]*(u?int64_t|unsigned long long|long long|size_t)[[:space:]]*>" src/ tests/ 2>/dev/null | grep -v "://" | grep -v ": *//"; then
    echo "  ^ 64-bit std::atomic does not link on MIPS32 (no lock-free 8-byte ops)"
    bad=1
fi
for f in $FILES; do
    [ -f "$f" ] || continue

    # 1. ApplyMode lives in namespace machino, NOT machino::power (hit twice).
    if grep -n 'power::ApplyMode' "$f"; then
        echo "  ^ $f: ApplyMode is in namespace machino, not machino::power"
        bad=1
    fi

    # 2. An odd number of unescaped double quotes on a line: in C++ that means
    #    a string literal ran off the end of the line, which is exactly what a
    #    code generator emitting a real newline instead of \n produces (hit
    #    once, six compiler errors deep). Escaped quotes, escaped backslashes
    #    and character literals are removed first; whole-line comments are
    #    skipped because prose may legitimately span a quote.
    awk -v file="$f" '
        {
            match($0, /^[ \t]*/)
            if (substr($0, RSTART + RLENGTH, 2) == "//") next

            line = $0
            gsub(/\\\\/, "", line)      # escaped backslashes first
            gsub(/\\"/, "", line)       # then escaped quotes
            for (;;) {                  # character literals, e.g. the quote char
                p = index(line, "\x27")
                if (p == 0 || p + 2 > length(line)) break
                if (substr(line, p + 2, 1) != "\x27") break
                line = substr(line, 1, p - 1) substr(line, p + 3)
            }
            n = gsub(/"/, "&", line)
            if (n % 2 == 1) {
                printf "%s:%d: odd number of quotes - string literal not closed on this line\n", file, NR
                rc = 1
            }
        }
        END { exit rc ? 1 : 0 }
    ' "$f" || bad=1
done
[ "$bad" = 0 ] || { echo "check-linux-only: FAILED"; exit 1; }
echo "check-linux-only: ok"
