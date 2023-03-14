// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2023 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "platform_defs.h"
#include "command.h"
#include "init.h"
#include "io.h"
#include "input.h"
#include <sys/types.h>
#include <sys/xattr.h>

#define rol32(x,y)             (((x) << (y)) | ((x) >> (32 - (y))))

static void
namehash_help(void)
{
	printf(_(
"\n"
" Prints the dirattr hash of the given names."
"\n"
" -i -- read standard input for the name, up to %d bytes.\n"
"\n"),
			MAXNAMELEN - 1);
}

/* XXX copied from xfs_da_btree.c */
static unsigned int
hashname(
	const unsigned char	*name,
	size_t			namelen)
{
	unsigned int		hash;

	/*
	 * Do four characters at a time as long as we can.
	 */
	for (hash = 0; namelen >= 4; namelen -= 4, name += 4)
		hash = (name[0] << 21) ^ (name[1] << 14) ^ (name[2] << 7) ^
		       (name[3] << 0) ^ rol32(hash, 7 * 4);

	/*
	 * Now do the rest of the characters.
	 */
	switch (namelen) {
	case 3:
		return (name[0] << 14) ^ (name[1] << 7) ^ (name[2] << 0) ^
		       rol32(hash, 7 * 3);
	case 2:
		return (name[0] << 7) ^ (name[1] << 0) ^ rol32(hash, 7 * 2);
	case 1:
		return (name[0] << 0) ^ rol32(hash, 7 * 1);
	default: /* case 0: */
		return hash;
	}
}

#define is_invalid_char(c)	((c) == '/' || (c) == '\0')

static inline unsigned char
random_filename_char(void)
{
	static unsigned char filename_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
						"abcdefghijklmnopqrstuvwxyz"
						"0123456789-_";

	return filename_alphabet[random() % (sizeof filename_alphabet - 1)];
}

/*
 * Given a name and its hash value, massage the name in such a way
 * that the result is another name of equal length which shares the
 * same hash value.
 */
static void
obfuscate_name(
	unsigned int	hash,
	size_t		name_len,
	unsigned char	*name)
{
	unsigned char	*newp = name;
	int		i;
	unsigned int	new_hash = 0;
	unsigned char	*first;
	unsigned char	high_bit;
	int		shift;

	/*
	 * Our obfuscation algorithm requires at least 5-character
	 * names, so don't bother if the name is too short.  We
	 * work backward from a hash value to determine the last
	 * five bytes in a name required to produce a new name
	 * with the same hash.
	 */
	if (name_len < 5)
		return;

	/*
	 * The beginning of the obfuscated name can be pretty much
	 * anything, so fill it in with random characters.
	 * Accumulate its new hash value as we go.
	 */
	for (i = 0; i < name_len - 5; i++) {
		*newp = random_filename_char();
		new_hash = *newp ^ rol32(new_hash, 7);
		newp++;
	}

	/*
	 * Compute which five bytes need to be used at the end of
	 * the name so the hash of the obfuscated name is the same
	 * as the hash of the original.  If any result in an invalid
	 * character, flip a bit and arrange for a corresponding bit
	 * in a neighboring byte to be flipped as well.  For the
	 * last byte, the "neighbor" to change is the first byte
	 * we're computing here.
	 */
	new_hash = rol32(new_hash, 3) ^ hash;

	first = newp;
	high_bit = 0;
	for (shift = 28; shift >= 0; shift -= 7) {
		*newp = (new_hash >> shift & 0x7f) ^ high_bit;
		if (is_invalid_char(*newp)) {
			*newp ^= 1;
			high_bit = 0x80;
		} else
			high_bit = 0;
		ASSERT(!is_invalid_char(*newp));
		newp++;
	}

	/*
	 * If we flipped a bit on the last byte, we need to fix up
	 * the matching bit in the first byte.  The result will
	 * be a valid character, because we know that first byte
	 * has 0's in its upper four bits (it was produced by a
	 * 28-bit right-shift of a 32-bit unsigned value).
	 */
	if (high_bit) {
		*first ^= 0x10;
		ASSERT(!is_invalid_char(*first));
	}
}

static int
namehash_f(
	int		argc,
	char		**argv)
{
	bool		read_stdin = false;
	int		c;

	while ((c = getopt(argc, argv, "i")) != EOF) {
		switch (c) {
		case 'i':
			read_stdin = true;
			break;
		default:
			exitcode = 1;
			namehash_help();
			return 0;
		}
	}

	if (read_stdin) {
		char	buf[MAXNAMELEN];
		size_t	len;

		len = fread(buf, 1, MAXNAMELEN - 1, stdin);

		printf("0x%08x\n", hashname(buf, len));
		return 0;
	}

	for (c = optind; c < argc; c++) {
		printf("0x%08x %s\n", hashname(argv[c], strlen(argv[c])),
				argv[c]);
	}

	return 0;
}

static void
hashcoll_help(void)
{
	printf(_(
"\n"
" Creates names in the open file with colliding dahash values."
"\n"
" -d -- create directory entries.  This is the default for directories.\n"
" -a -- create extended attributes.  This is the default for nondirectories.\n"
" -i -- read standard input for the name, up to %d bytes.\n"
" -n -- create this many names.\n"
" -s -- seed the rng with this value.\n"
"\n"),
			MAXNAMELEN - 1);
}

static int
collide_dirents(
	unsigned long		nr,
	const unsigned char	*name,
	size_t			namelen)
{
	unsigned char		direntname[MAXNAMELEN];
	unsigned int		hash = hashname(name, namelen);
	unsigned long		i;
	int			newfd, ret;

#ifdef HAVE_OPENAT
	newfd = openat(file->fd, name, O_CREAT, 0700);
	if (newfd < 0) {
		perror(name);
		return 1;
	}

	for (i = 0; i < nr; i++) {
		strncpy(direntname, name, MAXNAMELEN - 1);
		obfuscate_name(hash, namelen, direntname);

		printf("%lu/%lu: '%s' -> '%s'\n", i, nr, name, direntname);

		ret = linkat(file->fd, name, file->fd, direntname, 0);
		if (ret < 0) {
			perror(direntname);
			return 1;
		}
	}

	close(newfd);
#else
	printf(_("openat not supported?!\n"));
	exitcode = 1;
#endif

	return 0;
}

static int
collide_xattrs(
	unsigned long		nr,
	const unsigned char	*name,
	size_t			namelen)
{
	unsigned char		xattrname[MAXNAMELEN + 5];
	unsigned int		hash = hashname(name, namelen);
	unsigned long		i;
	int			ret;

	snprintf(xattrname, MAXNAMELEN + 5, "user.%s", name);
	ret = fsetxattr(file->fd, xattrname, "1", 1, 0);
	if (ret) {
		perror(name);
		return 1;
	}

	for (i = 0; i < nr; i++) {
		snprintf(xattrname, MAXNAMELEN + 5, "user.%s", name);
		obfuscate_name(hash, namelen, xattrname + 5);

		printf("%lu/%lu: '%s' -> '%s'\n", i, nr, name, xattrname);

		ret = fsetxattr(file->fd, xattrname, "1", 1, 0);
		if (ret) {
			perror(xattrname);
			return 1;
		}
	}

	return 0;
}

static int
hashcoll_f(
	int		argc,
	char		**argv)
{
	struct stat	statbuf = { };
	bool		create_dirent = false, create_xattr = false;
	bool		read_stdin = false;
	bool		create_defaults = true;
	unsigned long	nr = 1, seed = 0;
	int		c;

	c = fstat(file->fd, &statbuf);
	if (c == 0) {
		if (S_ISDIR(statbuf.st_mode))
			create_dirent = true;
		else
			create_xattr = true;
	}

	while ((c = getopt(argc, argv, "adin:s:")) != EOF) {
		switch (c) {
		case 'a':
			if (create_defaults)
				create_dirent = false;
			create_xattr = true;
			create_defaults = false;
			break;
		case 'd':
			if (create_defaults)
				create_xattr = false;
			create_dirent = true;
			create_defaults = false;
			break;
		case 'i':
			read_stdin = true;
			break;
		case 'n':
			nr = strtoul(optarg, NULL, 10);
			break;
		case 's':
			seed = strtoul(optarg, NULL, 10);
			break;
		default:
			exitcode = 1;
			hashcoll_help();
			return 0;
		}
	}

	if (create_dirent && !S_ISDIR(statbuf.st_mode)) {
		printf(_("cannot create dirents in nondirectory\n"));
		exitcode = 1;
		return 0;
	}

	if (create_xattr && !(S_ISDIR(statbuf.st_mode) ||
			      S_ISREG(statbuf.st_mode))) {
		printf(_("cannot create xattrs in special file\n"));
		exitcode = 1;
		return 0;
	}

	if (seed)
		srandom(seed);

	if (read_stdin) {
		char	buf[MAXNAMELEN];
		size_t	len;

		len = fread(buf, 1, MAXNAMELEN - 1, stdin);

		if (len < 5) {
			printf(_("name not long enough to hash\n"));
			exitcode = 1;
			return 0;
		}

		if (create_dirent)
			collide_dirents(nr, buf, len);
		if (create_xattr)
			collide_xattrs(nr, buf, len);
		return 0;
	}

	for (c = optind; c < argc; c++) {
		size_t	len = strlen(argv[c]);

		if (len < 5) {
			printf(_("%s: name not long enough to hash\n"),
					argv[c]);
			exitcode = 1;
			continue;
		}

		if (create_dirent)
			collide_dirents(nr, argv[c], len);
		if (create_xattr)
			collide_xattrs(nr, argv[c], len);
	}

	return 0;
}

static cmdinfo_t	namehash_cmd = {
	.name		= "namehash",
	.cfunc		= namehash_f,
	.argmin		= 0,
	.argmax		= -1,
	.flags		= CMD_NOMAP_OK | CMD_FLAG_FOREIGN_OK,
	.args		= N_("-i|names..."),
	.oneline	= N_("print dir-attr hash of a name"),
	.help		= namehash_help,
};

static cmdinfo_t	hashcoll_cmd = {
	.name		= "hashcoll",
	.cfunc		= hashcoll_f,
	.argmin		= 0,
	.argmax		= -1,
	.flags		= CMD_NOMAP_OK | CMD_FLAG_FOREIGN_OK,
	.args		= N_("-d|-a [-nr n] -i|names..."),
	.oneline	= N_("create dirent or xattrs with colliding names"),
	.help		= hashcoll_help,
};

void
namehash_init(void)
{
	add_command(&namehash_cmd);
	add_command(&hashcoll_cmd);
}
