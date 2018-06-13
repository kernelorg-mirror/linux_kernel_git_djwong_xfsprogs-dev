#
# Copyright (c) 2000-2004 Silicon Graphics, Inc.  All Rights Reserved.
#

TOPDIR = ..
include $(TOPDIR)/include/builddefs

LTCOMMAND = mkfs.xfs

HFILES =
CFILES = proto.c xfs_mkfs.c config.c defaults.c

CFGFILE_CFLAGS = -DMKFS_XFS_CONF_DIR=\"$(PKG_MKFS_CFG_DIR)\" \
		 -DMKFS_XFS_DEFAULT_CONFIG=\"$(PKG_MKFS_DEFAULT_CFGFILE)\"
LCFLAGS += $(CFGFILE_CFLAGS)
LLDLIBS += $(LIBXFS) $(LIBXCMD) $(LIBFROG) $(LIBRT) $(LIBPTHREAD) $(LIBBLKID) \
	$(LIBUUID)
LTDEPENDENCIES += $(LIBXFS) $(LIBXCMD) $(LIBFROG)
LLDFLAGS = -static-libtool-libs

default: depend $(LTCOMMAND)

include $(BUILDRULES)

install: default
	$(INSTALL) -m 755 -d $(PKG_ROOT_SBIN_DIR)
	$(LTINSTALL) -m 755 $(LTCOMMAND) $(PKG_ROOT_SBIN_DIR)
	$(INSTALL) -m 755 -d $(PKG_CFG_DIR)
	$(INSTALL) -m 755 -d $(PKG_MKFS_CFG_DIR)
install-dev:

-include .dep
