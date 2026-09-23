#!/bin/sh
# Host unit tests without make.
#
# `make -C machino test` is the definition of record; this script exists for
# environments that have a C++ compiler but no make (MSYS2/ucrt64 is the one
# that prompted it). To make drift impossible it does not carry its own file
# list -- it reads TEST_SRC out of the Makefile, so adding a test to the
# Makefile is enough for both paths.
#
# Usage: sh tools/host-test.sh [MBEDTLS_ROOT]
#        MBEDTLS may also come from the environment. It must point at an
#        unpacked mbedtls-<ver> containing libmbedall-<cc>.a, i.e. what
#        tools/fetch-mbedtls.sh produces.
set -eu

cd "$(dirname "$0")/.."

MBEDTLS="${1:-${MBEDTLS:-}}"
if [ -z "$MBEDTLS" ]; then
    echo "usage: sh tools/host-test.sh <mbedtls-root>   (or set MBEDTLS)" >&2
    echo "       build one with: sh tools/fetch-mbedtls.sh <dir> gcc ar" >&2
    exit 2
fi

HOST_CXX="${HOST_CXX:-g++}"

# A native (mingw) gcc asks Windows for the temp directory and gets C:\WINDOWS
# when TMP and TEMP are unset -- which is what happens whenever this script is
# started from a shell that has them set but not exported. It then fails with
# "Cannot create temporary file". Give it one we know is writable.
if [ -z "${TMP:-}" ] && [ -z "${TEMP:-}" ] && [ -z "${TMPDIR:-}" ]; then
    mkdir -p build/tmp
    TMPDIR="$(pwd)/build/tmp"; TMP="$TMPDIR"; TEMP="$TMPDIR"
    export TMPDIR TMP TEMP
fi
# Derive the archive name the same way the Makefile does: the compiler is part
# of it so a host and a cross build can share one unpacked tree.
MBEDTLS_LIB="$MBEDTLS/libmbedall-$(basename "$(echo "$HOST_CXX" | sed 's/g++$/gcc/')").a"
[ -f "$MBEDTLS_LIB" ] || { echo "no $MBEDTLS_LIB -- run tools/fetch-mbedtls.sh first" >&2; exit 2; }

# TEST_SRC is a multi-line make variable: take everything from the assignment
# up to the first line that does not end in a backslash, then drop the
# continuations and the variable name.
#
# The CR is deleted first. A checkout with CRLF endings leaves every line
# ending in \r, so the "ends in a backslash" test never matches and the
# backslashes survive into the compiler command line as stray arguments --
# which is exactly how this script failed the first time it was run.
SRC=$(tr -d '\r' < Makefile \
      | sed -n '/^TEST_SRC *:*=/,/[^\\]$/p' \
      | sed -e 's/^TEST_SRC *:*= *//' -e 's/\\$//' \
      | tr '\n' ' ')
[ -n "$SRC" ] || { echo "could not read TEST_SRC from Makefile" >&2; exit 1; }

LIBS="$MBEDTLS_LIB"
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) LIBS="$LIBS -lbcrypt -lws2_32" ;;
esac

CXXFLAGS="-std=c++17 -O1 -fno-exceptions -fno-rtti -Wall -Wextra
 -Wno-unused-parameter -Werror -pthread -Isrc -Itests
 -DMACHINO_VERSION=\"test\" -DMBEDTLS_SSL_DTLS_SRTP -isystem $MBEDTLS/include"

# One translation unit at a time, into build/host/. Two reasons: a re-run after
# touching one file takes a second instead of a minute, and handing ~80 paths
# plus the flags to the compiler in a single argv overflows the command line on
# Windows shells. The link reads its inputs from a response file for the same
# reason.
OUT=build/host
mkdir -p "$OUT"
OBJS=""
for f in $SRC; do
    o="$OUT/$(echo "$f" | tr '/' '_' | sed 's/\.cpp$/.o/')"
    OBJS="$OBJS $o"
    # Up to date only if the object is newer than every prerequisite gcc
    # recorded last time -- headers included. Comparing against the .cpp alone
    # would silently skip a rebuild after a header change, which for a test
    # runner means reporting a pass for code that was never compiled.
    if [ -f "$o" ] && [ -f "$o.d" ] && [ "$o" -nt Makefile ]; then
        stale=""
        for dep in $(tr -d '\\' < "$o.d" | tr ' ' '\n' | grep -v ':$' | grep -v '^$'); do
            [ -f "$dep" ] || { stale=1; break; }
            [ "$o" -nt "$dep" ] || { stale=1; break; }
        done
        [ -n "$stale" ] || continue
    fi
    echo "  CXX $f"
    # shellcheck disable=SC2086
    $HOST_CXX $CXXFLAGS -MMD -MF "$o.d" -c "$f" -o "$o"
done

RSP="$OUT/link.rsp"
: > "$RSP"
for o in $OBJS; do echo "$o" >> "$RSP"; done
# MSYS rewrites /c/... into C:/... when it passes an argument to a native
# program, but it cannot see inside a response file. Convert the library paths
# ourselves, or the linker looks for a directory named "c" that does not exist.
for l in $LIBS; do
    case "$l" in
        -*) echo "$l" >> "$RSP" ;;
        *)  if command -v cygpath >/dev/null 2>&1; then
                cygpath -m "$l" >> "$RSP"
            else
                echo "$l" >> "$RSP"
            fi ;;
    esac
done
echo "  LD  tests/machino-tests"
$HOST_CXX -pthread "@$RSP" -o tests/machino-tests

./tests/machino-tests
sh tests/test_streamerctl.sh
sh tests/test_openipc_install.sh
