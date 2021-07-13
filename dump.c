#include <sys/stat.h>
#include <libgen.h>
#include <getopt.h>

#include "erofs/config.h"
#include "erofs/inode.h"
#include "erofs/print.h"
#include "erofs/io.h"


#define EROFS_SUPER_END (EROFS_SUPER_OFFSET + sizeof(struct erofs_super_block))

static struct option long_options[] = {
	{"help", no_argument, 0, 1},
	{0, 0, 0, 0},
};
static void usage(void)
{
	fputs("usage: [options] erofs-image \n"
	" waiting to be done...	\n", stderr);
}
static parse_extended_opts(const char *opts)
{

}
static dumpfs_parse_options_cfg(int argc, char **argv)
{
	char *endptr;
	int opt, i;
	while((opt = getopt_long(argc, argv, "d:x:z:E:T:U:", long_options, NULL)) != -1) {
		switch (opt) {
			case 'd':
				i = atoi(optarg);
				if (i < EROFS_MSG_MIN || i > EROFS_MSG_MAX) {
					erofs_err("invalid debug level %d", i);
					return -EINVAL;
				}
				cfg.c_dbg_lvl = i;
				break;

			case 'V':
				opt = parse_extended_opts(optarg);
				if (opt)
					return opt;
				break;
			case 'S':
				cfg.c_unix_timestamp = strtoull(optarg, &endptr, 0);
				if (cfg.c_unix_timestamp == -1 || *endptr != '\0') {
					erofs_err("invalid UNIX timestamp %s", optarg);
					return -EINVAL;
				}
				cfg.c_timeinherit = TIMESTAMP_FIXED;
				break;
			case 'M':
				// to do
				break;

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

	if (optind >= argc) {
		erofs_err("Source directory is missing");
		return -EINVAL;
	}

	if (optind < argc) {
		erofs_err("Unexpected argument: %s\n", argv[optind]);
		return -EINVAL;
	}
	return 0;
}
static struct options {
	// to do
} dumpcfg;

int main(int argc, char** argv) 
{
	int err = 0;
	struct erofs_buffer_head *sb_bh;
	struct erofs_inode *root_inode;
	erofs_nid_t root_nid;

	// init config
	erofs_init_configure();

	// print version info
	fprintf(stderr, "%s %s", basename(argv[0]), cfg.c_version);
	err = dumpfs_parse_options_cfg(argc, argv);	
	if (err) {
		if (err = -EINVAL)
			usage();
		return 1;
	}

	erofs_show_config();

	// open image
	err = dev_open_ro(cfg.c_img_path);
	if (err) {
		fprintf(stderr, "failed to open: %s\n", cfg.c_img_path);
		return 1;
	}

	err = erofs_read_superblock();
	if (err) {
		fprintf(stderr, "failed to read erofs super block\n");
		return 1;
	}	
	erofs_inode_manager_init();

	root_nid = sbi.root_nid;
	err = erofs_ilookup("/", root_inode);
	if (err) {
		fprintf(stderr, "failed to look up root inode");
		return 1;
	}
}