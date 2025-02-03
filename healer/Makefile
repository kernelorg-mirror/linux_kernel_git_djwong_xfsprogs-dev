# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2024-2026 Oracle.  All Rights Reserved.
#

TOPDIR = ..
builddefs=$(TOPDIR)/include/builddefs
include $(builddefs)

INSTALL_HEALER = install-healer

LTCOMMAND = xfs_healer

CFILES = \
fsrepair.c \
weakhandle.c \
xfs_healer.c

HFILES = \
xfs_healer.h

LLDLIBS += $(LIBHANDLE) $(LIBFROG) $(LIBURCU) $(LIBPTHREAD)
LTDEPENDENCIES += $(LIBHANDLE) $(LIBFROG)
LLDFLAGS = -static

ifeq ($(HAVE_SYSTEMD),yes)
INSTALL_HEALER += install-systemd
SYSTEMD_SERVICES=\
	system-xfs_healer.slice \
	xfs_healer@.service
OPTIONAL_TARGETS += $(SYSTEMD_SERVICES)
endif

default: depend $(LTCOMMAND) $(SYSTEMD_SERVICES)

%.service: %.service.in $(builddefs)
	@echo "    [SED]    $@"
	$(Q)$(SED) -e "s|@pkg_libexec_dir@|$(PKG_LIBEXEC_DIR)|g" \
		   < $< > $@

include $(BUILDRULES)

install: $(INSTALL_HEALER)

install-healer: default
	$(INSTALL) -m 755 -d $(PKG_LIBEXEC_DIR)
	$(INSTALL) -m 755 $(LTCOMMAND) $(PKG_LIBEXEC_DIR)

install-systemd: default
	$(INSTALL) -m 755 -d $(SYSTEMD_SYSTEM_UNIT_DIR)
	$(INSTALL) -m 644 $(SYSTEMD_SERVICES) $(SYSTEMD_SYSTEM_UNIT_DIR)

install-dev:

-include .dep
