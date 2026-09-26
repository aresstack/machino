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

# mbedTLS (pinned, built by tools/fetch-mbedtls.sh <dir> <cc> <ar>): the one
# third-party dependency, used only for the WebRTC DTLS handshake. MBEDTLS
# points at the unpacked mbedtls-<ver> root; the archive name carries the
# compiler it was built with, so host and cross builds coexist.
MBEDTLS ?=
ifneq ($(MBEDTLS),)
CXXFLAGS += -DMBEDTLS_SSL_DTLS_SRTP -isystem $(MBEDTLS)/include
MBEDTLS_LIB := $(MBEDTLS)/libmbedall-$(notdir $(CROSS_COMPILE)gcc).a
LIBS += $(MBEDTLS_LIB)
check-mbedtls: ;
else
check-mbedtls:
	$(error MBEDTLS is not set - run machino/tools/fetch-mbedtls.sh <dir> <cc> <ar> and pass MBEDTLS=<dir>/mbedtls-<ver>)
endif
.PHONY: check-mbedtls
.DEFAULT_GOAL := all   # check-mbedtls above must not become the default goal

CORE_SRC := src/core/random.cpp src/core/config.cpp src/core/config_store.cpp src/core/events.cpp src/core/json.cpp src/core/stream_hub.cpp src/core/runtime_stats.cpp src/core/log_file.cpp \
            src/core/hw/board_profile_parser.cpp src/core/hw/resolve.cpp \
            src/core/lifecycle/pipeline_manager.cpp src/core/power/performance_service.cpp src/core/media/tuning_service.cpp \
            src/core/detection/detection_service.cpp src/core/detection/nna_detector.cpp src/core/detection/nna_manifest.cpp src/core/detection/detector_availability.cpp src/core/usb/usb_host_service.cpp src/core/net/connectivity.cpp src/core/hw/pin_resolver.cpp src/core/state_store.cpp src/core/net/network_txn.cpp src/app/api/net_views.cpp src/app/api/net_api.cpp src/core/devices/device_package.cpp src/core/devices/device_manifest.cpp src/core/devices/aic8800_package.cpp src/core/net/wpa_parse.cpp src/core/net/proc_net.cpp src/core/net/hostapd_conf.cpp src/core/cellular/modem_ports.cpp src/core/cellular/at_framing.cpp src/core/cellular/at_parse.cpp src/core/cellular/sim_manager.cpp src/core/cellular/cellular_config.cpp src/core/cellular/cellular_service.cpp src/core/cellular/ecm_link.cpp src/core/cellular/ppp_link.cpp src/core/net/cellular_uplink.cpp src/core/net/route_plan.cpp src/core/net/route_manager.cpp src/core/net/ipsec_config.cpp src/core/net/ipsec_service.cpp src/profiles/usb_profiles.cpp \
            src/profiles/builtin_profiles.cpp
SRC := src/core/log.cpp $(CORE_SRC) \
       src/app/log_reader.cpp src/app/osd/osd_service.cpp src/app/onvif/soap.cpp src/app/onvif/discovery.cpp src/app/onvif/discovery_server.cpp src/app/onvif/onvif_service.cpp src/app/api/api_service.cpp src/app/api/ipsec_api.cpp src/app/compat/majestic_webui.cpp src/app/compat/majestic_migrate.cpp src/app/http/http_parse.cpp src/app/http/session.cpp src/app/http/setup.cpp src/app/http/fmp4.cpp src/app/http/websocket.cpp src/app/http/http_server.cpp \
       src/app/linux_grace_timer.cpp src/app/linux_system_stats.cpp \
       src/app/watchdog.cpp src/app/linux_watchdog.cpp \
       src/app/rtsp/h264_nal.cpp src/app/rtsp/rtsp_auth.cpp src/app/rtsp/rtsp_server.cpp src/app/main.cpp \
       src/app/webrtc/sdp.cpp src/app/webrtc/stun.cpp src/app/webrtc/rtp.cpp src/app/webrtc/aes.cpp src/app/webrtc/srtp.cpp src/app/webrtc/dtls.cpp src/app/webrtc/peer.cpp \
       src/adapters/linux/sysfs_gpio.cpp src/adapters/linux/linux_usb_host.cpp src/adapters/linux/wpa_ctrl.cpp src/adapters/linux/linux_netif.cpp src/adapters/linux/linux_ethernet_uplink.cpp src/adapters/linux/wpa_supplicant_wifi.cpp src/adapters/linux/hostapd_ap.cpp src/adapters/linux/wifi_station_uplink.cpp src/adapters/linux/linux_route_backend.cpp src/adapters/linux/linux_ppp_backend.cpp src/adapters/linux/linux_ipsec_backend.cpp \
       src/adapters/linux/linux_serial_scan.cpp src/adapters/linux/at_transport.cpp src/adapters/linux/linux_ecm_backend.cpp \
       src/adapters/ingenic/sensor_params.cpp src/adapters/ingenic/imp_sessions.cpp \
       src/adapters/ingenic/ingenic_power_control.cpp \
       src/adapters/ingenic/ingenic_image_control.cpp \
       src/adapters/ingenic/ingenic_platform.cpp src/adapters/ingenic/ingenic_framesource.cpp \
       src/adapters/ingenic/ingenic_encoder.cpp src/adapters/ingenic/ingenic_jpeg.cpp \
       src/adapters/ingenic/detection/ivs_motion.cpp src/adapters/ingenic/detection/nna_source.cpp \
       src/adapters/linux/linux_nna_process.cpp
OBJ := $(SRC:.cpp=.o)

# host unit tests: core + profiles + lifecycle + performance + API (fakes) + the SDK-free adapter conversion
HOST_CXX      ?= g++
HOST_CXXFLAGS ?= -std=c++17 -O1 -fno-exceptions -fno-rtti -Wall -Wextra -Wno-unused-parameter -Werror -pthread -Isrc -Itests -DMACHINO_VERSION='"test"'
HOST_CXXFLAGS += -DMBEDTLS_SSL_DTLS_SRTP -isystem $(MBEDTLS)/include
HOST_MBEDTLS_LIB := $(MBEDTLS)/libmbedall-$(notdir $(HOST_CXX:g++=gcc)).a
HOST_LIBS ?= $(HOST_MBEDTLS_LIB)
ifeq ($(OS),Windows_NT)
HOST_LIBS += -lbcrypt -lws2_32
endif
TEST_SRC := tests/test_main.cpp tests/test_lifecycle.cpp tests/test_power.cpp tests/test_json.cpp tests/test_events.cpp \
            tests/test_http_parse.cpp tests/test_session.cpp tests/test_fmp4.cpp tests/test_sps.cpp tests/test_webrtc.cpp tests/test_websocket.cpp tests/test_api.cpp tests/test_tuning.cpp tests/test_multistream.cpp tests/test_detection.cpp tests/test_compat.cpp tests/test_schema_contract.cpp tests/test_logging.cpp tests/test_osd.cpp tests/test_setup.cpp tests/test_onvif.cpp tests/test_discovery.cpp tests/test_onvif_digest.cpp tests/test_random.cpp tests/test_watchdog.cpp tests/test_usb.cpp tests/test_connectivity.cpp tests/test_pin_resolver.cpp tests/test_network_txn.cpp tests/test_state_store.cpp tests/test_net_views.cpp tests/test_net_api.cpp tests/test_wpa_parse.cpp tests/test_wwwpages.cpp tests/test_proc_net.cpp tests/test_hostapd_conf.cpp tests/test_adapter_wiring.cpp tests/test_cellular_ports.cpp tests/test_cellular.cpp tests/test_ecm_link.cpp tests/test_cellular_uplink.cpp tests/test_ppp_link.cpp tests/test_route_plan.cpp tests/test_devices.cpp tests/test_nna_detector.cpp tests/test_ipsec.cpp tests/log_stub.cpp \
            src/core/random.cpp src/core/config.cpp src/core/config_store.cpp src/core/events.cpp src/core/json.cpp src/core/stream_hub.cpp src/core/runtime_stats.cpp src/core/log_file.cpp \
            src/core/hw/board_profile_parser.cpp src/core/hw/resolve.cpp src/core/lifecycle/pipeline_manager.cpp \
            src/core/power/performance_service.cpp src/core/media/tuning_service.cpp src/core/detection/detection_service.cpp src/core/detection/nna_detector.cpp src/core/detection/nna_manifest.cpp src/core/detection/detector_availability.cpp src/core/usb/usb_host_service.cpp src/core/net/connectivity.cpp src/core/hw/pin_resolver.cpp src/core/state_store.cpp src/core/net/network_txn.cpp src/app/api/net_views.cpp src/app/api/net_api.cpp src/core/devices/device_package.cpp src/core/devices/device_manifest.cpp src/core/devices/aic8800_package.cpp src/core/net/wpa_parse.cpp src/core/net/proc_net.cpp src/core/net/hostapd_conf.cpp src/core/cellular/modem_ports.cpp src/core/cellular/at_framing.cpp src/core/cellular/at_parse.cpp src/core/cellular/sim_manager.cpp src/core/cellular/cellular_config.cpp src/core/cellular/cellular_service.cpp src/core/cellular/ecm_link.cpp src/core/cellular/ppp_link.cpp src/core/net/cellular_uplink.cpp src/core/net/route_plan.cpp src/core/net/route_manager.cpp src/core/net/ipsec_config.cpp src/core/net/ipsec_service.cpp src/profiles/usb_profiles.cpp \
            src/app/osd/osd_service.cpp src/app/watchdog.cpp \
            src/app/api/api_service.cpp src/app/api/ipsec_api.cpp src/app/compat/majestic_webui.cpp src/app/compat/majestic_migrate.cpp src/app/http/http_parse.cpp src/app/http/session.cpp src/app/onvif/soap.cpp src/app/onvif/discovery.cpp src/app/onvif/onvif_service.cpp src/app/http/setup.cpp \
            src/app/http/fmp4.cpp src/app/http/websocket.cpp src/app/rtsp/h264_nal.cpp src/app/rtsp/rtsp_auth.cpp tests/test_rtsp_auth.cpp tests/test_rtsp_claim.cpp \
            src/app/webrtc/sdp.cpp src/app/webrtc/stun.cpp src/app/webrtc/rtp.cpp src/app/webrtc/aes.cpp src/app/webrtc/srtp.cpp src/app/webrtc/dtls.cpp tests/test_dtls.cpp \
            src/profiles/builtin_profiles.cpp src/adapters/ingenic/sensor_params.cpp

.PHONY: all clean strip flags test
all: $(BIN)

# order-only: the PHONY guard must not mark the binary stale (a strip-only
# invocation would otherwise re-link with default vendor-lib paths)
$(BIN): $(OBJ) | check-mbedtls
	$(CXX) $(OBJ) $(LDFLAGS) $(if $(IMP_LIB),-L$(IMP_LIB)) $(IMPLIBS) $(LIBS) -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

strip: $(BIN)
	$(CROSS_COMPILE)strip $(BIN)

test: check-mbedtls
	$(HOST_CXX) $(HOST_CXXFLAGS) $(TEST_SRC) $(HOST_LIBS) -o tests/machino-tests
	./tests/machino-tests
	sh tests/test_streamerctl.sh
	sh tests/test_openipc_install.sh

flags:
	@echo "CXXFLAGS=$(CXXFLAGS)"
	@echo "LDFLAGS=$(LDFLAGS) $(IMPLIBS) $(LIBS)"

clean:
	rm -f $(OBJ) $(BIN) tests/machino-tests tests/machino-tests.exe tests/tmp_machino.conf tests/tmp_machino.conf.tmp
	rm -rf tests/tmp-streamerctl tests/tmp-openipc-install
