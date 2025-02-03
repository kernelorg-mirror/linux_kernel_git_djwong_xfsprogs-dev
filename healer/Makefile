# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2024-2025 Oracle.  All Rights Reserved.
#

TOPDIR = ..
builddefs=$(TOPDIR)/include/builddefs
include $(builddefs)

XFS_HEALER_PROG = xfs_healer
INSTALL_HEALER = install-healer

LDIRT = $(XFS_HEALER_PROG)

default: $(XFS_HEALER_PROG)

xfs_healer: xfs_healer.in $(builddefs)
	@echo "    [SED]    $@"
	$(Q)$(SED) -e "s|@pkg_version@|$(PKG_VERSION)|g" \
		   < $< > $@
	$(Q)chmod a+x $@

include $(BUILDRULES)

install: $(INSTALL_HEALER)

install-healer: default
	$(INSTALL) -m 755 -d $(PKG_LIBEXEC_DIR)
	$(INSTALL) -m 755 $(XFS_HEALER_PROG) $(PKG_LIBEXEC_DIR)

install-dev:

-include .dep
