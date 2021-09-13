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
	bool print_version;
};
static struct dumpcfg dumpcfg;

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
		"-s          print information about superblock\n"
		"-V      print the version number of dump.erofs and exit.\n",
		stderr);
}
static void dumpfs_print_version(void)
{
	fprintf(stderr, "dump.erofs %s\n", cfg.c_version);
}

static int dumpfs_parse_options_cfg(int argc, char **argv)
{
	int opt;

	while ((opt = getopt_long(argc, argv, "sV",
					long_options, NULL)) != -1) {
		switch (opt) {
		case 's':
			dumpcfg.print_superblock = true;
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
		goto out;
	}

	err = dev_open_ro(cfg.c_img_path);
	if (err) {
		erofs_err("open image file failed");
		goto out;
	}

	err = erofs_read_superblock();
	if (err) {
		erofs_err("read superblock failed");
		goto out;
	}

	if (dumpcfg.print_superblock)
		dumpfs_print_superblock();
out:
	if (cfg.c_img_path)
		free(cfg.c_img_path);
	return err;
}
