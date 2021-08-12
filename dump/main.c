// SPDX-License-Identifier: GPL-2.0+
/*
 * dump/main.c
 *
 * Copyright (C) 2021-2022 HUAWEI, Inc.
 *             http://www.huawei.com/
 * Created by Wang Qi <mpiglet@outlook.com>
 *            Guo Xuenan <guoxuenan@huawei.com>
 */

#include <stdlib.h>
#include <getopt.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <lz4.h>

#include "erofs/print.h"
#include "erofs/io.h"

static struct option long_options[] = {
	{"help", no_argument, 0, 1},
	{0, 0, 0, 0},
};

static void usage(void)
{
	fputs("usage: [options] erofs-image \n\n"
		"Dump erofs layout from erofs-image, and [options] are:\n"
		"-v/-V      print dump.erofs version info\n"
		"-h/--help  display this help and exit\n", stderr);
}
static void dumpfs_print_version(void)
{
	fprintf(stderr, "dump.erofs %s\n", cfg.c_version);
}

static int dumpfs_parse_options_cfg(int argc, char **argv)
{
	int opt;

	while ((opt = getopt_long(argc, argv, "sSvVi:I:h",
					long_options, NULL)) != -1) {
		switch (opt) {
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

int main(int argc, char **argv)
{
	int err = 0;

	erofs_init_configure();
	err = dumpfs_parse_options_cfg(argc, argv);
	if (err) {
		if (err == -EINVAL)
			usage();
		return -1;
	}

	return 0;
}
