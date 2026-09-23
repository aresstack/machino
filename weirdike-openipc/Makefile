# weirdiked / weirdikectl -- IKEv2 client for OpenIPC on Ingenic T40.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Cross build, same shape as machino's:
#
#   make MBEDTLS=/path/to/mbedtls-3.6.7 CROSS_COMPILE=.../mipsel-linux-
#
# Host build of the testable parts only (no TUN, no sockets):
#
#   make test

CROSS_COMPILE ?=
CC      := $(CROSS_COMPILE)gcc
STRIP   := $(CROSS_COMPILE)strip

VENDOR  := vendor/weirdike
SRC     := src
OUT     := build

MBEDTLS ?=
ifeq ($(MBEDTLS),)
$(warning MBEDTLS is unset -- set it to an mbedTLS 3.6.x install prefix)
endif

CFLAGS  += -std=c99 -Os -Wall -Wextra -Wno-unused-parameter \
           -ffunction-sections -fdata-sections \
           -D_GNU_SOURCE \
           -I$(VENDOR) -I$(SRC)
ifneq ($(MBEDTLS),)
CFLAGS  += -I$(MBEDTLS)/include
LDFLAGS += -L$(MBEDTLS)/library
endif
LDFLAGS += -Wl,--gc-sections
LIBS    += -lmbedcrypto -lmbedx509

# Hardening: the whole point of this binary is to be exposed to a hostile
# network, so it gets the flags the platform supports.
CFLAGS  += -fstack-protector-strong -D_FORTIFY_SOURCE=2
LDFLAGS += -Wl,-z,relro -Wl,-z,now

CORE_SRC := $(wildcard $(VENDOR)/*.c)
DAEMON_SRC := $(SRC)/weirdiked.c $(SRC)/wd_config.c $(SRC)/wd_net.c
CTL_SRC    := $(SRC)/weirdikectl.c

.PHONY: all clean test install

all: $(OUT)/weirdiked $(OUT)/weirdikectl

$(OUT):
	mkdir -p $(OUT)

$(OUT)/weirdiked: $(CORE_SRC) $(DAEMON_SRC) | $(OUT)
	$(CC) $(CFLAGS) -o $@ $(CORE_SRC) $(DAEMON_SRC) $(LDFLAGS) $(LIBS)

$(OUT)/weirdikectl: $(CTL_SRC) | $(OUT)
	$(CC) $(CFLAGS) -o $@ $(CTL_SRC) $(LDFLAGS)

strip: all
	$(STRIP) $(OUT)/weirdiked $(OUT)/weirdikectl

# The host test covers the parts that are pure logic: config parsing and the
# input validation. It deliberately does NOT try to exercise IKE -- WeirdIKE's
# own CI does that, against strongSwan.
test: | $(OUT)
	$(CC) -std=c99 -Wall -Wextra -Wno-unused-parameter -g \
	      -I$(SRC) -I$(VENDOR) -o $(OUT)/test_config \
	      tests/test_config.c $(SRC)/wd_config.c
	$(OUT)/test_config

install: all
	install -d $(DESTDIR)/usr/sbin $(DESTDIR)/etc/weirdike $(DESTDIR)/etc/init.d
	install -m 0755 $(OUT)/weirdiked   $(DESTDIR)/usr/sbin/weirdiked
	install -m 0755 $(OUT)/weirdikectl $(DESTDIR)/usr/sbin/weirdikectl
	install -m 0755 openipc/S99weirdike $(DESTDIR)/etc/init.d/S99weirdike
	install -m 0600 openipc/weirdike.conf.example $(DESTDIR)/etc/weirdike/weirdike.conf.example
	install -d $(DESTDIR)/var/www/cgi-bin
	install -m 0755 openipc/ipsec.cgi $(DESTDIR)/var/www/cgi-bin/ipsec.cgi

clean:
	rm -rf $(OUT)
