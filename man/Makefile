# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2000-2001 Silicon Graphics, Inc.  All Rights Reserved.
#

TOPDIR = ..
include $(TOPDIR)/include/builddefs

SUBDIRS = man2 man3 man5 man8

default : $(SUBDIRS)

install : install-pkg

install-pkg : $(addsuffix -install-pkg,$(SUBDIRS))

install-dev : $(addsuffix -install-dev,$(SUBDIRS))

%-install-pkg:
	$(Q)$(MAKE) $(MAKEOPTS) -C $* install-pkg

%-install-dev:
	$(Q)$(MAKE) $(MAKEOPTS) -C $* install-dev

include $(BUILDRULES)
