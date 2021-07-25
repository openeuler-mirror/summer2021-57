#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <libgen.h>
#include <getopt.h>
#include <stdlib.h>
#include <sys/sysmacros.h>

#include "erofs/config.h"
#include "erofs/inode.h"
#include "erofs/print.h"
#include "erofs/io.h"

#define EROFS_SUPER_END (EROFS_SUPER_OFFSET + sizeof(struct erofs_super_block))

extern struct erofs_inode *erofs_new_inode(void);
//dumpfs config
struct dumpcfg {
	bool print_superblock;
	bool print_inode;
	bool print_inode_phy;
	bool print_statistic;
	bool print_version; 

	int ino;
};
static struct dumpcfg dumpcfg;

// statistic info
struct statistics {
	unsigned long blocks;
	unsigned long files;
	unsigned long files_total_size;
	unsigned long files_total_origin_size;
	unsigned long compress_files;
	unsigned long uncompress_files;

	unsigned long regular_files;
	unsigned long dir_files;
	unsigned long chardev_files;
	unsigned long blkdev_files;
	unsigned long fifo_files;
	unsigned long sock_files;
	unsigned long symlink_files;

	unsigned int partial_used_block;
	unsigned long wasted_fragment_bytes;
};
static struct statistics statistics;

static struct option long_options[] = {
	{"help", no_argument, 0, 1},
	{0, 0, 0, 0},
};

// used to get extent info
#define Z_EROFS_LEGACY_MAP_HEADER_SIZE	\
	(sizeof(struct z_erofs_map_header) + Z_EROFS_VLE_LEGACY_HEADER_PADDING)

static unsigned int vle_compressmeta_capacity(erofs_off_t filesize)
{
	const unsigned int indexsize = BLK_ROUND_UP(filesize) *
		sizeof(struct z_erofs_vle_decompressed_index);

	return Z_EROFS_LEGACY_MAP_HEADER_SIZE + indexsize;
}

static int z_erofs_fill_inode_lazy(struct erofs_inode *vi)
{
	int ret;
	erofs_off_t pos;
	struct z_erofs_map_header *h;
	char buf[sizeof(struct z_erofs_map_header)];

	if (vi->flags & EROFS_I_Z_INITED)
		return 0;

	DBG_BUGON(!erofs_sb_has_big_pcluster() &&
		  vi->datalayout == EROFS_INODE_FLAT_COMPRESSION_LEGACY);
	pos = round_up(iloc(vi->nid) + vi->inode_isize + vi->xattr_isize, 8);

	ret = dev_read(buf, pos, sizeof(buf));
	if (ret < 0)
		return -EIO;

	h = (struct z_erofs_map_header *)buf;
	vi->z_advise = le16_to_cpu(h->h_advise);
	vi->z_algorithmtype[0] = h->h_algorithmtype & 15;
	vi->z_algorithmtype[1] = h->h_algorithmtype >> 4;

	if (vi->z_algorithmtype[0] >= Z_EROFS_COMPRESSION_MAX) {
		erofs_err("unknown compression format %u for nid %llu",
			  vi->z_algorithmtype[0], (unsigned long long)vi->nid);
		return -EOPNOTSUPP;
	}

	vi->z_logical_clusterbits = LOG_BLOCK_SIZE + (h->h_clusterbits & 7);
	if (vi->datalayout == EROFS_INODE_FLAT_COMPRESSION &&
	    !(vi->z_advise & Z_EROFS_ADVISE_BIG_PCLUSTER_1) ^
	    !(vi->z_advise & Z_EROFS_ADVISE_BIG_PCLUSTER_2)) {
		erofs_err(
"big pcluster head1/2 of compact indexes should be consistent for nid %llu",
			  vi->nid * 1ULL);
		return -EFSCORRUPTED;
	}
	vi->flags |= EROFS_I_Z_INITED;
	return 0;
}

static void usage(void)
{
	// TODO
	fputs("usage: [options] erofs-image \n"
	" waiting to be done...	\n", stderr);
}

//static void parse_extended_opts(const char *opts)
//{
//	// TODO
//}

static int dumpfs_parse_options_cfg(int argc, char **argv)
{
	int opt, i;
	while((opt = getopt_long(argc, argv, "sSVi:I:", long_options, NULL)) != -1) {
		switch (opt) {
			case 's':
				dumpcfg.print_superblock = true;
				fprintf(stderr, "parse -s \n");
				break;
			case 'S':
				dumpcfg.print_statistic = true;
				fprintf(stderr, "parse -S \n");
				break;
			case 'V':
				dumpcfg.print_version = true;
				fprintf(stderr, "parse -V \n");
				break;
			case 'i':
				// to do
				i = atoi(optarg);
				fprintf(stderr, "parse -i %d\n", i);
				dumpcfg.print_inode = true;
				dumpcfg.ino = i;
				break;
			case 'I':
				i = atoi(optarg);
				fprintf(stderr, "parse -I %d\n", i);
				dumpcfg.print_inode_phy = true;
				dumpcfg.ino = i;
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

static dev_t erofs_new_decode_dev(u32 dev)
{
	const unsigned int major = (dev & 0xfff00) >> 8;
	const unsigned int minor = (dev & 0xff) | ((dev >> 12) & 0xfff00);

	return makedev(major, minor);
}


static int erofs_read_inode_from_disk(struct erofs_inode *vi)
{
	int ret, ifmt;
	char buf[sizeof(struct erofs_inode_extended)];
	struct erofs_inode_compact *dic;
	struct erofs_inode_extended *die;
	const erofs_off_t inode_loc = iloc(vi->nid);

	ret = dev_read(buf, inode_loc, sizeof(*dic));
	if (ret < 0)
		return -EIO;

	dic = (struct erofs_inode_compact *)buf;
	ifmt = le16_to_cpu(dic->i_format);

	vi->datalayout = erofs_inode_datalayout(ifmt);
	if (vi->datalayout >= EROFS_INODE_DATALAYOUT_MAX) {
		erofs_err("unsupported datalayout %u of nid %llu",
			  vi->datalayout, vi->nid | 0ULL);
		return -EOPNOTSUPP;
	}
	switch (erofs_inode_version(ifmt)) {
	case EROFS_INODE_LAYOUT_EXTENDED:
		vi->inode_isize = sizeof(struct erofs_inode_extended);

		ret = dev_read(buf + sizeof(*dic), inode_loc + sizeof(*dic),
			       sizeof(*die) - sizeof(*dic));
		if (ret < 0)
			return -EIO;

		die = (struct erofs_inode_extended *)buf;
		vi->xattr_isize = erofs_xattr_ibody_size(die->i_xattr_icount);
		vi->i_mode = le16_to_cpu(die->i_mode);

		switch (vi->i_mode & S_IFMT) {
		case S_IFREG:
		case S_IFDIR:
		case S_IFLNK:
			vi->u.i_blkaddr = le32_to_cpu(die->i_u.raw_blkaddr);
			break;
		case S_IFCHR:
		case S_IFBLK:
			vi->u.i_rdev =
				erofs_new_decode_dev(le32_to_cpu(die->i_u.rdev));
			break;
		case S_IFIFO:
		case S_IFSOCK:
			vi->u.i_rdev = 0;
			break;
		default:
			goto bogusimode;
		}

		vi->i_uid = le32_to_cpu(die->i_uid);
		vi->i_gid = le32_to_cpu(die->i_gid);
		vi->i_nlink = le32_to_cpu(die->i_nlink);

		vi->i_ctime = le64_to_cpu(die->i_ctime);
		vi->i_ctime_nsec = le64_to_cpu(die->i_ctime_nsec);
		vi->i_size = le64_to_cpu(die->i_size);
		break;
	case EROFS_INODE_LAYOUT_COMPACT:
		vi->inode_isize = sizeof(struct erofs_inode_compact);
		vi->xattr_isize = erofs_xattr_ibody_size(dic->i_xattr_icount);
		vi->i_mode = le16_to_cpu(dic->i_mode);

		switch (vi->i_mode & S_IFMT) {
		case S_IFREG:
		case S_IFDIR:
		case S_IFLNK:
			vi->u.i_blkaddr = le32_to_cpu(dic->i_u.raw_blkaddr);
			break;
		case S_IFCHR:
		case S_IFBLK:
			vi->u.i_rdev =
				erofs_new_decode_dev(le32_to_cpu(dic->i_u.rdev));
			break;
		case S_IFIFO:
		case S_IFSOCK:
			vi->u.i_rdev = 0;
			break;
		default:
			goto bogusimode;
		}

		vi->i_uid = le16_to_cpu(dic->i_uid);
		vi->i_gid = le16_to_cpu(dic->i_gid);
		vi->i_nlink = le16_to_cpu(dic->i_nlink);

		vi->i_ctime = sbi.build_time;
		vi->i_ctime_nsec = sbi.build_time_nsec;

		vi->i_size = le32_to_cpu(dic->i_size);
		break;
	default:
		erofs_err("unsupported on-disk inode version %u of nid %llu",
			  erofs_inode_version(ifmt), vi->nid | 0ULL);
		return -EOPNOTSUPP;
	}

	vi->flags = 0;
	if (erofs_inode_is_data_compressed(vi->datalayout))
		z_erofs_fill_inode(vi);
	return 0;
bogusimode:
	erofs_err("bogus i_mode (%o) @ nid %llu", vi->i_mode, vi->nid | 0ULL);
	return -EFSCORRUPTED;
}

// get compress file actual on-disk size
static unsigned long z_erofs_get_file_size(struct erofs_inode *inode)
{
	int err;
	const erofs_off_t ibase = iloc(inode->nid);
	const erofs_off_t pos = Z_EROFS_VLE_LEGACY_INDEX_ALIGN(ibase + inode->inode_isize + inode->xattr_isize);
	struct z_erofs_vle_decompressed_index *first;
	unsigned int advise, type;
	unsigned long lcn_max;
	void *compressdata;
	unsigned long filesize = 0;
	unsigned int pcluster_size = 4096;
	unsigned int lcluster_size = 4096;

	inode->extent_isize = vle_compressmeta_capacity(inode->i_size);
	compressdata = malloc(inode->extent_isize);
	if (!compressdata) {
		fprintf(stderr, "malloc failed!\n");
		return -ENOMEM;
	}
	lcn_max = BLK_ROUND_UP(inode->i_size);
	
	err = dev_read(compressdata, pos, inode->extent_isize);
	if (err < 0) {
		fprintf(stderr, "read compressmeta failed!\n");
		return -EIO;
	}

	err = z_erofs_fill_inode_lazy(inode);
	if (err) {
		fprintf(stderr, "z_erofs_fill_inode_lazy error occured\n");
		return -1;
	}

	first = (struct z_erofs_vle_decompressed_index *) (compressdata); // + Z_EROFS_LEGACY_MAP_HEADER_SIZE);	
	unsigned long lcn = 0;
	unsigned long last_head_lcn = 0;
	fprintf(stderr, "lcn max: %lu\n", lcn_max);
	while (lcn < lcn_max) {
		struct z_erofs_vle_decompressed_index *di = first + lcn;
		advise = le16_to_cpu(di->di_advise);
		type = (advise >> Z_EROFS_VLE_DI_CLUSTER_TYPE_BIT) &
			((1 << Z_EROFS_VLE_DI_CLUSTER_TYPE_BITS) - 1);
		switch (type) {
		case Z_EROFS_VLE_CLUSTER_TYPE_NONHEAD:
			fprintf(stderr, "nonhead lcn: %lu head lcn: %u\n ", lcn, le16_to_cpu(di->di_u.delta[0]));
			lcn += le16_to_cpu(di->di_u.delta[1]) ? le16_to_cpu(di->di_u.delta[1]) : 1;
			break;
		case Z_EROFS_VLE_CLUSTER_TYPE_PLAIN:
		case Z_EROFS_VLE_CLUSTER_TYPE_HEAD:
			if (le32_to_cpu(di->di_u.blkaddr) == 0) {

			} else {
				last_head_lcn = lcn;
				filesize += pcluster_size;
			}
			fprintf(stderr, "head lcn: %lu, blk addr: %u type: %u\n", lcn, le32_to_cpu(di->di_u.blkaddr), type);
			lcn++;
			break;
		default:
			DBG_BUGON(1);
			return -EOPNOTSUPP;
		}
		
	}	
	fprintf(stderr, "inode compressed blocks : %u\n", inode->u.i_blocks);
	struct z_erofs_vle_decompressed_index *last = first + last_head_lcn;
	advise = le16_to_cpu(last->di_advise);
	type = (advise >> Z_EROFS_VLE_DI_CLUSTER_TYPE_BIT) &&
			((1 << Z_EROFS_VLE_DI_CLUSTER_TYPE_BITS) - 1);
	
	switch (type) {
		case Z_EROFS_VLE_CLUSTER_TYPE_PLAIN:
			filesize -= pcluster_size - (inode->i_size - last_head_lcn * lcluster_size - le16_to_cpu(last->di_clusterofs));
			break;
		case Z_EROFS_VLE_CLUSTER_TYPE_HEAD:
			// erofs_off_t offset = blknr_to_addr(last->di_u.blkaddr);
			// int last_cluster_size = pread64(erofs_devfd, 
			break;
		default:
			return -ENOTSUP;
	}
	return filesize;
}

static unsigned long erofs_get_file_actual_size(struct erofs_inode *inode)
{
	switch (inode->datalayout) {
		case EROFS_INODE_FLAT_INLINE:
		case EROFS_INODE_FLAT_PLAIN:
			statistics.uncompress_files++;
			return inode->i_size;
		case EROFS_INODE_FLAT_COMPRESSION_LEGACY:
		case EROFS_INODE_FLAT_COMPRESSION:
			statistics.compress_files++;
			return z_erofs_get_file_size(inode);
	}
	return -EINVAL;
}


static void dumpfs_print_version()
{
	// TODO
	fprintf(stderr, "TODO\n");
	fprintf(stderr, "VERSION INFO\n");
}

static void dumpfs_print_superblock()
{
	fprintf(stderr, "Filesystem magic number:	0x%04X\n", EROFS_SUPER_MAGIC_V1);
	fprintf(stderr, "Filesystem blocks: 		%lu\n", sbi.blocks);
	fprintf(stderr, "Filesystem meta address:	0x%04X\n", sbi.meta_blkaddr);
	fprintf(stderr, "Filesystem xattr address:	0x%04X\n", sbi.xattr_blkaddr);
	fprintf(stderr, "Filesystem root nid:		%ld\n", sbi.root_nid);
	fprintf(stderr, "Filesystem inodes count:	%ld\n", sbi.inos);

	//fprintf(stderr, "Filesystem created:		%s", ctime(sbi.build_time));

}

static void dumpfs_print_inode()
{

}

static void dumpfs_print_inode_phy()
{

}
// file num、file size、file type
static int read_dir(erofs_nid_t nid, erofs_nid_t parent_nid) 
{
	struct erofs_inode vi = { .nid = nid};
	int ret;
	char buf[EROFS_BLKSIZ];
	erofs_off_t offset;
	
	fprintf(stderr, "read_dir: %lu\n", nid);

	ret = erofs_read_inode_from_disk(&vi);
	if (ret)
		return ret;

	offset = 0;
	while (offset < vi.i_size) {
		erofs_off_t maxsize = min_t(erofs_off_t,
					vi.i_size - offset, EROFS_BLKSIZ);
		struct erofs_dirent *de = (void *)buf;
		struct erofs_dirent *end;
		unsigned int nameoff;

		ret = erofs_pread(&vi, buf, maxsize, offset);
		if (ret)
			return ret;

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

			/* the last dirent in the block? */
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
			statistics.files++;

			unsigned long actual_size = 0;
			switch (de->file_type) {
			
			case EROFS_FT_UNKNOWN:
				break;	

			case EROFS_FT_REG_FILE:
				
				ret = erofs_read_inode_from_disk(&inode);
				if (ret) {
					fprintf(stderr, "read reg file inode failed!\n");
					return 1;
				}
				statistics.files_total_origin_size += inode.i_size;
				statistics.regular_files++;
				actual_size = erofs_get_file_actual_size(&inode);
				if (actual_size < 0) {
					fprintf(stderr, "error occured getting actual size\n");
					return -EIO;
				}
				statistics.files_total_size += actual_size;
				break;	

			case EROFS_FT_DIR:
				statistics.dir_files++;
				if (de->nid != nid && de->nid != parent_nid) {
					ret = read_dir(de->nid, nid);
					if (ret) {
						fprintf(stderr, "parse dir nid%llu error occurred\n", de->nid);
						return 1;
					}
				}
				break;	

			case EROFS_FT_CHRDEV:
				statistics.chardev_files++;
				break;	

			case EROFS_FT_BLKDEV:
				statistics.blkdev_files++;
				break;	

			case EROFS_FT_FIFO:
				statistics.fifo_files++;
				break;	

			case EROFS_FT_SOCK:
				statistics.sock_files++;
				break;	

			case EROFS_FT_SYMLINK:
				statistics.symlink_files++;
				break;

			}

			++de;
		}
		offset += maxsize;
	}
	return 0;
	
}
static void dumpfs_print_statistic()
{
	struct erofs_inode *root_inode;
	int err;

	root_inode = erofs_new_inode();
	err = erofs_ilookup("/", root_inode);
	if (err) {
		fprintf(stderr, "look for root inode failed!");
		return;
	}
	
	//blocks info
	statistics.blocks = sbi.blocks;
	
	err = read_dir(sbi.root_nid, sbi.root_nid);
	if (err) {
		fprintf(stderr, "read root dir failed!");
		return;
	}
	// file type count
	fprintf(stderr, "Filesystem Files:		%lu\n", statistics.files);
	fprintf(stderr, "Filesystem Regular Files:	%lu\n", statistics.regular_files);
	fprintf(stderr, "Filesystem Dir Files:		%lu\n", statistics.dir_files);
	fprintf(stderr, "Filesystem CharDev Files:	%lu\n", statistics.chardev_files);
	fprintf(stderr, "Filesystem BlkDev Files:	%lu\n", statistics.blkdev_files);
	fprintf(stderr, "Filesystem FIFO Files:		%lu\n", statistics.fifo_files);
	fprintf(stderr, "Filesystem SOCK Files:		%lu\n", statistics.sock_files);
	fprintf(stderr, "Filesystem Link Files:		%lu\n", statistics.symlink_files);

	fprintf(stderr, "Filesystem Compressed Files:	%lu\n", statistics.compress_files);
	fprintf(stderr, "Filesystem Uncompressed Files:	%lu\n", statistics.uncompress_files);


	fprintf(stderr, "Filsystem total original file size:	%lu Bytes\n", statistics.files_total_origin_size);
	fprintf(stderr, "Filesystem total file size:	%lu Bytes\n", statistics.files_total_size);

	return;
}

int main(int argc, char** argv) 
{
	int err = 0;
	struct erofs_inode *root_inode;
	// init config
	erofs_init_configure();

	// print version info
	fprintf(stderr, "%s %s\n", basename(argv[0]), cfg.c_version);
	err = dumpfs_parse_options_cfg(argc, argv);	
	if (err) {
		if (err == -EINVAL)
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

	if (dumpcfg.print_version) {
		dumpfs_print_version();
	}

	if (dumpcfg.print_superblock) {
		dumpfs_print_superblock();
	}

	root_inode = erofs_new_inode();
	err = erofs_ilookup("/", root_inode);	
	if (err) {
		fprintf(stderr, "failed to look up root inode");
		return 1;
	}

	fprintf(stderr, "root inode nid:	%lu\n", root_inode->nid);
	fprintf(stderr, "root inode size:	%lu\n", root_inode->i_size);

	if (dumpcfg.print_inode)
		dumpfs_print_inode();
	else if (dumpcfg.print_inode_phy)
		dumpfs_print_inode_phy();
	
	if (dumpcfg.print_statistic)
		dumpfs_print_statistic();

	return 0;
}