# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2024-2025 Oracle.  All Rights Reserved.
#

TOPDIR = ..
builddefs=$(TOPDIR)/include/builddefs
include $(builddefs)

# Python implementation
XFS_HEALER_PROG = xfs_healer
INSTALL_HEALER = install-healer

# Rust implementation
ifeq ($(HAVE_HEALER_CRATES),yes)

HFILES = \
	bindgen_xfs_fs.h

RUSTFILES = \
	src/fsgeom.rs \
	src/lib.rs \
	src/main.rs \
	src/repair.rs \
	src/util.rs \
	src/weakhandle.rs \
	src/xfs_fs.rs \
	src/xfsprogs.rs \
	src/xfs_types.rs \
	src/healthmon/cstruct.rs \
	src/healthmon/event.rs \
	src/healthmon/fs.rs \
	src/healthmon/groups.rs \
	src/healthmon/inodes.rs \
	src/healthmon/json.rs \
	src/healthmon/mod.rs

BUILT_RUSTFILES = \
	src/xfs_fs.rs \
	src/xfsprogs.rs

CARGO_MANIFEST=Cargo.toml
CARGO_CONFIG=.cargo/config.toml

XFS_HEALER_RUST += bin/xfs_healer
INSTALL_HEALER += install-rust-healer
CLEAN_HEALER += clean-rust-healer

ifeq ($(HAVE_CLIPPY),yes)
	CLIPPY=$(Q)cargo clippy --no-deps
else
	CLIPPY=@true
endif

ifeq ($(ENABLE_RELEASE),yes)
	CARGO_BUILD_FLAGS=--release
	CARGO_INSTALL_FLAGS=
else
	CARGO_BUILD_FLAGS=
	CARGO_INSTALL_FLAGS=--debug
endif

# Assume that if rustc supports LTO then cargo knows how to configure it.
# rustc and cargo support LTO as of Rust-2021.
ifeq ($(HAVE_RUSTC_LTO),yes)
	CARGO_LTO=true
else
	CARGO_LTO=false
endif

RUST_DIRT=$(CARGO_MANIFEST) $(CARGO_CONFIG) $(XFS_HEALER_RUST) $(BUILT_RUSTFILES)
endif # HAVE_HEALER_CRATES

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

LDIRT = $(XFS_HEALER_PROG) $(RUST_DIRT)

default: $(XFS_HEALER_PROG) $(XFS_HEALER_RUST) $(SYSTEMD_SERVICES) $(UDEV_RULES) $(XFS_HEALER_HELPER)

clean: $(CLEAN_HEALER)

clean-rust-healer:
	-cargo clean

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

src/xfsprogs.rs: src/xfsprogs.rs.in $(builddefs)
	@echo "    [SED]    $@"
	$(Q)$(SED) -e "s|@pkg_version@|$(PKG_VERSION)|g" \
		   < $< > $@

$(CARGO_MANIFEST): $(CARGO_MANIFEST).in $(builddefs)
	@echo "    [TOML]   $@"
	$(Q)$(SED) -e "s|@pkg_version@|$(PKG_VERSION)|g" \
		   -e "s|@cargo_lto@|$(CARGO_LTO)|g" \
		   < $< | sed -e 's|\~WIP.*|"|g' > $@

ifeq ($(USE_DEBIAN_CRATES),yes)
$(CARGO_CONFIG): $(CARGO_CONFIG).debian
	@echo "    [TOML]   $@"
	$(Q)cp $< $@
else
$(CARGO_CONFIG):
	touch $@
endif

ifeq ($(HAVE_RUSTFMT),yes)
rustfmt: $(RUSTFILES)
	rustfmt $^
endif

docs:
	@echo "    [CARGO]  doc $@"
	$(Q)cargo doc --no-deps

src/xfs_fs.rs: bindgen_xfs_fs.h ../libxfs/xfs_fs.h rbindgen
	@echo "    [RBIND]  $@"
	$(Q)./rbindgen $< $@ '*xfs_fs.h*'

# cargo install only knows how to build a binary and install it to $root/bin,
# so we install it to ./rust/bin/ and let the install-rust target move it to
# $prefix/usr/libexec/xfsprogs like we want.
$(XFS_HEALER_RUST): $(RUSTFILES) $(CARGO_MANIFEST) $(CARGO_CONFIG)
	@echo "    [CARGO]  build $@ $(CARGO_BUILD_FLAGS)"
	$(CLIPPY)
	$(Q)cargo build $(CARGOFLAGS) $(CARGO_BUILD_FLAGS)
	$(Q)cargo install --path . --root . $(CARGOFLAGS) $(CARGO_INSTALL_FLAGS) &>/dev/null

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

# Leave the python version in the installed system for now
install-rust-healer: default install-healer
	$(INSTALL) -m 755 -d $(PKG_LIBEXEC_DIR)
	$(INSTALL) -m 755 $(XFS_HEALER_PROG) $(PKG_LIBEXEC_DIR)/$(XFS_HEALER_PROG).py
	$(INSTALL) -m 755 $(XFS_HEALER_RUST) $(PKG_LIBEXEC_DIR)

install-dev:

-include .dep
