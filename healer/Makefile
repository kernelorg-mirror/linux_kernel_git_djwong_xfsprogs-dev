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

default: depend $(LTCOMMAND)

include $(BUILDRULES)

install: $(INSTALL_HEALER)

install-healer: default
	$(INSTALL) -m 755 -d $(PKG_LIBEXEC_DIR)
	$(INSTALL) -m 755 $(LTCOMMAND) $(PKG_LIBEXEC_DIR)

install-dev:

-include .dep
