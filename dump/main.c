#include <sys/stat.h>
#include <libgen.h>
#include <getopt.h>

#include "erofs/config.h"
#include "erofs/inode.h"
#include "erofs/print.h"
#include "erofs/io.h"


#define EROFS_SUPER_END (EROFS_SUPER_OFFSET + sizeof(struct erofs_super_block))

//dumpfs config
struct dumpcfg {


};
static struct dumpcfg dumpcfg;

static struct option long_options[] = {
	{"help", no_argument, 0, 1},
	{0, 0, 0, 0},
};

static void usage(void)
{
	// TODO
	fputs("usage: [options] erofs-image \n"
	" waiting to be done...	\n", stderr);
}

static parse_extended_opts(const char *opts)
{
	// TODO
}

static dumpfs_parse_options_cfg(int argc, char **argv)
{
	char *endptr;
	int opt, i;
	while((opt = getopt_long(argc, argv, "sSVi:I:", long_options, NULL)) != -1) {
		switch (opt) {
			case 's':
				break;
			case 'S':
				break;
			case 'V':
				if (opt)
					return opt;
				break;
			case 'i':
				// to do
				i = atoi(optarg);
				break;
			case 'I':
				i = atoi(optarg);
				break;

			case 1:
				// long option --help 
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
		erofs_err("Unexpected argument: %s\n", argv[optind]);
		return -EINVAL;
	}
	return 0;
}
static void dumpfs_print_super_block()
{
	fprintf(stderr, "Filesystem UUID:		%s\n", sbi.uuid);
	fprintf(stderr, "Filesystem magic number:	0x%04X\n", EROFS_SUPER_MAGIC_V1);
	fprintf(stderr, "Filesystem blocks: 		%llu\n", sbi.blocks);
	fprintf(stderr, "Filesystem meta address:	0x%04X\n", sbi.meta_blkaddr);
	fprintf(stderr, "Filesystem xattr address:	0x%04X\n", sbi.xattr_blkaddr);
	fprintf(stderr, "Filesystem root nid:		%lld\n", sbi.root_nid);
	fprintf(stderr, "Filesystem inodes count:	%lld", sbi.inos);

	fprintf(stderr, "Filesystem created:		%s", ctime(sbi.build_time));

}

static void dumpfs_statistic()
{

}

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
	root_inode = erofs_new_inode();
	err = erofs_ilookup("/", root_inode);
	if (err) {
		fprintf(stderr, "failed to look up root inode");
		return 1;
	}
	
	return 0;
}