# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2024-2026 Oracle.  All Rights Reserved.
#

TOPDIR = ..
builddefs=$(TOPDIR)/include/builddefs
include $(builddefs)

INSTALL_HEALER = install-healer

LTCOMMAND = xfs_healer
BUILD_TARGETS = $(LTCOMMAND)

CFILES = \
fsrepair.c \
weakhandle.c \
xfs_healer.c

HFILES = \
xfs_healer.h

CFLAGS+=-DXFS_SCRUB_SVCNAME=\"$(XFS_SCRUB_SVCNAME)\"

ifeq ($(HAVE_LISTMOUNT),yes)
CFLAGS += -DHAVE_LISTMOUNT
endif

LLDLIBS += $(LIBHANDLE) $(LIBFROG) $(LIBURCU) $(LIBPTHREAD)
LTDEPENDENCIES += $(LIBHANDLE) $(LIBFROG)
LLDFLAGS = -static

XFS_HEALER_SVCNAME=xfs_healer@.service
CFLAGS += -DXFS_HEALER_SVCNAME=\"$(XFS_HEALER_SVCNAME)\"

ifeq ($(HAVE_SYSTEMD),yes)
INSTALL_HEALER += install-systemd
SYSTEMD_SERVICES=\
	system-xfs_healer.slice \
	$(XFS_HEALER_SVCNAME)
OPTIONAL_TARGETS += $(SYSTEMD_SERVICES)
endif # HAVE_SYSTEMD

ifeq ($(HAVE_HEALER_START_DEPS),yes)
BUILD_TARGETS += xfs_healer_start
LDIRT += xfs_healer_start xfs_healer_start.o
SYSTEMD_SERVICES += xfs_healer_start.service
endif # xfs_healer_start deps

LDIRT += $(SYSTEMD_SERVICES)

default: depend $(BUILD_TARGETS) $(SYSTEMD_SERVICES)

ifeq ($(CHECKSRC),2)
xfs_healer_start: $(SUBDIRS) xfs_healer_start.o
.PHONY: xfs_healer_start
else
xfs_healer_start: $(SUBDIRS) xfs_healer_start.o $(LTDEPENDENCIES)
	@echo "    [LD]     $@"
	$(Q)$(LTLINK) -o $@ $(LDFLAGS) xfs_healer_start.o $(LDLIBS)
endif

%.service: %.service.in $(builddefs)
	@echo "    [SED]    $@"
	$(Q)$(SED) -e "s|@pkg_libexec_dir@|$(PKG_LIBEXEC_DIR)|g" \
		   < $< > $@

include $(BUILDRULES)

install: $(INSTALL_HEALER)

install-healer: default
	$(INSTALL) -m 755 -d $(PKG_LIBEXEC_DIR)
	$(LTINSTALL) -m 755 $(BUILD_TARGETS) $(PKG_LIBEXEC_DIR)

install-systemd: default
	$(INSTALL) -m 755 -d $(SYSTEMD_SYSTEM_UNIT_DIR)
	$(INSTALL) -m 644 $(SYSTEMD_SERVICES) $(SYSTEMD_SYSTEM_UNIT_DIR)

install-dev:

-include .dep
