# machino-nna: Venus/NNA-Inferenzhelfer fuer den T40 (nna1).
#
# Gebaut mit INGENICS Toolchain (mips-linux-gnu, gcc 7.2, -muclibc), NICHT mit
# der musl-Toolchain von machinod -- die libc-Grenze ist der ganze Punkt
# (docs/architecture/nna.md). Vorlage ist Ingenics eigenes Makefile_t40 aus
# magik-toolkit/Models/post/yolov5s/venus_sample_yolov5s.
#
#   make -C machino/tools/nna \
#        CROSS_COMPILE=/pfad/zu/mips-linux-gnu- \
#        INFERENCEKIT=/pfad/zu/magik-toolkit/InferenceKit/nna1/mips720-glibc229
#
# STATISCH: das OpenIPC-Rootfs ist musl, uClibc-Laufzeitbibliotheken gibt es
# dort nicht. -static bindet die uClibc der Toolchain mit ein; das Binary
# haengt damit an nichts als dem Kernel und /dev/soc-nna.
CROSS_COMPILE ?= mips-linux-gnu-
INFERENCEKIT  ?= ../../../magik-toolkit/InferenceKit/nna1/mips720-glibc229

CXX     := $(CROSS_COMPILE)g++
STRIP   := $(CROSS_COMPILE)strip
TARGET  := machino-nna

CXXFLAGS := -std=c++11 -muclibc -mfp64 -mnan=2008 -mabs=2008 -Wall -EL -O2 \
            -march=mips32r2 -flax-vector-conversions -D_GNU_SOURCE \
            -I$(INFERENCEKIT)/include
LDFLAGS  := -muclibc -static
LIBS     := -L$(INFERENCEKIT)/lib/uclibc -lvenus -lpthread -lrt -ldl -lm

$(TARGET): machino-nna.cpp
	$(CXX) $(CXXFLAGS) $< $(LDFLAGS) $(LIBS) -o $@
	$(STRIP) $@

clean:
	rm -f $(TARGET)

.PHONY: clean
