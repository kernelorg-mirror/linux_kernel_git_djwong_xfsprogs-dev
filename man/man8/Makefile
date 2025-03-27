#
# Copyright (c) 2000-2001 Silicon Graphics, Inc.  All Rights Reserved.
#

TOPDIR = ../..
include $(TOPDIR)/include/builddefs

MAN_SECTION	= 8

MAN_PAGES = \
	fsck.xfs.8 \
	mkfs.xfs.8 \
	xfs_admin.8 \
	xfs_bmap.8 \
	xfs_copy.8 \
	xfs_db.8 \
	xfs_estimate.8 \
	xfs_freeze.8 \
	xfs_fsr.8 \
	xfs_growfs.8 \
	xfs_info.8 \
	xfs_io.8 \
	xfs_logprint.8 \
	xfs_mdrestore.8 \
	xfs_metadump.8 \
	xfs_mkfile.8 \
	xfs_ncheck.8 \
	xfs_property.8 \
	xfs_protofile.8 \
	xfs_quota.8 \
	xfs_repair.8 \
	xfs_rtcp.8 \
	xfs_spaceman.8

ifeq ($(ENABLE_HEALER),yes)
  MAN_PAGES += xfs_healer.8
endif
ifeq ($(HAVE_HEALER_START_DEPS),yes)
  MAN_PAGES += xfs_healer_start.8
endif
ifeq ($(ENABLE_SCRUB),yes)
  MAN_PAGES += xfs_scrub.8 xfs_scrub_all.8
endif

MAN_DEST	= $(PKG_MAN_DIR)/man$(MAN_SECTION)
LSRCFILES	= $(MAN_PAGES)
DIRT		= mkfs.xfs.8 xfs_scrub_all.8

default : $(MAN_PAGES)

include $(BUILDRULES)

install : default
	$(INSTALL) -m 755 -d $(MAN_DEST)
	$(INSTALL_MAN)

mkfs.xfs.8: mkfs.xfs.8.in
	@echo "    [SED]    $@"
	$(Q)$(SED) -e 's|@mkfs_cfg_dir@|$(MKFS_CFG_DIR)|g' < $^ > $@

xfs_scrub_all.8: xfs_scrub_all.8.in
	@echo "    [SED]    $@"
	$(Q)$(SED) -e 's|@stampfile@|$(XFS_SCRUB_ALL_AUTO_MEDIA_SCAN_STAMP)|g' < $^ > $@

install-dev :
