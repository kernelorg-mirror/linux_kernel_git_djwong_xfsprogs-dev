// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2002,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include <ctype.h>
#include <time.h>
#include "type.h"
#include "faddr.h"
#include "fprint.h"
#include "field.h"
#include "inode.h"
#include "btblock.h"
#include "bit.h"
#include "print.h"
#include "output.h"
#include "sig.h"
#include "malloc.h"
#include "io.h"
#include "init.h"

int
fp_charns(
	void	*obj,
	int	bit,
	int	count,
	char	*fmtstr,
	int	size,
	int	arg,
	int	base,
	int	array)
{
	int	i;
	char	*p;

	ASSERT(bitoffs(bit) == 0);
	ASSERT(size == bitsz(char));
	dbprintf("\"");
	for (i = 0, p = (char *)obj + byteize(bit);
	     i < count && !seenint();
	     i++, p++) {
		if (*p == '\\' || *p == '\'' || *p == '"' || *p == '\?')
			dbprintf("\\%c", *p);
		else if (isgraph((int)*p) || *p == ' ')
			dbprintf("%c", *p);
		else if (*p == '\a' || *p == '\b' || *p == '\f' || *p == '\n' ||
			 *p == '\r' || *p == '\t' || *p == '\v')
			dbprintf("\\%c", *p + ('a' - '\a'));
		else
			dbprintf("\\%03o", *p & 0xff);
	}
	dbprintf("\"");
	return 1;
}

int
fp_num(
	void		*obj,
	int		bit,
	int		count,
	char		*fmtstr,
	int		size,
	int		arg,
	int		base,
	int		array)
{
	int		bitpos;
	int		i;
	int		isnull;
	int64_t		val;

	for (i = 0, bitpos = bit;
	     i < count && !seenint();
	     i++, bitpos += size) {
		val = getbitval(obj, bitpos, size,
			(arg & FTARG_SIGNED) ? BVSIGNED : BVUNSIGNED);
		if ((arg & FTARG_SKIPZERO) && val == 0)
			continue;
		isnull = (arg & FTARG_SIGNED) || size == 64 ?
			val == -1LL : val == ((1LL << size) - 1LL);
		if ((arg & FTARG_SKIPNULL) && isnull)
			continue;
		if (array && count > 1)
			dbprintf("%d:", i + base);
		if ((arg & FTARG_DONULL) && isnull)
			dbprintf(_("null"));
		else if (size > 32)
			dbprintf(fmtstr, val);
		else
			dbprintf(fmtstr, (int32_t)val);
		if (i < count - 1)
			dbprintf(" ");
	}
	return 1;
}

/*ARGSUSED*/
int
fp_sarray(
	void	*obj,
	int	bit,
	int	count,
	char	*fmtstr,
	int	size,
	int	arg,
	int	base,
	int	array)
{
	print_sarray(obj, bit, count, size, base, array,
		(const field_t *)fmtstr, (arg & FTARG_SKIPNMS) != 0);
	return 1;
}

/*ARGSUSED*/
int
fp_time(
	void	*obj,
	int	bit,
	int	count,
	char	*fmtstr,
	int	size,
	int	arg,
	int	base,
	int	array)
{
	int	bitpos;
	char	*c;
	int	i;
	time_t  t;

	ASSERT(bitoffs(bit) == 0);
	for (i = 0, bitpos = bit;
	     i < count && !seenint();
	     i++, bitpos += size) {
		if (array)
			dbprintf("%d:", i + base);
		t = (time_t)getbitval((char *)obj + byteize(bitpos), 0,
				sizeof(int32_t) * 8, BVSIGNED);
		c = ctime(&t);
		dbprintf("%24.24s", c);
		if (i < count - 1)
			dbprintf(" ");
	}
	return 1;
}

static void
fp_timespec64(
	struct timespec64	*ts)
{
	BUILD_BUG_ON(sizeof(long) != sizeof(time_t));

	if (ts->tv_sec > LONG_MAX || ts->tv_sec < LONG_MIN) {
		dbprintf("%lld", (long long)ts->tv_sec);
	} else {
		time_t	tt = ts->tv_sec;
		char	*c;

		c = ctime(&tt);
		dbprintf("%24.24s", c);
	}
}

int
fp_btsec(
	void			*obj,
	int			bit,
	int			count,
	char			*fmtstr,
	int			size,
	int			arg,
	int			base,
	int			array)
{
	struct timespec64	ts;
	union xfs_timestamp	*xts;
	int			bitpos;
	int			i;

	ASSERT(bitoffs(bit) == 0);
	for (i = 0, bitpos = bit;
	     i < count && !seenint();
	     i++, bitpos += size) {
		if (array)
			dbprintf("%d:", i + base);
		xts = obj + byteize(bitpos);
		libxfs_inode_from_disk_timestamp(obj, &ts, xts);
		fp_timespec64(&ts);
		if (i < count - 1)
			dbprintf(" ");
	}
	return 1;
}

int
fp_btnsec(
	void			*obj,
	int			bit,
	int			count,
	char			*fmtstr,
	int			size,
	int			arg,
	int			base,
	int			array)
{
	struct timespec64	ts;
	union xfs_timestamp	*xts;
	int			bitpos;
	int			i;

	ASSERT(bitoffs(bit) == 0);
	for (i = 0, bitpos = bit;
	     i < count && !seenint();
	     i++, bitpos += size) {
		if (array)
			dbprintf("%d:", i + base);
		xts = obj + byteize(bitpos);
		libxfs_inode_from_disk_timestamp(obj, &ts, xts);
		dbprintf("%u", ts.tv_nsec);
		if (i < count - 1)
			dbprintf(" ");
	}
	return 1;
}

int
fp_btqtimer(
	void			*obj,
	int			bit,
	int			count,
	char			*fmtstr,
	int			size,
	int			arg,
	int			base,
	int			array)
{
	struct timespec64	ts = { 0 };
	struct xfs_disk_dquot	*ddq = obj;
	__be32			*t;
	int			bitpos;
	int			i;

	ASSERT(bitoffs(bit) == 0);
	for (i = 0, bitpos = bit;
	     i < count && !seenint();
	     i++, bitpos += size) {
		if (array)
			dbprintf("%d:", i + base);
		t = obj + byteize(bitpos);
		libxfs_dquot_from_disk_timestamp(ddq, &ts.tv_sec, *t);

		/*
		 * Display the raw value if it's the default grace expiration
		 * period (root dquot) or if the quota has not expired.
		 */
		if (ddq->d_id == 0 || ts.tv_sec == 0)
			dbprintf("%llu", (unsigned long long)ts.tv_sec);
		else
			fp_timespec64(&ts);
		if (i < count - 1)
			dbprintf(" ");
	}
	return 1;
}

/*ARGSUSED*/
int
fp_uuid(
	void	*obj,
	int	bit,
	int	count,
	char	*fmtstr,
	int	size,
	int	arg,
	int	base,
	int	array)
{
	char	bp[40];	/* UUID string is 36 chars + trailing '\0' */
	int	i;
	uuid_t	*p;

	ASSERT(bitoffs(bit) == 0);
	for (p = (uuid_t *)((char *)obj + byteize(bit)), i = 0;
	     i < count && !seenint();
	     i++, p++) {
		if (array)
			dbprintf("%d:", i + base);
		platform_uuid_unparse(p, bp);
		dbprintf("%s", bp);
		if (i < count - 1)
			dbprintf(" ");
	}
	return 1;
}

/*
 * CRC is correct is the current buffer it is being pulled out
 * of is not marked with a EFSCORRUPTED error.
 */
int
fp_crc(
	void	*obj,
	int	bit,
	int	count,
	char	*fmtstr,
	int	size,
	int	arg,
	int	base,
	int	array)
{
	int		bitpos;
	int		i;
	int64_t		val;
	char		*ok;

	switch (iocur_crc_valid()) {
	case -1:
		ok = "unchecked";
		break;
	case 0:
		ok = "bad";
		break;
	case 1:
		ok = "correct";
		break;
	default:
		ok = "unknown state";
		break;
	}

	for (i = 0, bitpos = bit;
	     i < count && !seenint();
	     i++, bitpos += size) {
		if (array)
			dbprintf("%d:", i + base);
		val = getbitval(obj, bitpos, size, BVUNSIGNED);
		if (size > 32)
			dbprintf(fmtstr, val, ok);
		else
			dbprintf(fmtstr, (int32_t)val, ok);
		if (i < count - 1)
			dbprintf(" ");
	}
	return 1;
}
