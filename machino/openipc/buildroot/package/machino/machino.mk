################################################################################
#
# machino
#
################################################################################

# C++17 media runtime for Ingenic T40 IP cameras. Built from the machino/
# subdirectory of the source tree with the Buildroot cross toolchain. The
# Ingenic IMP headers ship in the repo (include/ submodule); the vendor
# archives (libimp/libalog/libsysutils) must come from your SDK - point
# MACHINO_IMP_LIB at where they are staged.
#
# git (not a github tarball): the IMP headers live in a submodule, which
# tarballs do not contain. Pin MACHINO_VERSION to a tag or commit on upgrade;
# the on-camera config is preserved by the installer, never by re-flashing.
MACHINO_VERSION = main
MACHINO_SITE = https://github.com/aresstack/machino.git
MACHINO_SITE_METHOD = git
MACHINO_GIT_SUBMODULES = YES
MACHINO_LICENSE = MIT
MACHINO_LICENSE_FILES = LICENSE

# The vendor IMP libraries. In thingino/OpenIPC these come from the SDK/vendor
# package; declare that here so it is built first, and point MACHINO_IMP_LIB at
# its staged archives. Name is integration-specific - adjust to your tree.
# MACHINO_DEPENDENCIES = ingenic-lib

MACHINO_SOC ?= T40
MACHINO_SDK ?= 1.3.1
MACHINO_IMP_INC ?= $(@D)/include/$(MACHINO_SOC)/$(MACHINO_SDK)/en
MACHINO_IMP_LIB ?= $(STAGING_DIR)/usr/lib
MACHINO_IMPLIBS ?= -Wl,--start-group -l:libimp.a -l:libalog.a -l:libsysutils.a -Wl,--end-group

define MACHINO_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)/machino \
		CROSS_COMPILE="$(TARGET_CROSS)" \
		PLATFORM="$(MACHINO_SOC)" \
		IMP_INC="$(MACHINO_IMP_INC)" \
		IMP_LIB="$(MACHINO_IMP_LIB)" \
		IMPLIBS="$(MACHINO_IMPLIBS)" \
		WERROR=1
endef

define MACHINO_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/machino/machinod                     $(TARGET_DIR)/usr/bin/machino
	$(INSTALL) -D -m 0755 $(@D)/machino/openipc/sbin/streamerctl     $(TARGET_DIR)/usr/sbin/streamerctl
	$(INSTALL) -D -m 0755 $(@D)/machino/openipc/sbin/machino-manager $(TARGET_DIR)/usr/sbin/machino-manager
	$(INSTALL) -D -m 0755 $(@D)/machino/openipc/init/machino         $(TARGET_DIR)/etc/init.d/machino
	$(INSTALL) -D -m 0755 $(@D)/machino/openipc/init/S95streamer     $(TARGET_DIR)/etc/init.d/S95streamer
	$(INSTALL) -D -m 0644 $(@D)/machino/machino.conf.example         $(TARGET_DIR)/etc/machino/machino.conf
endef

$(eval $(generic-package))
