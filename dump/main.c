// SPDX-License-Identifier: GPL-2.0+
/*
* mkfs/main.c
*
* Copyright (C) 2021-2022 HUAWEI, Inc.
*             http://www.huawei.com/
* Created by Wang Qi <mpiglet@outlook.com>
*/

#include <stdlib.h>
#include <getopt.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <lz4.h>

#include "erofs/print.h"
#include "erofs/io.h"

struct dumpcfg {
	bool print_superblock;
	bool print_statistic;
	bool print_version; 
};
static struct dumpcfg dumpcfg;

static char chart_format[] = "%-16s	%-11d %8.2f%% |%-50s|\n";
static char header_format[] = "%-16s %11s %16s |%-50s|\n";
static char *file_types[] = {
	".so",
	".png",
	".jpg",
	".xml",
	".html",
	".odex",
	".vdex",
	".apk",
	".ttf",
	".jar",
	".json",
	".ogg",
	".oat",
	".art",
	".rc",
	".otf",
	".txt",
	"others",
};
enum {
	SOFILETYPE = 0,
	PNGFILETYPE,
	JPEGFILETYPE,
	XMLFILETYPE,
	HTMLFILETYPE,
	ODEXFILETYPE,
	VDEXFILETYPE,
	APKFILETYPE,
	TTFFILETYPE,
	JARFILETYPE,
	JSONFILETYPE,
	OGGFILETYPE,
	OATFILETYPE,
	ARTFILETYPE,
	RCFILETYPE,
	OTFFILETYPE,
	TXTFILETYPE,
	OTHERFILETYPE,
};

#define	FILE_SIZE_BITS	30
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

	unsigned int file_count_categorized_by_postfix[OTHERFILETYPE + 1];
	unsigned int file_original_size_counts[FILE_SIZE_BITS];
	unsigned int file_actual_size_counts[FILE_SIZE_BITS];
};
static struct statistics statistics;

static struct option long_options[] = {
	{"help", no_argument, 0, 1},
	{0, 0, 0, 0},
};

static void usage(void)
{
	fputs("usage: [options] erofs-image \n\n"
	"Dump erofs layout from erofs-image, and [options] are:\n"
		"	-s		print information about superblock\n"
		"	-S		print statistic information of the erofs-image\n"
		"	-v/-V		print dump.erofs version info\n"
		"	-h/--help	display this help and exit\n", stderr);
}
static void dumpfs_print_version()
{
	fprintf(stderr, "dump.erofs %s\n", cfg.c_version);
}

static int dumpfs_parse_options_cfg(int argc, char **argv)
{
	int opt;
	while((opt = getopt_long(argc, argv, "sSvVi:I:h", long_options, NULL)) != -1) {
		switch (opt) {
			case 's':
				dumpcfg.print_superblock = true;
				break;
			case 'S':
				dumpcfg.print_statistic = true;
				break;
			case 'v':
			case 'V':
				dumpfs_print_version();
				exit(0);
			case 'h':
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

static int z_erofs_get_last_cluster_size_from_disk(struct erofs_map_blocks *map, erofs_off_t last_cluster_size, erofs_off_t *last_cluster_compressed_size)
{
	int ret;
	int decompressed_len;
	int compressed_len = 0;
	char *decompress;
	char raw[Z_EROFS_PCLUSTER_MAX_SIZE] = {0};

	ret = dev_read(raw, map->m_pa, map->m_plen);
	if (ret < 0) {
		return -EIO;
	}

	if (erofs_sb_has_lz4_0padding()) {
		compressed_len = map->m_plen;
	} else {
		// lz4 maximum compression ratio is 255
		decompress = (char *)malloc(map->m_plen * 255);
		if (!decompress) {
			erofs_err("allocate memory for decompress space failed");
			return -1;
		}
		decompressed_len = LZ4_decompress_safe_partial(raw, decompress, map->m_plen, last_cluster_size, map->m_plen * 10);
		if (decompressed_len < 0) {
			erofs_err("decompress last cluster to get decompressed size failed");
			free(decompress);
			return -1;
		}
		compressed_len = LZ4_compress_destSize(decompress, raw, &decompressed_len, Z_EROFS_PCLUSTER_MAX_SIZE);
		if (compressed_len < 0) {
			erofs_err("compress to get last extent size failed\n");
			free(decompress);
			return -1;
		}
		free(decompress);
		// dut to the use of lz4hc (can use different compress level), our normal lz4 compress result may be bigger
		compressed_len = compressed_len < map->m_plen ? compressed_len : map->m_plen;
	}
	
	*last_cluster_compressed_size = compressed_len;
	return 0;
}

static int z_erofs_get_compressed_size(struct erofs_inode *inode, erofs_off_t *size)
{
	int err;
	erofs_blk_t compressedlcs;
	erofs_off_t last_cluster_size;
	erofs_off_t last_cluster_compressed_size;
	struct erofs_map_blocks map = {
		.index = UINT_MAX,
		.m_la = inode->i_size - 1,
	};

	err = z_erofs_map_blocks_iter(inode, &map);
	if (err) {
		erofs_err("read nid %ld's last block failed\n", inode->nid);
		return err;
	}
	compressedlcs = map.m_plen >> inode->z_logical_clusterbits;
	*size = (inode->u.i_blocks - compressedlcs) * EROFS_BLKSIZ;
	last_cluster_size = inode->i_size - map.m_la;

	if (!(map.m_flags & EROFS_MAP_ZIPPED)) {
			*size += last_cluster_size;
	}
	else {
		err = z_erofs_get_last_cluster_size_from_disk(&map, last_cluster_size, &last_cluster_compressed_size);
		if (err) {
			erofs_err("get nid %ld's last extent size failed", inode->nid);
			return err;
		}
		*size += last_cluster_compressed_size;
	}
	return 0;
}

static int erofs_get_file_actual_size(struct erofs_inode *inode, erofs_off_t *size)
{
	int err;
	switch (inode->datalayout) {
		case EROFS_INODE_FLAT_INLINE:
		case EROFS_INODE_FLAT_PLAIN:
			statistics.uncompressed_files++;
			*size = inode->i_size;
			break;
		case EROFS_INODE_FLAT_COMPRESSION_LEGACY:
		case EROFS_INODE_FLAT_COMPRESSION:
			statistics.compressed_files++;
			err = z_erofs_get_compressed_size(inode, size);
			if (err) {
				erofs_err("get compressed file size failed\n");
				return err;
			}
	}
	return 0;
}

static void dumpfs_print_superblock()
{
	fprintf(stderr, "Filesystem magic number:	0x%04X\n", EROFS_SUPER_MAGIC_V1);
	fprintf(stderr, "Filesystem blocks: 		%lu\n", sbi.blocks);
	fprintf(stderr, "Filesystem meta block:		%u\n", sbi.meta_blkaddr);
	fprintf(stderr, "Filesystem xattr block:	%u\n", sbi.xattr_blkaddr);
	fprintf(stderr, "Filesystem root nid:		%ld\n", sbi.root_nid);
	fprintf(stderr, "Filesystem valid inos:		%lu\n", sbi.inos);

	time_t time = sbi.build_time;
	fprintf(stderr, "Filesystem created:		%s", ctime(&time));

	fprintf(stderr, "Filesystem uuid:		");
	for (int i = 0; i < 16; i++)
		fprintf(stderr, "%02x", sbi.uuid[i]);
	fprintf(stderr, "\n");

	if (erofs_sb_has_lz4_0padding())
		fprintf(stderr, "Filesystem support lz4 0padding\n");
	else
		fprintf(stderr, "Filesystem not support lz4 0padding\n");

	if (erofs_sb_has_big_pcluster())
		fprintf(stderr, "Filesystem support big pcluster\n");
	else
		fprintf(stderr, "Filesystem not support big pcluster\n");

	if (erofs_sb_has_sb_chksum())
		fprintf(stderr, "Filesystem has super block checksum feature\n");
	else
		fprintf(stderr, "Filesystem doesn't have super block checksum feature\n");

}

static unsigned check_file_category_by_postfix(const char *filename) {
	
	char *postfix = strrchr(filename, '.');
	int type = SOFILETYPE;
	if (postfix == NULL)
		return OTHERFILETYPE;
	while (type < OTHERFILETYPE) {
		if (strcmp(postfix, file_types[type]) == 0)
			break;
		type ++;
	}
	return type;
}

// file count、file size、file type
static int read_dir(erofs_nid_t nid, erofs_nid_t parent_nid) 
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
			const char *de_name;
			unsigned int de_namelen;
			struct erofs_inode inode = { .nid = de->nid };
			nameoff = le16_to_cpu(de->nameoff);
			de_name = (char *)buf + nameoff;

			if (de + 1 >= end)
				de_namelen = strnlen(de_name, maxsize - nameoff);
			else
				de_namelen = le16_to_cpu(de[1].nameoff) - nameoff;
			
			/* a corrupted entry is found */
			if (nameoff + de_namelen > maxsize ||
				de_namelen > EROFS_NAME_LEN) {
				erofs_err("bogus dirent @ nid %llu", le64_to_cpu(de->nid) | 0ULL);
				DBG_BUGON(1);
				return -EFSCORRUPTED;
			}
			if (de->nid != nid && de->nid != parent_nid)
				statistics.files++;

			erofs_off_t actual_size = 0;
			erofs_off_t original_size;
			int actual_size_mark, original_size_mark;
			memset(filename, 0, PATH_MAX + 1);
			memcpy(filename, de_name, de_namelen);

			switch (de->file_type) {
			case EROFS_FT_UNKNOWN:
				break;	
			case EROFS_FT_REG_FILE:
				err = erofs_read_inode_from_disk(&inode);
				if (err) {
					fprintf(stderr, "read reg file inode failed!\n");
					erofs_err("read file inode from disk failed!");
					return err;
				}
				original_size = inode.i_size;
				statistics.files_total_origin_size += original_size;
				statistics.regular_files++;
				err = erofs_get_file_actual_size(&inode, &actual_size);
				if (err) {
					erofs_err("get file size failed\n");
					return err;
				}
				statistics.files_total_size += actual_size;
				statistics.file_count_categorized_by_postfix[check_file_category_by_postfix(filename)]++;

				original_size_mark = 0;
				actual_size_mark = 0;
				actual_size >>= 10;
				original_size >>= 10;
				while (actual_size || original_size) {
					if (actual_size) {
						actual_size >>= 1;
						actual_size_mark++;
					}
					if (original_size) {
						original_size >>= 1;
						original_size_mark++;
					}
				}

				if (original_size_mark >= FILE_SIZE_BITS - 1)
					statistics.file_original_size_counts[FILE_SIZE_BITS - 1]++;
				else
					statistics.file_original_size_counts[original_size_mark]++;
				if (actual_size_mark >= FILE_SIZE_BITS - 1)
					statistics.file_actual_size_counts[FILE_SIZE_BITS - 1]++;
				else
					statistics.file_actual_size_counts[actual_size_mark]++;
				break;	

			case EROFS_FT_DIR:
				if (de->nid != nid && de->nid != parent_nid) {	
					statistics.dir_files++;
					statistics.uncompressed_files++;
					err = read_dir(de->nid, nid);
					if (err) {
						fprintf(stderr, "parse dir nid %llu error occurred\n", de->nid);
						return err;
					}
				}
				break;	
			case EROFS_FT_CHRDEV:
				statistics.chardev_files++;
				statistics.uncompressed_files++;
				break;	
			case EROFS_FT_BLKDEV:
				statistics.blkdev_files++;
				statistics.uncompressed_files++;
				break;	
			case EROFS_FT_FIFO:
				statistics.fifo_files++;
				statistics.uncompressed_files++;
				break;	
			case EROFS_FT_SOCK:
				statistics.sock_files++;
				statistics.uncompressed_files++;
				break;	
			case EROFS_FT_SYMLINK:
				statistics.symlink_files++;
				statistics.uncompressed_files++;
				break;
			}
			++de;
		}
		offset += maxsize;
	}
	return 0;
}

static void dumpfs_print_statistic_of_filetype()
{
	fprintf(stderr, "Filesystem total file count:         %lu\n", statistics.files);
	fprintf(stderr, "Filesystem regular file count:       %lu\n", statistics.regular_files);
	fprintf(stderr, "Filesystem directory count:          %lu\n", statistics.dir_files);
	fprintf(stderr, "Filesystem symlink file count:       %lu\n", statistics.symlink_files);
	fprintf(stderr, "Filesystem character device count:   %lu\n", statistics.chardev_files);
	fprintf(stderr, "Filesystem block device count:       %lu\n", statistics.blkdev_files);
	fprintf(stderr, "Filesystem FIFO file count:          %lu\n", statistics.fifo_files);
	fprintf(stderr, "Filesystem SOCK file count:          %lu\n", statistics.sock_files);
}
static void dumpfs_print_chart_row(char *col1, unsigned col2, double col3, char *col4)
{
	char row[500] = {0};
	sprintf(row, chart_format, col1, col2, col3, col4);
	fprintf(stderr, row);
	return;
}

static void dumpfs_print_chart_of_file(unsigned *file_counts, unsigned len)
{
	char col1[30];
	unsigned col2;
	double col3;
	char col4[400];
	unsigned lowerbound = 0, upperbound = 1;
	fprintf(stderr, header_format, ">=(KB) .. <(KB) ", "count",	"ratio", "distribution");
	for (int i = 0; i < len; i++) {
		memset(col1, 0, 30);
		memset(col4, 0, 400);
		if (i == len - 1)
			strcpy(col1, " others");
		else if (i <= 6)
			sprintf(col1, "%6d .. %-6d", lowerbound, upperbound);
		else

			sprintf(col1, "%6d .. %-6d", lowerbound, upperbound);
		col2 = file_counts[i];
		col3 = (double)(100 * col2) / (double)statistics.regular_files;
		memset(col4, '#', col3 / 2);
		dumpfs_print_chart_row(col1, col2, col3, col4);
		lowerbound = upperbound;
		upperbound <<= 1;
	}
}

static void dumpfs_print_chart_of_file_type(char **file_types, unsigned len)
{
	char col1[30];
	unsigned col2;
	double col3;
	char col4[401];

	fprintf(stderr, header_format, "type", "count",	"ratio", "distribution");
	for (int i = 0; i < len; i++) {
		memset(col1, 0, 30);
		memset(col4, 0, 401);
		sprintf(col1, "%-17s", file_types[i]);
		col2 = statistics.file_count_categorized_by_postfix[i];
		col3 = (double)(100 * col2) / (double)statistics.regular_files;
		memset(col4, '#', col3 / 2);
		dumpfs_print_chart_row(col1, col2, col3, col4);
	}
}

static void dumpfs_print_statistic_of_compression()
{
	statistics.compress_rate = (double)(100 * statistics.files_total_size) / (double)(statistics.files_total_origin_size);
	fprintf(stderr, "Filesystem compressed files:         %lu\n", statistics.compressed_files);
	fprintf(stderr, "Filesystem uncompressed files:       %lu\n", statistics.uncompressed_files);
	fprintf(stderr, "Filesystem total original file size: %lu Bytes\n", statistics.files_total_origin_size);
	fprintf(stderr, "Filesystem total file size:          %lu Bytes\n", statistics.files_total_size);
	fprintf(stderr, "Filesystem compress rate:            %.2f%%\n", statistics.compress_rate);
}

static void dumpfs_print_statistic()
{
	int err;

	statistics.blocks = sbi.blocks;
	err = read_dir(sbi.root_nid, sbi.root_nid);
	if (err) {
		erofs_err("read dir failed");
		return;
	}

	dumpfs_print_statistic_of_filetype();
	dumpfs_print_statistic_of_compression();

	fprintf(stderr, "\nOriginal file size distribution:\n");
	dumpfs_print_chart_of_file(statistics.file_original_size_counts, 17);
	fprintf(stderr, "\nOn-Disk file size distribution:\n");
	dumpfs_print_chart_of_file(statistics.file_actual_size_counts, 17);
	fprintf(stderr, "\nFile type distribution:\n");
	dumpfs_print_chart_of_file_type(file_types, OTHERFILETYPE + 1);
	return;
}

int main(int argc, char** argv) 
{
	int err = 0;
	erofs_init_configure();

	err = dumpfs_parse_options_cfg(argc, argv);	
	if (err) {
		if (err == -EINVAL)
			usage();
		return -1;
	}

	err = dev_open_ro(cfg.c_img_path);
	if (err) {
		erofs_err("open image file failed");
		return -1;
	}

	err = erofs_read_superblock();
	if (err) {
		erofs_err("read superblock failed");
		return -1;
	}	

	if (dumpcfg.print_superblock) {
		dumpfs_print_superblock();
	}

	if (dumpcfg.print_statistic)
		dumpfs_print_statistic();

	return 0;
}
