# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2024-2025 Oracle.  All Rights Reserved.
#

TOPDIR = ..
builddefs=$(TOPDIR)/include/builddefs
include $(builddefs)

XFS_HEALER_PROG = xfs_healer
INSTALL_HEALER = install-healer

RUSTFILES = \
	src/main.rs \
	src/lib.rs \
	src/xfs_fs.rs \
	src/healthmon/mod.rs

ifeq ($(ENABLE_RELEASE),yes)
	CARGO_BUILD_FLAGS=--release
else
	CARGO_BUILD_FLAGS=
endif

ifeq ($(HAVE_RUST),yes)
.PHONY: healer-rust
CARGO_CONFIG=Cargo.toml
XFS_HEALER_PROG += healer-rust
INSTALL_HEALER += install-rust-healer
endif

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

LDIRT = $(XFS_HEALER_PROG) $(CARGO_CONFIG) src/xfs_fs.rs

default: $(XFS_HEALER_PROG) $(SYSTEMD_SERVICES) $(UDEV_RULES) $(XFS_HEALER_HELPER)

xfs_healer: xfs_healer.in $(builddefs)
	@echo "    [SED]    $@"
	$(Q)$(SED) -e "s|@pkg_data_dir@|$(PKG_DATA_DIR)|g" \
		   -e "s|@pkg_version@|$(PKG_VERSION)|g" \
		   < $< > $@
	$(Q)chmod a+x $@

%.service: %.service.in $(builddefs)
	@echo "    [SED]    $@"
	$(Q)$(SED) -e "s|@pkg_libexec_dir@|$(PKG_LIBEXEC_DIR)|g" \
		   < $< > $@

$(CARGO_CONFIG): $(CARGO_CONFIG).in
	@echo "    [TOML]   $@"
	$(Q)$(SED) -e "s|@pkg_version@|$(PKG_VERSION)|g" < $< | sed -e 's|\~WIP.*|"|g' > $@

docs:
	@echo "    [CARGO]  $@"
	$(Q)cargo doc

src/xfs_fs.rs: bindgen_xfs_fs.h ../libxfs/xfs_fs.h rbindgen
	@echo "    [RBG]    $@"
	$(Q)./rbindgen $< $@ '*xfs_fs.h*'

healer-rust: $(RUSTFILES) $(CARGO_CONFIG)
	@echo "    [CARGO]  $@ $(CARGO_BUILD_FLAGS)"
	$(Q)cargo build $(CARGOFLAGS) $(CARGO_BUILD_FLAGS)

include $(BUILDRULES)

install: $(INSTALL_HEALER)

install-healer: default
	$(INSTALL) -m 755 -d $(PKG_LIBEXEC_DIR)
	$(INSTALL) -m 755 $(XFS_HEALER_PROG) $(PKG_LIBEXEC_DIR)

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

install-rust-healer: default
	cargo install --path . --root $(exec_prefix)

install-dev:

-include .dep
