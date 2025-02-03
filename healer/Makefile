# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2024-2025 Oracle.  All Rights Reserved.
#

TOPDIR = ..
builddefs=$(TOPDIR)/include/builddefs
include $(builddefs)

XFS_HEALER_PROG = xfs_healer.py
INSTALL_HEALER = install-healer

LTCOMMAND=xfs_healer

CFILES = \
eventlog.c \
xfs_healer.c

HFILES = \
xfs_healer.h

LLDLIBS += $(LIBHANDLE) $(LIBFROG) $(LIBURCU) $(LIBPTHREAD)
LTDEPENDENCIES += $(LIBHANDLE) $(LIBFROG)
LLDFLAGS = -static

LDIRT = $(XFS_HEALER_PROG)

default: depend $(LTCOMMAND) $(XFS_HEALER_PROG)

$(XFS_HEALER_PROG): $(XFS_HEALER_PROG).in $(builddefs) $(TOPDIR)/libfrog/gettext.py
	@echo "    [SED]    $@"
	$(Q)$(SED) -e "s|@pkg_version@|$(PKG_VERSION)|g" \
		   -e '/@INIT_GETTEXT@/r $(TOPDIR)/libfrog/gettext.py' \
		   -e '/@INIT_GETTEXT@/d' \
		   < $< > $@
	$(Q)chmod a+x $@

include $(BUILDRULES)

install: $(INSTALL_HEALER)

install-healer: default
	$(INSTALL) -m 755 -d $(PKG_LIBEXEC_DIR)
	$(INSTALL) -m 755 $(XFS_HEALER_PROG) $(PKG_LIBEXEC_DIR)/xfs_healer

install-dev:

-include .dep
