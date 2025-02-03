# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2024-2025 Oracle.  All Rights Reserved.
#

TOPDIR = ..
builddefs=$(TOPDIR)/include/builddefs
include $(builddefs)

XFS_HEALER_PROG = xfs_healer.py
INSTALL_HEALER = install-healer

ifeq ($(HAVE_SYSTEMD),yes)
INSTALL_HEALER += install-systemd
SYSTEMD_SERVICES=\
	system-xfs_healer.slice \
	xfs_healer@.service
OPTIONAL_TARGETS += $(SYSTEMD_SERVICES)

ifeq ($(HAVE_UDEV),yes)
	UDEV_RULES = xfs_healer.rules
	XFS_HEALER_HELPER = xfs_healer_start
	INSTALL_HEALER += install-udev
	OPTIONAL_TARGETS += $(XFS_HEALER_HELPER)
endif
endif

LDIRT = $(XFS_HEALER_PROG)

default: $(XFS_HEALER_PROG) $(SYSTEMD_SERVICES) $(UDEV_RULES) $(XFS_HEALER_HELPER)

$(XFS_HEALER_PROG): $(XFS_HEALER_PROG).in $(builddefs) $(TOPDIR)/libfrog/gettext.py
	@echo "    [SED]    $@"
	$(Q)$(SED) -e "s|@pkg_version@|$(PKG_VERSION)|g" \
		   -e '/@INIT_GETTEXT@/r $(TOPDIR)/libfrog/gettext.py' \
		   -e '/@INIT_GETTEXT@/d' \
		   -e "s|@pkg_data_dir@|$(PKG_DATA_DIR)|g" \
		   < $< > $@
	$(Q)chmod a+x $@

%.service: %.service.in $(builddefs)
	@echo "    [SED]    $@"
	$(Q)$(SED) -e "s|@pkg_libexec_dir@|$(PKG_LIBEXEC_DIR)|g" \
		   < $< > $@

include $(BUILDRULES)

install: $(INSTALL_HEALER)

install-healer: default
	$(INSTALL) -m 755 -d $(PKG_LIBEXEC_DIR)
	$(INSTALL) -m 755 $(XFS_HEALER_PROG) $(PKG_LIBEXEC_DIR)/xfs_healer

install-systemd: default
	$(INSTALL) -m 755 -d $(SYSTEMD_SYSTEM_UNIT_DIR)
	$(INSTALL) -m 644 $(SYSTEMD_SERVICES) $(SYSTEMD_SYSTEM_UNIT_DIR)

install-udev: default
	$(INSTALL) -m 755 -d $(UDEV_DIR)
	$(INSTALL) -m 755 $(XFS_HEALER_HELPER) $(UDEV_DIR)
	$(INSTALL) -m 755 -d $(UDEV_RULE_DIR)
	for i in $(UDEV_RULES); do \
		$(INSTALL) -m 644 $$i $(UDEV_RULE_DIR)/64-$$i; \
	done

install-dev:

-include .dep
