// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2021-2022 HUAWEI, Inc.
 *             http://www.huawei.com/
 * Created by Wang Qi <mpiglet@outlook.com>
 *            Guo Xuenan <guoxuenan@huawei.com>
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <getopt.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <lz4.h>
#include "erofs/print.h"
#include "erofs/io.h"

#ifdef HAVE_LIBUUID
#include <uuid.h>
#endif

struct dumpcfg {
	bool print_superblock;
	bool print_inode;
	bool print_statistic;
	bool print_inode_phy;
	bool print_version;
	u64 ino;
	u64 ino_phy;
};
static struct dumpcfg dumpcfg;

static const char chart_format[] = "%-16s	%-11d %8.2f%% |%-50s|\n";
static const char header_format[] = "%-16s %11s %16s |%-50s|\n";
static char *file_types[] = {
	".so", ".png", ".jpg", ".xml", ".html", ".odex",
	".vdex", ".apk", ".ttf", ".jar", ".json", ".ogg",
	".oat", ".art", ".rc", ".otf", ".txt", "others",
};
#define OTHERFILETYPE	ARRAY_SIZE(file_types)
/* (1 << FILE_MAX_SIZE_BITS)KB */
#define	FILE_MAX_SIZE_BITS	16

static const char * const file_category_types[] = {
	[EROFS_FT_UNKNOWN] = "unknown type",
	[EROFS_FT_REG_FILE] = "regular file",
	[EROFS_FT_DIR] = "directory",
	[EROFS_FT_CHRDEV] = "char dev",
	[EROFS_FT_BLKDEV] = "block dev",
	[EROFS_FT_FIFO] = "FIFO file",
	[EROFS_FT_SOCK] = "SOCK file",
	[EROFS_FT_SYMLINK] = "symlink file",
};

struct statistics {
	unsigned long blocks;
	unsigned long files;
	unsigned long files_total_size;
	unsigned long files_total_origin_size;
	double compress_rate;
	unsigned long compressed_files;
	unsigned long uncompressed_files;

	unsigned long regular_files;
	unsigned long dir_files;
	unsigned long chardev_files;
	unsigned long blkdev_files;
	unsigned long fifo_files;
	unsigned long sock_files;
	unsigned long symlink_files;

	/* statistics the number of files based on inode_info->flags */
	unsigned long file_category_stat[EROFS_FT_MAX];
	/* statistics the number of files based on file name extensions */
	unsigned int file_type_stat[OTHERFILETYPE];
	/* statistics the number of files based on file orignial size */
	unsigned int file_original_size[FILE_MAX_SIZE_BITS + 1];
	/* statistics the number of files based on the compressed
	 * size of file
	 */
	unsigned int file_comp_size[FILE_MAX_SIZE_BITS + 1];
};
static struct statistics stats;

static struct option long_options[] = {
	{"help", no_argument, 0, 1},
	{0, 0, 0, 0},
};

#define EROFS_FEATURE_COMPAT	0
#define EROFS_FEATURE_INCOMPAT	1

struct feature {
	int compat;
	unsigned int mask;
	const char *name;
};

static struct feature feature_lists[] = {
	{	EROFS_FEATURE_COMPAT, EROFS_FEATURE_COMPAT_SB_CHKSUM,
		"superblock-checksum"	},

	{	EROFS_FEATURE_INCOMPAT, EROFS_FEATURE_INCOMPAT_LZ4_0PADDING,
		"lz4-0padding"	},
	{	EROFS_FEATURE_INCOMPAT, EROFS_FEATURE_INCOMPAT_BIG_PCLUSTER,
		"big-pcluster"	},

	{	0, 0, 0	},
};

static void usage(void)
{
	fputs("usage: [options] erofs-image\n\n"
		"Dump erofs layout from erofs-image, and [options] are:\n"
		"--help  display this help and exit.\n"
		"-i #    print target # inode info\n"
		"-I #    print target # inode on-disk info\n"
		"-s      print information about superblock\n"
		"-S      print statistic information of the erofs-image\n"
		"-V      print the version number of dump.erofs and exit.\n",
		stdout);
}

static void dumpfs_print_version(void)
{
	fprintf(stdout, "dump.erofs %s\n", cfg.c_version);
}

static int dumpfs_parse_options_cfg(int argc, char **argv)
{
	int opt;
	u64 i;

	while ((opt = getopt_long(argc, argv, "i:I:sSV",
					long_options, NULL)) != -1) {
		switch (opt) {
		case 'i':
			i = atoll(optarg);
			dumpcfg.print_inode = true;
			dumpcfg.ino = i;
			break;
		case 'I':
			i = atoll(optarg);
			dumpcfg.print_inode_phy = true;
			dumpcfg.ino_phy = i;
			break;
		case 's':
			dumpcfg.print_superblock = true;
			break;
		case 'S':
			dumpcfg.print_statistic = true;
			break;
		case 'V':
			dumpfs_print_version();
			exit(0);
		case 1:
		    usage();
		    exit(0);
		default: /* '?' */
			return -EINVAL;
		}
	}

	if (optind >= argc)
		return -EINVAL;

	cfg.c_img_path = strdup(argv[optind++]);
	if (!cfg.c_img_path)
		return -ENOMEM;

	if (optind < argc) {
		erofs_err("unexpected argument: %s\n", argv[optind]);
		return -EINVAL;
	}
	return 0;
}

static int get_file_compressed_size(struct erofs_inode *inode,
		erofs_off_t *size)
{
	*size = 0;
	switch (inode->datalayout) {
	case EROFS_INODE_FLAT_INLINE:
	case EROFS_INODE_FLAT_PLAIN:
		stats.uncompressed_files++;
		*size = inode->i_size;
		break;
	case EROFS_INODE_FLAT_COMPRESSION_LEGACY:
	case EROFS_INODE_FLAT_COMPRESSION:
		stats.compressed_files++;
		*size = inode->u.i_blocks * EROFS_BLKSIZ;
		break;
	default:
		erofs_err("unknown datalayout");
		return -1;
	}
	return 0;
}

static int get_path_by_nid(erofs_nid_t nid, erofs_nid_t parent_nid,
		erofs_nid_t target, char *path, unsigned int pos)
{
	int err;
	struct erofs_inode inode = {.nid = nid};
	erofs_off_t offset;
	char buf[EROFS_BLKSIZ];

	path[pos++] = '/';
	if (target == sbi.root_nid)
		return 0;

	err = erofs_read_inode_from_disk(&inode);
	if (err) {
		erofs_err("read inode %lu failed", nid);
		return err;
	}

	offset = 0;
	while (offset < inode.i_size) {
		erofs_off_t maxsize = min_t(erofs_off_t,
					inode.i_size - offset, EROFS_BLKSIZ);
		struct erofs_dirent *de = (void *)buf;
		struct erofs_dirent *end;
		unsigned int nameoff;

		err = erofs_pread(&inode, buf, maxsize, offset);
		if (err)
			return err;

		nameoff = le16_to_cpu(de->nameoff);
		if (nameoff < sizeof(struct erofs_dirent) ||
		    nameoff >= PAGE_SIZE) {
			erofs_err("invalid de[0].nameoff %u @ nid %llu",
				  nameoff, nid | 0ULL);
			return -EFSCORRUPTED;
		}

		end = (void *)buf + nameoff;
		while (de < end) {
			const char *dname;
			unsigned int dname_len;

			nameoff = le16_to_cpu(de->nameoff);
			dname = (char *)buf + nameoff;
			if (de + 1 >= end)
				dname_len = strnlen(dname, maxsize - nameoff);
			else
				dname_len = le16_to_cpu(de[1].nameoff)
					- nameoff;

			/* a corrupted entry is found */
			if (nameoff + dname_len > maxsize ||
			    dname_len > EROFS_NAME_LEN) {
				erofs_err("bogus dirent @ nid %llu",
						le64_to_cpu(de->nid) | 0ULL);
				DBG_BUGON(1);
				return -EFSCORRUPTED;
			}

			if (de->nid == target) {
				memcpy(path + pos, dname, dname_len);
				return 0;
			}

			if (de->file_type == EROFS_FT_DIR &&
					de->nid != parent_nid &&
					de->nid != nid) {
				memcpy(path + pos, dname, dname_len);
				err = get_path_by_nid(de->nid, nid,
						target, path, pos + dname_len);
				if (!err)
					return 0;
				memset(path + pos, 0, dname_len);
			}
			++de;
		}
		offset += maxsize;
	}
	return -1;
}

static void dumpfs_print_inode_phy(void)
{
	int err;
	erofs_nid_t nid = dumpcfg.ino_phy;
	struct erofs_inode inode = {.nid = nid};
	char path[PATH_MAX + 1] = {0};

	err = erofs_read_inode_from_disk(&inode);
	if (err < 0) {
		erofs_err("read inode %lu from disk failed", nid);
		return;
	}

	const erofs_off_t ibase = iloc(inode.nid);
	const erofs_off_t pos = Z_EROFS_VLE_LEGACY_INDEX_ALIGN(
			ibase + inode.inode_isize + inode.xattr_isize);
	erofs_blk_t blocks = inode.u.i_blocks;
	erofs_blk_t start = 0;
	erofs_blk_t end = 0;
	struct erofs_map_blocks map = {
		.index = UINT_MAX,
		.m_la = 0,
	};

	fprintf(stdout, "Inode %lu on-disk info:\n", nid);
	err = get_path_by_nid(sbi.root_nid, sbi.root_nid, nid, path, 0);
	if (!err)
		fprintf(stderr, "File Path:			%s\n",
				path);
	else
		erofs_err("path not found");

	switch (inode.datalayout) {
	case EROFS_INODE_FLAT_INLINE:
	case EROFS_INODE_FLAT_PLAIN:
		if (inode.u.i_blkaddr == NULL_ADDR)
			start = end = erofs_blknr(pos);
		else {
			start = inode.u.i_blkaddr;
			end = start + BLK_ROUND_UP(inode.i_size) - 1;
		}
		fprintf(stdout, "File size:			%lu\n",
				inode.i_size);
		fprintf(stdout,
				"Plain Block Address:		%u - %u\n",
				start, end);
		break;

	case EROFS_INODE_FLAT_COMPRESSION_LEGACY:
	case EROFS_INODE_FLAT_COMPRESSION:
		err = z_erofs_map_blocks_iter(&inode, &map, 0);
		if (err)
			erofs_err("get file blocks range failed");

		start = erofs_blknr(map.m_pa);
		end = start - 1 + blocks;
		fprintf(stderr,
				"Compressed Block Address:	%u - %u\n",
				start, end);
		break;
	}
}

static void dumpfs_print_inode(void)
{
	int err;
	erofs_off_t size;
	u16 access_mode;
	time_t t;
	erofs_nid_t nid = dumpcfg.ino;
	struct erofs_inode inode = {.nid = nid};
	char path[PATH_MAX + 1] = {0};
	char access_mode_str[] = "rwxrwxrwx";

	err = erofs_read_inode_from_disk(&inode);
	if (err) {
		erofs_err("read inode %lu from disk failed", nid);
		return;
	}

	err = get_file_compressed_size(&inode, &size);
	if (err) {
		erofs_err("get file size failed\n");
		return;
	}

	fprintf(stdout, "Inode %lu info:\n", dumpcfg.ino);
	err = get_path_by_nid(sbi.root_nid, sbi.root_nid, nid, path, 0);

	fprintf(stdout, "File path:            %s\n",
			!err ? path : "path not found");
	fprintf(stdout, "File nid:             %lu\n", inode.nid);
	fprintf(stdout, "File inode core size: %d\n", inode.inode_isize);
	fprintf(stdout, "File original size:   %lu\n", inode.i_size);
	fprintf(stdout,	"File on-disk size:    %lu\n", size);
	fprintf(stdout, "File compress rate:   %.2f%%\n",
			(double)(100 * size) / (double)(inode.i_size));
	fprintf(stdout, "File extent size:     %u\n", inode.extent_isize);
	fprintf(stdout,	"File xattr size:      %u\n", inode.xattr_isize);
	fprintf(stdout, "File type:            ");
	switch (inode.i_mode & S_IFMT) {
	case S_IFBLK:  fprintf(stdout, "block device\n");     break;
	case S_IFCHR:  fprintf(stdout, "character device\n"); break;
	case S_IFDIR:  fprintf(stdout, "directory\n");        break;
	case S_IFIFO:  fprintf(stdout, "FIFO/pipe\n");        break;
	case S_IFLNK:  fprintf(stdout, "symlink\n");          break;
	case S_IFREG:  fprintf(stdout, "regular file\n");     break;
	case S_IFSOCK: fprintf(stdout, "socket\n");           break;
	default:       fprintf(stdout, "unknown?\n");         break;
	}

	access_mode = inode.i_mode & 0777;
	t = inode.i_ctime;
	for (int i = 8; i >= 0; i--)
		if (((access_mode >> i) & 1) == 0)
			access_mode_str[8 - i] = '-';
	fprintf(stdout, "File access:          %04o/%s\n",
			access_mode, access_mode_str);
	fprintf(stdout, "File uid:             %u\n", inode.i_uid);
	fprintf(stdout, "File gid:             %u\n", inode.i_gid);
	fprintf(stdout, "File datalayout:      %d\n", inode.datalayout);
	fprintf(stdout,	"File nlink:           %u\n", inode.i_nlink);
	fprintf(stdout, "File create time:     %s", ctime(&t));
	fprintf(stdout, "File access time:     %s", ctime(&t));
	fprintf(stdout, "File modify time:     %s", ctime(&t));
}

static int get_file_type(const char *filename)
{
	char *postfix = strrchr(filename, '.');
	int type = 0;

	if (postfix == NULL)
		return OTHERFILETYPE - 1;
	while (type < OTHERFILETYPE - 1) {
		if (strcmp(postfix, file_types[type]) == 0)
			break;
		type++;
	}
	return type;
}

static void update_file_size_statatics(erofs_off_t occupied_size,
		erofs_off_t original_size)
{
	int occupied_size_mark;
	int original_size_mark;

	original_size_mark = 0;
	occupied_size_mark = 0;
	occupied_size >>= 10;
	original_size >>= 10;

	while (occupied_size || original_size) {
		if (occupied_size) {
			occupied_size >>= 1;
			occupied_size_mark++;
		}
		if (original_size) {
			original_size >>= 1;
			original_size_mark++;
		}
	}

	if (original_size_mark >= FILE_MAX_SIZE_BITS)
		stats.file_original_size[FILE_MAX_SIZE_BITS]++;
	else
		stats.file_original_size[original_size_mark]++;

	if (occupied_size_mark >= FILE_MAX_SIZE_BITS)
		stats.file_comp_size[FILE_MAX_SIZE_BITS]++;
	else
		stats.file_comp_size[occupied_size_mark]++;
}

static int erofs_read_dir(erofs_nid_t nid, erofs_nid_t parent_nid)
{
	struct erofs_inode vi = { .nid = nid};
	int err;
	char buf[EROFS_BLKSIZ];
	char filename[PATH_MAX + 1];
	erofs_off_t offset;

	err = erofs_read_inode_from_disk(&vi);
	if (err)
		return err;

	offset = 0;
	while (offset < vi.i_size) {
		erofs_off_t maxsize = min_t(erofs_off_t,
			vi.i_size - offset, EROFS_BLKSIZ);
		struct erofs_dirent *de = (void *)buf;
		struct erofs_dirent *end;
		unsigned int nameoff;

		err = erofs_pread(&vi, buf, maxsize, offset);
		if (err)
			return err;

		nameoff = le16_to_cpu(de->nameoff);

		if (nameoff < sizeof(struct erofs_dirent) ||
		    nameoff >= PAGE_SIZE) {
			erofs_err("invalid de[0].nameoff %u @ nid %llu",
				  nameoff, nid | 0ULL);
			return -EFSCORRUPTED;
		}
		end = (void *)buf + nameoff;
		while (de < end) {
			const char *dname;
			unsigned int dname_len;
			struct erofs_inode inode = { .nid = de->nid };
			erofs_off_t occupied_size = 0;

			nameoff = le16_to_cpu(de->nameoff);
			dname = (char *)buf + nameoff;

			if (de + 1 >= end)
				dname_len = strnlen(dname, maxsize - nameoff);
			else
				dname_len =
					le16_to_cpu(de[1].nameoff) - nameoff;

			/* a corrupted entry is found */
			if (nameoff + dname_len > maxsize ||
				dname_len > EROFS_NAME_LEN) {
				erofs_err("bogus dirent @ nid %llu",
						le64_to_cpu(de->nid) | 0ULL);
				DBG_BUGON(1);
				return -EFSCORRUPTED;
			}
			if (de->nid != nid && de->nid != parent_nid)
				stats.files++;

			memset(filename, 0, PATH_MAX + 1);
			memcpy(filename, dname, dname_len);
			if (de->file_type >= EROFS_FT_MAX) {
				erofs_err("invalid file type %llu", de->nid);
				continue;
			}
			if (de->file_type != EROFS_FT_DIR)
				stats.file_category_stat[de->file_type]++;
			switch (de->file_type) {
			case EROFS_FT_REG_FILE:
				err = erofs_read_inode_from_disk(&inode);
				if (err) {
					erofs_err("read file inode from disk failed!");
					return err;
				}
				stats.files_total_origin_size += inode.i_size;
				stats.file_type_stat[get_file_type(filename)]++;

				err = get_file_compressed_size(&inode,
						&occupied_size);
				if (err) {
					erofs_err("get file size failed\n");
					return err;
				}
				stats.files_total_size += occupied_size;
				update_file_size_statatics(occupied_size, inode.i_size);
				break;

			case EROFS_FT_DIR:
				if (de->nid != nid && de->nid != parent_nid) {
					stats.uncompressed_files++;
					err = erofs_read_dir(de->nid, nid);
					if (err) {
						fprintf(stdout,
								"parse dir nid %llu error occurred\n",
								de->nid);
						return err;
					}
					stats.file_category_stat[EROFS_FT_DIR]++;
				}
				break;
			case EROFS_FT_UNKNOWN:
			case EROFS_FT_CHRDEV:
			case EROFS_FT_BLKDEV:
			case EROFS_FT_FIFO:
			case EROFS_FT_SOCK:
			case EROFS_FT_SYMLINK:
				stats.uncompressed_files++;
				break;
			default:
				erofs_err("%d file type not exists", de->file_type);
			}
			++de;
		}
		offset += maxsize;
	}
	return 0;
}

static void dumpfs_print_statistic_of_filetype(void)
{
	fprintf(stdout, "Filesystem total file count:		%lu\n",
			stats.files);
	for (int i = 0; i < EROFS_FT_MAX; i++)
		fprintf(stdout, "Filesystem %s count:		%lu\n",
			file_category_types[i], stats.file_category_stat[i]);
}

static void dumpfs_print_chart_row(char *col1, unsigned int col2,
		double col3, char *col4)
{
	char row[500] = {0};

	sprintf(row, chart_format, col1, col2, col3, col4);
	fprintf(stdout, row);
}

static void dumpfs_print_chart_of_file(unsigned int *file_counts,
		unsigned int len)
{
	char col1[30];
	unsigned int col2;
	double col3;
	char col4[400];
	unsigned int lowerbound = 0;
	unsigned int upperbound = 1;

	fprintf(stdout, header_format, ">=(KB) .. <(KB) ", "count",
			"ratio", "distribution");
	for (int i = 0; i < len; i++) {
		memset(col1, 0, sizeof(col1));
		memset(col4, 0, sizeof(col4));
		if (i == len - 1)
			sprintf(col1, "%6d ..", lowerbound);
		else if (i <= 6)
			sprintf(col1, "%6d .. %-6d", lowerbound, upperbound);
		else

			sprintf(col1, "%6d .. %-6d", lowerbound, upperbound);
		col2 = file_counts[i];
		col3 = (double)(100 * col2) / (double)stats.file_category_stat[EROFS_FT_REG_FILE];
		memset(col4, '#', col3 / 2);
		dumpfs_print_chart_row(col1, col2, col3, col4);
		lowerbound = upperbound;
		upperbound <<= 1;
	}
}

static void dumpfs_print_chart_of_file_type(char **file_types, unsigned int len)
{
	char col1[30];
	unsigned int col2;
	double col3;
	char col4[401];

	fprintf(stdout, header_format, "type", "count", "ratio",
			"distribution");
	for (int i = 0; i < len; i++) {
		memset(col1, 0, sizeof(col1));
		memset(col4, 0, sizeof(col4));
		sprintf(col1, "%-17s", file_types[i]);
		col2 = stats.file_type_stat[i];
		col3 = (double)(100 * col2) / (double)stats.file_category_stat[EROFS_FT_REG_FILE];
		memset(col4, '#', col3 / 2);
		dumpfs_print_chart_row(col1, col2, col3, col4);
	}
}

static void dumpfs_print_statistic_of_compression(void)
{
	stats.compress_rate = (double)(100 * stats.files_total_size) /
		(double)(stats.files_total_origin_size);
	fprintf(stdout, "Filesystem compressed files:            %lu\n",
			stats.compressed_files);
	fprintf(stdout, "Filesystem uncompressed files:          %lu\n",
			stats.uncompressed_files);
	fprintf(stdout, "Filesystem total original file size:    %lu Bytes\n",
			stats.files_total_origin_size);
	fprintf(stdout, "Filesystem total file size:             %lu Bytes\n",
			stats.files_total_size);
	fprintf(stdout, "Filesystem compress rate:               %.2f%%\n",
			stats.compress_rate);
}

static void dumpfs_print_statistic(void)
{
	int err;

	stats.blocks = sbi.blocks;
	err = erofs_read_dir(sbi.root_nid, sbi.root_nid);
	if (err) {
		erofs_err("read dir failed");
		return;
	}

	dumpfs_print_statistic_of_filetype();
	dumpfs_print_statistic_of_compression();

	fprintf(stdout, "\nOriginal file size distribution:\n");
	dumpfs_print_chart_of_file(stats.file_original_size,
			ARRAY_SIZE(stats.file_original_size));
	fprintf(stdout, "\nOn-Disk file size distribution:\n");
	dumpfs_print_chart_of_file(stats.file_comp_size,
			ARRAY_SIZE(stats.file_comp_size));
	fprintf(stdout, "\nFile type distribution:\n");
	dumpfs_print_chart_of_file_type(file_types, OTHERFILETYPE);
}

static void dumpfs_print_superblock(void)
{
	time_t time = sbi.build_time;
	unsigned int features[] = {sbi.feature_compat, sbi.feature_incompat};
	char uuid_str[37] = "not available";
	int i = 0;
	int j = 0;

	fprintf(stdout, "Filesystem magic number:			0x%04X\n", EROFS_SUPER_MAGIC_V1);
	fprintf(stdout, "Filesystem blocks:				%lu\n", sbi.blocks);
	fprintf(stdout, "Filesystem inode metadata start block:		%u\n", sbi.meta_blkaddr);
	fprintf(stdout, "Filesystem shared xattr metadata start block:	%u\n", sbi.xattr_blkaddr);
	fprintf(stdout, "Filesystem root nid:				%ld\n", sbi.root_nid);
	fprintf(stdout, "Filesystem valid inode count:			%lu\n", sbi.inos);
	fprintf(stdout, "Filesystem created:				%s", ctime(&time));
	fprintf(stdout, "Filesystem features:				");
	for (; i < ARRAY_SIZE(features); i++) {
		for (; j < ARRAY_SIZE(feature_lists); j++) {
			if (i == feature_lists[j].compat
				&& (features[i] & feature_lists[j].mask))
				fprintf(stdout, "%s ", feature_lists[j].name);
		}
	}
	fprintf(stdout, "\n");
#ifdef HAVE_LIBUUID
	uuid_unparse_lower(sbi.uuid, uuid_str);
#endif
	fprintf(stdout, "Filesystem UUID:				%s\n", uuid_str);
}

int main(int argc, char **argv)
{
	int err = 0;

	erofs_init_configure();
	err = dumpfs_parse_options_cfg(argc, argv);
	if (err) {
		if (err == -EINVAL)
			usage();
		goto exit;
	}

	err = dev_open_ro(cfg.c_img_path);
	if (err) {
		erofs_err("open image file failed");
		goto exit;
	}

	err = erofs_read_superblock();
	if (err) {
		erofs_err("read superblock failed");
		goto exit;
	}

	if (dumpcfg.print_superblock)
		dumpfs_print_superblock();

	if (dumpcfg.print_statistic)
		dumpfs_print_statistic();

	if (dumpcfg.print_inode)
		dumpfs_print_inode();

	if (dumpcfg.print_inode_phy)
		dumpfs_print_inode_phy();
exit:
	erofs_exit_configure();
	return err;
}
