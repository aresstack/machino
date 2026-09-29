#!/bin/sh
# Fetches and builds the two PINNED audio encoders as static archives:
#
#   libfaac  (AAC-LC)  - /audio.m4a, AAC in /ws/video (mp4a.40.2, what Safari decodes)
#   libopus  (Opus)    - /audio.opus, Opus in /ws/video (what Chrome/Firefox prefer)
#
#   tools/fetch-codecs.sh <build-dir> [CC] [AR]
#
# Leaves <build-dir>/include/{faac.h,opus/*.h} and
# <build-dir>/libfaac-<cc>.a, <build-dir>/libopus-<cc>.a. Pass CODECS=<build-dir>
# to make. Idempotent per compiler: host and cross builds share the sources.
#
# Like fetch-mbedtls.sh it builds without make or meson: every source into one
# archive with the defines the upstream build would set. Integrity: the clones
# are checked out at a pinned COMMIT and the checked-out HEAD is verified, so a
# moved tag cannot swap the code underneath.
#
# Float builds on purpose: the T40 toolchain is hard-float (__mips_hard_float,
# 64-bit FPRs), so the float paths are both faster and the ones upstream tunes.
set -e
FAAC_REPO=https://github.com/knik0/faac.git
FAAC_COMMIT=b92b7f81e53b1027107c900b11609abf32a1fb1a    # the pin build.sh uses (modern faac_encoder_* API)
OPUS_REPO=https://github.com/xiph/opus.git
OPUS_TAG=v1.5.2
OPUS_COMMIT=ddbe48383984d56acd9e1ab6a090c54ca6b735a6

DIR=${1:?usage: fetch-codecs.sh <build-dir> [CC] [AR]}
CC=${2:-cc}
AR=${3:-ar}
TAG=$(basename "$CC")
STAMP="$DIR/.codecs-built-$TAG"
if [ -f "$STAMP" ]; then echo "codecs already built for $CC"; exit 0; fi
mkdir -p "$DIR/include/opus"

verify() {  # <dir> <commit>
    head=$(git -C "$1" rev-parse HEAD)
    [ "$head" = "$2" ] || { echo "ERROR: $1 is at $head, pinned $2" >&2; exit 1; }
}

if [ ! -d "$DIR/faac/.git" ]; then
    git clone --quiet "$FAAC_REPO" "$DIR/faac"
    git -C "$DIR/faac" checkout --quiet "$FAAC_COMMIT"
fi
verify "$DIR/faac" "$FAAC_COMMIT"
if [ ! -d "$DIR/opus/.git" ]; then
    git clone --quiet --depth 1 --branch "$OPUS_TAG" "$OPUS_REPO" "$DIR/opus"
fi
verify "$DIR/opus" "$OPUS_COMMIT"

# ---- faac: libfaac/*.c without the x86-only SSE quantizer ------------------
OBJ="$DIR/obj-faac-$TAG"; mkdir -p "$OBJ"; rm -f "$OBJ"/*.o
for f in "$DIR"/faac/libfaac/*.c; do
    case "$f" in *quantize_sse.c) continue ;; esac
    "$CC" -O2 -fPIC -DPACKAGE_VERSION='"machino"' -DMAX_CHANNELS=2 -DFAAC_SBR_DECIMATION=1 \
        -I"$DIR/faac/include" -I"$DIR/faac/libfaac" -c "$f" -o "$OBJ/$(basename "${f%.c}").o"
done
rm -f "$DIR/libfaac-$TAG.a"
"$AR" rcs "$DIR/libfaac-$TAG.a" "$OBJ"/*.o
cp "$DIR/faac/include/faac.h" "$DIR/include/faac.h"

# ---- opus: the source lists of the upstream automake fragments -------------
# CELT + SILK + SILK float + the Opus layer + its float analysis; no SIMD,
# no DNN (not enabled by default either).
list() {  # <mk file> <VAR>: the files one "VAR = \" block names
    awk -v v="$2" '
        $1 == v && $2 == "=" { on = 1; next }
        on { gsub(/\\/, ""); if ($1 == "") { on = 0; next } print $1 }
    ' "$DIR/opus/$1"
}
SRCS="$(list celt_sources.mk CELT_SOURCES) $(list silk_sources.mk SILK_SOURCES) \
      $(list silk_sources.mk SILK_SOURCES_FLOAT) $(list opus_sources.mk OPUS_SOURCES) \
      $(list opus_sources.mk OPUS_SOURCES_FLOAT)"
OBJ="$DIR/obj-opus-$TAG"; mkdir -p "$OBJ"; rm -f "$OBJ"/*.o
for s in $SRCS; do
    o="$OBJ/$(echo "$s" | tr '/' '_' | sed 's/\.c$/.o/')"
    "$CC" -O2 -fPIC -DOPUS_BUILD -DUSE_ALLOCA -DHAVE_LRINT -DHAVE_LRINTF \
        -I"$DIR/opus/include" -I"$DIR/opus/celt" -I"$DIR/opus/silk" -I"$DIR/opus/silk/float" -I"$DIR/opus" \
        -c "$DIR/opus/$s" -o "$o"
done
rm -f "$DIR/libopus-$TAG.a"
"$AR" rcs "$DIR/libopus-$TAG.a" "$OBJ"/*.o
cp "$DIR"/opus/include/*.h "$DIR/include/opus/"

touch "$STAMP"
echo "codecs built: $DIR/libfaac-$TAG.a $DIR/libopus-$TAG.a"
