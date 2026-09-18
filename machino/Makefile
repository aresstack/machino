# Machino M2 - C++ foundation (ports & adapters). Cross-built with the thingino
# toolchain exactly like the C prototype: build.sh supplies CROSS_COMPILE,
# PLATFORM, IMP_INC, IMP_LIB and IMPLIBS (vendor libs + musl shim, start-group).
#
#   make -C machino CROSS_COMPILE=.../mipsel-linux- PLATFORM=T40 \
#        IMP_INC=../include/T40/1.3.1/en IMP_LIB=../3rdparty/install/lib \
#        IMPLIBS="-Wl,--start-group -l:libimp.a -l:libalog.a -l:libsysutils.a -Wl,--end-group -l:libmuslshim.a"
#
# No exceptions, no RTTI, no third-party frameworks. libstdc++ is linked
# statically (single self-contained drop-in on a 5 MB rootfs).

PLATFORM      ?= T40
CROSS_COMPILE ?=
INC_ROOT      ?= ../include
IMP_INC       ?= $(INC_ROOT)/$(PLATFORM)/1.3.1/en
IMP_LIB       ?=
IMPLIBS       ?= -Wl,--start-group -l:libimp.a -l:libalog.a -l:libsysutils.a -Wl,--end-group
VERSION       ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo m2-dev)

CXX     := $(CROSS_COMPILE)g++
BIN     := machinod

CXXFLAGS ?= -std=c++17 -Os -fno-pie -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
            -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE
CXXFLAGS += -DMACHINO_VERSION='"$(VERSION)"' -DPLATFORM_$(PLATFORM) -Isrc -I$(IMP_INC) -I$(IMP_INC)/imp
LDFLAGS  ?= -no-pie -Wl,--gc-sections
LDFLAGS  += -static-libstdc++ -static-libgcc
LIBS     ?= -lpthread -lrt -lm

SRC := src/core/log.cpp src/core/config.cpp src/core/pipeline.cpp src/core/stream_hub.cpp \
       src/app/rtsp/h264_nal.cpp src/app/rtsp/rtsp_server.cpp src/app/main.cpp \
       src/adapters/ingenic/ingenic_platform.cpp src/adapters/ingenic/ingenic_framesource.cpp \
       src/adapters/ingenic/ingenic_encoder.cpp
OBJ := $(SRC:.cpp=.o)

.PHONY: all clean strip
all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(OBJ) $(LDFLAGS) $(if $(IMP_LIB),-L$(IMP_LIB)) $(IMPLIBS) $(LIBS) -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

strip: $(BIN)
	$(CROSS_COMPILE)strip $(BIN)

clean:
	rm -f $(OBJ) $(BIN)
