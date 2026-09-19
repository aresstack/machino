# Machino - C++ runtime (ports & adapters). Cross-built with the thingino
# toolchain exactly like the C prototype: build.sh supplies CROSS_COMPILE,
# PLATFORM, IMP_INC, IMP_LIB and IMPLIBS (vendor libs + musl shim, start-group).
#
#   make -C machino CROSS_COMPILE=.../mipsel-linux- PLATFORM=T40 \
#        IMP_INC=../include/T40/1.3.1/en IMP_LIB=../3rdparty/install/lib \
#        IMPLIBS="-Wl,--start-group -l:libimp.a -l:libalog.a -l:libsysutils.a -Wl,--end-group -l:libmuslshim.a"
#   make -C machino test        # host unit tests (no SDK, no hardware)
#
# No exceptions, no RTTI, no third-party frameworks. libstdc++ is linked
# statically (single self-contained drop-in on a 5 MB rootfs). Non-PIE: the
# vendor archives are non-PIC. Warnings are errors for our own sources; the
# vendor headers are included as system headers.

PLATFORM      ?= T40
CROSS_COMPILE ?=
INC_ROOT      ?= ../include
IMP_INC       ?= $(INC_ROOT)/$(PLATFORM)/1.3.1/en
IMP_LIB       ?=
IMPLIBS       ?= -Wl,--start-group -l:libimp.a -l:libalog.a -l:libsysutils.a -Wl,--end-group
VERSION       ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
WERROR        ?= 1

CXX     := $(CROSS_COMPILE)g++
BIN     := machinod

CXXFLAGS ?= -std=c++17 -Os -fno-pie -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
            -Wall -Wextra -Wno-unused-parameter $(if $(filter 1,$(WERROR)),-Werror) -D_GNU_SOURCE
CXXFLAGS += -DMACHINO_VERSION='"$(VERSION)"' -DPLATFORM_$(PLATFORM) -Isrc -isystem $(IMP_INC) -isystem $(IMP_INC)/imp
LDFLAGS  ?= -no-pie -Wl,--gc-sections
LDFLAGS  += -static-libstdc++ -static-libgcc
LIBS     ?= -lpthread -lrt -lm

CORE_SRC := src/core/config.cpp src/core/config_store.cpp src/core/events.cpp src/core/json.cpp src/core/stream_hub.cpp \
            src/core/hw/board_profile_parser.cpp src/core/hw/resolve.cpp \
            src/core/lifecycle/pipeline_manager.cpp src/core/power/performance_service.cpp src/core/media/tuning_service.cpp \
            src/profiles/builtin_profiles.cpp
SRC := src/core/log.cpp $(CORE_SRC) \
       src/app/api/api_service.cpp src/app/compat/majestic_webui.cpp src/app/http/http_parse.cpp src/app/http/http_server.cpp \
       src/app/linux_grace_timer.cpp src/app/linux_system_stats.cpp \
       src/app/rtsp/h264_nal.cpp src/app/rtsp/rtsp_server.cpp src/app/main.cpp \
       src/adapters/ingenic/sensor_params.cpp src/adapters/ingenic/imp_sessions.cpp \
       src/adapters/ingenic/ingenic_power_control.cpp \
       src/adapters/ingenic/ingenic_image_control.cpp \
       src/adapters/ingenic/ingenic_platform.cpp src/adapters/ingenic/ingenic_framesource.cpp \
       src/adapters/ingenic/ingenic_encoder.cpp src/adapters/ingenic/ingenic_jpeg.cpp
OBJ := $(SRC:.cpp=.o)

# host unit tests: core + profiles + lifecycle + performance + API (fakes) + the SDK-free adapter conversion
HOST_CXX      ?= g++
HOST_CXXFLAGS ?= -std=c++17 -O1 -fno-exceptions -fno-rtti -Wall -Wextra -Wno-unused-parameter -Werror -pthread -Isrc -Itests -DMACHINO_VERSION='"test"'
TEST_SRC := tests/test_main.cpp tests/test_lifecycle.cpp tests/test_power.cpp tests/test_json.cpp tests/test_events.cpp \
            tests/test_http_parse.cpp tests/test_api.cpp tests/test_tuning.cpp tests/test_multistream.cpp tests/log_stub.cpp \
            src/core/config.cpp src/core/config_store.cpp src/core/events.cpp src/core/json.cpp src/core/stream_hub.cpp \
            src/core/hw/board_profile_parser.cpp src/core/hw/resolve.cpp src/core/lifecycle/pipeline_manager.cpp \
            src/core/power/performance_service.cpp src/core/media/tuning_service.cpp src/app/api/api_service.cpp src/app/compat/majestic_webui.cpp src/app/http/http_parse.cpp \
            src/profiles/builtin_profiles.cpp src/adapters/ingenic/sensor_params.cpp

.PHONY: all clean strip flags test
all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(OBJ) $(LDFLAGS) $(if $(IMP_LIB),-L$(IMP_LIB)) $(IMPLIBS) $(LIBS) -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

strip: $(BIN)
	$(CROSS_COMPILE)strip $(BIN)

test:
	$(HOST_CXX) $(HOST_CXXFLAGS) $(TEST_SRC) -o tests/machino-tests
	./tests/machino-tests
	sh tests/test_streamerctl.sh
	sh tests/test_openipc_install.sh

flags:
	@echo "CXXFLAGS=$(CXXFLAGS)"
	@echo "LDFLAGS=$(LDFLAGS) $(IMPLIBS) $(LIBS)"

clean:
	rm -f $(OBJ) $(BIN) tests/machino-tests tests/machino-tests.exe tests/tmp_machino.conf tests/tmp_machino.conf.tmp
	rm -rf tests/tmp-streamerctl tests/tmp-openipc-install
