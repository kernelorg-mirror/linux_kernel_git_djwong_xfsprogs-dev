// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2007 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "xfs_metadump.h"

static int	show_progress = 0;
static int	show_info = 0;
static int	progress_since_warning = 0;

static void
fatal(const char *msg, ...)
{
	va_list		args;

	va_start(args, msg);
	fprintf(stderr, "%s: ", progname);
	vfprintf(stderr, msg, args);
	exit(1);
}

static void
print_progress(const char *fmt, ...)
{
	char		buf[60];
	va_list		ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	buf[sizeof(buf)-1] = '\0';

	printf("\r%-59s", buf);
	fflush(stdout);
	progress_since_warning = 1;
}

extern int	platform_check_ismounted(char *, char *, struct stat *, int);

static int
open_device(
	char		*path,
	int		*is_target_file)
{
	struct stat	statbuf;
	int		open_flags = O_RDWR;
	int		dst_fd;

	*is_target_file = 0;
	if (stat(path, &statbuf) < 0)  {
		/* ok, assume it's a file and create it */
		open_flags |= O_CREAT;
		*is_target_file = 1;
	} else if (S_ISREG(statbuf.st_mode))  {
		open_flags |= O_TRUNC;
		*is_target_file = 1;
	} else  {
		/*
		 * check to make sure a filesystem isn't mounted on the device
		 */
		if (platform_check_ismounted(path, NULL, &statbuf, 0))
			fatal("a filesystem is mounted on target device \"%s\","
				" cannot restore to a mounted filesystem.\n",
				path);
	}

	dst_fd = open(path, open_flags, 0644);
	if (dst_fd < 0)
		fatal("couldn't open target \"%s\"\n", path);

	return dst_fd;
}

static void
check_dev(
	int			dst_fd,
	int			is_target_file,
	unsigned long long	bytes)
{
	if (is_target_file)  {
		/* ensure regular files are correctly sized */

		if (ftruncate(dst_fd, bytes))
			fatal("cannot set filesystem image size: %s\n",
				strerror(errno));
	} else  {
		/* ensure device is sufficiently large enough */

		char		*lb[XFS_MAX_SECTORSIZE] = { NULL };
		off64_t		off;

		off = bytes - sizeof(lb);
		if (pwrite(dst_fd, lb, sizeof(lb), off) < 0)
			fatal("failed to write last block, is target too "
				"small? (error: %s)\n", strerror(errno));
	}
}

/*
 * perform_restore() -- do the actual work to restore the metadump
 *
 * @src_f: A FILE pointer to the source metadump
 * @dst_fd: the file descriptor for the target file
 * @is_target_file: designates whether the target is a regular file
 * @mbp: pointer to metadump's first xfs_metablock, read and verified by the caller
 *
 * src_f should be positioned just past a read the previously validated metablock
 */
static void
perform_restore(
	FILE			*src_f,
	int			dst_fd,
	int			is_target_file,
	const struct xfs_metablock	*mbp,
	char			*log_path)
{
	struct xfs_metablock	*metablock;	/* header + index + blocks */
	__be64			*block_index;
	char			*block_buffer;
	int			block_size;
	int			max_indices;
	int			cur_index;
	int			mb_count;
	xfs_sb_t		sb;
	int64_t			bytes_read;
	int			log_fd = -1;
	bool			is_mdx;

	is_mdx = mbp->mb_magic == cpu_to_be32(XFS_MDX_MAGIC);

	block_size = 1 << mbp->mb_blocklog;
	max_indices = (block_size - sizeof(xfs_metablock_t)) / sizeof(__be64);

	metablock = (xfs_metablock_t *)calloc(max_indices + 1, block_size);
	if (metablock == NULL)
		fatal("memory allocation failure\n");

	mb_count = be16_to_cpu(mbp->mb_count);
	if (mb_count == 0 || mb_count > max_indices)
		fatal("bad block count: %u\n", mb_count);

	memcpy(metablock, mbp, sizeof(struct xfs_metablock));
	block_index = (__be64 *)((char *)metablock + sizeof(xfs_metablock_t));
	block_buffer = (char *)metablock + block_size;

	if (fread(block_index, block_size - sizeof(struct xfs_metablock), 1, src_f) != 1)
		fatal("error reading from metadump file\n");

	if (block_index[0] != 0)
		fatal("first block is not the primary superblock\n");


	if (fread(block_buffer, mb_count << mbp->mb_blocklog, 1, src_f) != 1)
		fatal("error reading from metadump file\n");

	libxfs_sb_from_disk(&sb, (struct xfs_dsb *)block_buffer);

	if (sb.sb_magicnum != XFS_SB_MAGIC)
		fatal("bad magic number for primary superblock\n");

	/*
	 * Normally the upper bound would be simply XFS_MAX_SECTORSIZE
	 * but the metadump format has a maximum number of BBSIZE blocks
	 * it can store in a single metablock.
	 */
	if (sb.sb_sectsize < XFS_MIN_SECTORSIZE ||
	    sb.sb_sectsize > XFS_MAX_SECTORSIZE ||
	    sb.sb_sectsize > max_indices * block_size)
		fatal("bad sector size %u in metadump image\n", sb.sb_sectsize);

	((struct xfs_dsb*)block_buffer)->sb_inprogress = 1;

	check_dev(dst_fd, is_target_file, sb.sb_dblocks * sb.sb_blocksize);

	bytes_read = 0;

	for (;;) {
		int		write_fd = dst_fd;

		if (metablock->mb_magic != mbp->mb_magic)
			fatal("magic value 0x%x wrong, expected 0x%x\n",
					metablock->mb_magic, mbp->mb_magic);

		if (metablock->mb_info & XFS_METADUMP_LOGDEV) {
			int	log_is_file;

			if (!is_mdx)
				fatal("logdev set on an old style metadump?\n");
			if (log_fd == -1) {
				if (!log_path)
					fatal(
	"metadump has log contents but -l was not specified?\n");
				log_fd = open_device(log_path, &log_is_file);
				check_dev(log_fd, log_is_file,
						sb.sb_logblocks * sb.sb_blocksize);
			}
			write_fd = log_fd;
		}
		if (metablock->mb_info & XFS_METADUMP_RTDEV) {
			if (!is_mdx)
				fatal("rtdev set on an old style metadump?\n");
			fatal("rtdev not supported\n");
		}

		if (show_progress && (bytes_read & ((1 << 20) - 1)) == 0)
			print_progress("%lld MB read", bytes_read >> 20);

		for (cur_index = 0; cur_index < mb_count; cur_index++) {
			if (pwrite(write_fd, &block_buffer[cur_index <<
					mbp->mb_blocklog], block_size,
					be64_to_cpu(block_index[cur_index]) <<
						BBSHIFT) < 0)
				fatal("error writing block %llu: %s\n",
					be64_to_cpu(block_index[cur_index]) << BBSHIFT,
					strerror(errno));
		}
		if (is_mdx) {
			size_t	nr = fread(metablock, block_size, 1, src_f);

			if (nr == 0)
				break;
			if (nr != 1)
				fatal("error reading from extended metadump file\n");
		} else {
			if (mb_count < max_indices)
				break;

			if (fread(metablock, block_size, 1, src_f) != 1)
				fatal("error reading from metadump file\n");
		}

		mb_count = be16_to_cpu(metablock->mb_count);
		if (mb_count == 0)
			break;
		if (mb_count > max_indices)
			fatal("bad block count: %u\n", mb_count);

		if (fread(block_buffer, mb_count << mbp->mb_blocklog,
								1, src_f) != 1)
			fatal("error reading from metadump file\n");

		bytes_read += block_size + (mb_count << mbp->mb_blocklog);
	}

	if (progress_since_warning)
		putchar('\n');

	memset(block_buffer, 0, sb.sb_sectsize);
	sb.sb_inprogress = 0;
	libxfs_sb_to_disk((struct xfs_dsb *)block_buffer, &sb);
	if (xfs_sb_version_hascrc(&sb))
		xfs_update_cksum(block_buffer, sb.sb_sectsize, XFS_SB_CRC_OFF);

	if (pwrite(dst_fd, block_buffer, sb.sb_sectsize, 0) < 0)
		fatal("error writing primary superblock: %s\n", strerror(errno));

	if (log_fd >= 0)
		close(log_fd);

	free(metablock);
}

static void
usage(void)
{
	fprintf(stderr, "Usage: %s [-V] [-g] [-i] [-l logdev] source target\n", progname);
	exit(1);
}

int
main(
	int 		argc,
	char 		**argv)
{
	char		*log_path = NULL;
	FILE		*src_f;
	int		dst_fd;
	int		c;
	int		is_target_file;
	struct xfs_metablock	mb;

	progname = basename(argv[0]);

	while ((c = getopt(argc, argv, "gl:iV")) != EOF) {
		switch (c) {
			case 'g':
				show_progress = 1;
				break;
			case 'l':
				log_path = optarg;
				break;
			case 'i':
				show_info = 1;
				break;
			case 'V':
				printf("%s version %s\n", progname, VERSION);
				exit(0);
			default:
				usage();
		}
	}

	if (argc - optind < 1 || argc - optind > 2)
		usage();

	/* show_info without a target is ok */
	if (!show_info && argc - optind != 2)
		usage();

	/*
	 * open source and test if this really is a dump. The first metadump block
	 * will be passed to perform_restore() which will continue to read the
	 * file from this point. This avoids rewind the stream, which causes
	 * restore to fail when source was being read from stdin.
 	 */
	if (strcmp(argv[optind], "-") == 0) {
		src_f = stdin;
		if (isatty(fileno(stdin)))
			fatal("cannot read from a terminal\n");
	} else {
		src_f = fopen(argv[optind], "rb");
		if (src_f == NULL)
			fatal("cannot open source dump file\n");
	}

	if (fread(&mb, sizeof(mb), 1, src_f) != 1)
		fatal("error reading from metadump file\n");
	if (mb.mb_magic != cpu_to_be32(XFS_MD_MAGIC) &&
	    mb.mb_magic != cpu_to_be32(XFS_MDX_MAGIC))
		fatal("specified file is not a metadata dump\n");

	if (show_info) {
		if (mb.mb_info & XFS_METADUMP_INFO_FLAGS) {
			printf("%s: %sobfuscated, %s log, %s metadata blocks\n",
			argv[optind],
			mb.mb_info & XFS_METADUMP_OBFUSCATED ? "":"not ",
			mb.mb_info & XFS_METADUMP_DIRTYLOG ? "dirty":"clean",
			mb.mb_info & XFS_METADUMP_FULLBLOCKS ? "full":"zeroed");
		} else {
			printf("%s: no informational flags present\n",
				argv[optind]);
		}

		if (argc - optind == 1)
			exit(0);
	}

	optind++;

	/* check and open target */
	dst_fd = open_device(argv[optind], &is_target_file);

	perform_restore(src_f, dst_fd, is_target_file, &mb, log_path);

	close(dst_fd);
	if (src_f != stdin)
		fclose(src_f);

	return 0;
}
