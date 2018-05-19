# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2017 Oracle.  All Rights Reserved.
#

TOPDIR = ..
include $(TOPDIR)/include/builddefs

LTLIBRARY = libfrog.la
LT_CURRENT = 0
LT_REVISION = 0
LT_AGE = 0

CFILES = \
avl64.c \
bitmap.c \
convert.c \
fsgeom.c \
list_sort.c \
linux.c \
paths.c \
projects.c \
ptvar.c \
radix-tree.c \
topology.c \
util.c \
workqueue.c

CXXFILES = \
crc32.cpp

HFILES = \
crc32defs.h \
crc32table.h

LSRCFILES += gen_crc32table.c

ifeq ($(USE_FMV),yes)
CXXFLAGS += -DUSE_FMV
BUILD_CXXFLAGS += -DUSE_FMV
endif

ifeq ($(USE_PCLMUL_FMV),yes)
CRC32C_ASMFILES += crc32c-asm-intel-pcl.S
CRC32C_OBJFILES += crc32c-asm-intel-pcl.o
CXXFLAGS += -DUSE_PCLMUL_FMV
BUILD_CXXFLAGS += -DUSE_PCLMUL_FMV
endif

ASMFILES += $(CRC32C_ASMFILES)

ifeq ($(HAVE_GETMNTENT),yes)
LCFLAGS += -DHAVE_GETMNTENT
endif

LDIRT = gen_crc32table crc32table.h crc32selftest

default: crc32selftest ltdepend $(LTLIBRARY)

crc32.o: $(TOPDIR)/include/builddefs

$(LTLIBRARY): $(TOPDIR)/include/builddefs

crc32table.h: gen_crc32table.c crc32defs.h
	@echo "    [CC]     gen_crc32table"
	$(Q) $(BUILD_CC) $(BUILD_CFLAGS) -o gen_crc32table $<
	@echo "    [GENERATE] $@"
	$(Q) ./gen_crc32table > crc32table.h

# The selftest binary will return an error if it fails. This is made a
# dependency of the build process so that we refuse to build the tools on broken
# systems/architectures. Hence we make sure that xfsprogs will never use a
# busted CRC calculation at build time and hence avoid putting bad CRCs down on
# disk.
crc32selftest: gen_crc32table.c crc32table.h crc32.cpp crc32defs.h $(CRC32C_ASMFILES)
	@echo "    [TEST]    CRC32"
	$(Q) $(BUILD_CXX) $(BUILD_CXXFLAGS) -D CRC32_SELFTEST=1 crc32.cpp $(CRC32C_ASMFILES) -o $@
	$(Q) ./$@

include $(BUILDRULES)

install install-dev: default

-include .ltdep
