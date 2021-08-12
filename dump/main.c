#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <libgen.h>
#include <getopt.h>
#include <stdlib.h>
#include <sys/sysmacros.h>
#include <lz4.h>

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

	u64 ino;
	u64 nid;
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
	fputs("usage: [options] erofs-image \n\n"
	"Dump erofs layout from erofs-image, and [options] are:\n"
	      " -s            print information about superblock\n"
	      " -S                print statistic information of the erofs-image\n"
	      " -i #                print target # inode info\n"
	      " -I #          print target # inode on-disk info\n"
	      " -v/-V                print dump.erofs version info\n"
	      " -h/--help             display this help and exit\n", stderr);
}
static void dumpfs_print_version()
{
	// TODO
	// fprintf(stderr, "VERSION INFO\n");
	// print version info
	fprintf(stderr, "dump.erofs %s\n", cfg.c_version);
}
//static void parse_extended_opts(const char *opts)
//{
//	// TODO
//}

static int dumpfs_parse_options_cfg(int argc, char **argv)
{
	int opt;
	u64 i;
	while((opt = getopt_long(argc, argv, "sSvVi:I:h", long_options, NULL)) != -1) {
		switch (opt) {
			case 's':
				dumpcfg.print_superblock = true;
				// fprintf(stderr, "parse -s \n");
				break;
			case 'S':
				dumpcfg.print_statistic = true;
				// fprintf(stderr, "parse -S \n");
				break;
			case 'v':
			case 'V':
				// dumpcfg.print_version = true;
				// fprintf(stderr, "parse -V \n");
				dumpfs_print_version();
				exit(0);
				break;
			case 'i':
				// to do
				i = atoll(optarg);
				// fprintf(stderr, "parse -i %lu\n", i);
				dumpcfg.print_inode = true;
				dumpcfg.ino = i;
				break;
			case 'I':
				i = atoll(optarg);
				// fprintf(stderr, "parse -I %lu\n", i);
				dumpcfg.print_inode_phy = true;
				dumpcfg.ino = i;
				break;
			case 'h':
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
	vi->i_ino[0] = le32_to_cpu(dic->i_ino);
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



static unsigned long z_erofs_get_last_cluster_size_from_disk(struct z_erofs_vle_decompressed_index *last, int last_original_size)
{

	int ret;
	char raw[EROFS_BLKSIZ] = {0};
	char decompress[EROFS_BLKSIZ * 1000] = {0};
	//read last extent buffer, decompress and compress to get size

	ret = dev_read(raw, blknr_to_addr(last->di_u.blkaddr), EROFS_BLKSIZ);
	if (ret < 0) {
		fprintf(stderr, "dev_read error!\n");
		return -EIO;
	}

	ret = LZ4_decompress_safe_partial(raw, decompress, EROFS_BLKSIZ, last_original_size, EROFS_BLKSIZ * 1000);
	if (ret < 0) {
		fprintf(stderr, "LZ4 decompress safe error ret: %d\n", ret);
		return -EIO;
	}

	int source_size = EROFS_BLKSIZ;
	ret = LZ4_compress_destSize(decompress, raw, &source_size, EROFS_BLKSIZ * 1000);
	if (ret <= 0) {
		fprintf(stderr, "LZ4 compress destsize error ret: %d\n", ret);
		return -EIO;
	}
	return ret;
}

// get compress file actual on-disk size}
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
	erofs_off_t compressed_blocks = 0;

	compressed_blocks = inode->u.i_blocks;
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
	unsigned long block = 0;

	//fprintf(stderr, "erofs compressed blocks: %lu \n", compressed_blocks);
	fprintf(stderr, "inode number: %lu	nid: %lu	lcn max: %lu	original size: %lu\n", inode->i_ino[0], inode->nid, lcn_max, inode->i_size);
	filesize = (compressed_blocks - 1) * pcluster_size;
	while (lcn < lcn_max) {//&& block < compressed_blocks) {
		struct z_erofs_vle_decompressed_index *di = first + lcn;
		advise = le16_to_cpu(di->di_advise);
		type = (advise >> Z_EROFS_VLE_DI_CLUSTER_TYPE_BIT) &
			((1 << Z_EROFS_VLE_DI_CLUSTER_TYPE_BITS) - 1);
		switch (type) {
		case Z_EROFS_VLE_CLUSTER_TYPE_NONHEAD:
			// fprintf(stderr, "nonhead lcn: %lu head lcn: %u\n", lcn, le16_to_cpu(di->di_u.delta[0]));
			lcn += le16_to_cpu(di->di_u.delta[1]) ? le16_to_cpu(di->di_u.delta[1]) : 1;
			break;
		case Z_EROFS_VLE_CLUSTER_TYPE_PLAIN:
		case Z_EROFS_VLE_CLUSTER_TYPE_HEAD:
			if (block == compressed_blocks - 1) {
				last_head_lcn = lcn;
			}
			// if (le32_to_cpu(di->di_u.blkaddr) == 0) {

			// } else {
			// 	last_head_lcn = lcn;
			// 	filesize += pcluster_size;
			// }
			// fprintf(stderr, "head lcn: %lu, blk addr: %u offset: %u type: %u\n", lcn,
			//	le32_to_cpu(di->di_u.blkaddr), le16_to_cpu(di->di_clusterofs), type);
			lcn++;
			block++;

			break;
		default:
			DBG_BUGON(1);
			return -EOPNOTSUPP;
		}
		
	}	
	// fprintf(stderr, "inode compressed blocks: %lu\n", compressed_blocks);
	struct z_erofs_vle_decompressed_index *last = first + last_head_lcn;
	advise = le16_to_cpu(last->di_advise);
	type = (advise >> Z_EROFS_VLE_DI_CLUSTER_TYPE_BIT) &&
			((1 << Z_EROFS_VLE_DI_CLUSTER_TYPE_BITS) - 1);
	
	int last_block_size = 0;

	switch (type) {
		case Z_EROFS_VLE_CLUSTER_TYPE_PLAIN:
			filesize += (inode->i_size - last_head_lcn * lcluster_size - le16_to_cpu(last->di_clusterofs));
			break;
		case Z_EROFS_VLE_CLUSTER_TYPE_HEAD:
			// erofs_off_t offset = blknr_to_addr(last->di_u.blkaddr);
			// int last_cluster_size = pread64(erofs_devfd, 

			last_block_size = z_erofs_get_last_cluster_size_from_disk(last, inode->i_size - lcluster_size * last_head_lcn - last->di_clusterofs);
			if (last_block_size < 0) {
				fprintf(stderr, "error occurred while get last extent size\n");
				break;
			}
			fprintf(stderr, "last block size: %d\n", last_block_size);
			filesize += last_block_size;
			break;
		default:
			return -ENOTSUP;
	}

	fprintf(stderr, "filesize: %lu\n\n", filesize);
	return filesize;
}

static erofs_off_t erofs_get_file_actual_size(struct erofs_inode *inode)
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




static void dumpfs_print_superblock()
{
	fprintf(stderr, "Filesystem magic number:	0x%04X\n", EROFS_SUPER_MAGIC_V1);
	fprintf(stderr, "Filesystem blocks: 		%lu\n", sbi.blocks);

	fprintf(stderr, "Filesystem meta block:		%u\n", sbi.meta_blkaddr);
	fprintf(stderr, "Filesystem xattr block:		%u\n", sbi.xattr_blkaddr);
	fprintf(stderr, "Filesystem root nid:		%ld\n", sbi.root_nid);

	time_t t = sbi.build_time;
	fprintf(stderr, "Filesystem created:		%s", ctime(&t));

	fprintf(stderr, "Filesystem inodes count:	%ld\n", sbi.inos);
	fprintf(stderr, "Filesystem lz4 max distanve:	%d\n", sbi.lz4_max_distance);
	
	fprintf(stderr, "Filesystem uuid:		");
	for (int i = 0; i < 16; i++)
		fprintf(stderr, "%02x", sbi.uuid[i]);
	fprintf(stderr, "\n");
}

// static erofs_nid_t read_dir_for_ino (erofs_nid_t nid, erofs_nid_t parent_nid, struct erofs_inode *inode)
// {
// 	
// 	int ret;
// 	char buf[EROFS_BLKSIZ];
// 	erofs_nid_t target = 0;
// 	erofs_off_t offset;
// 	//fprintf(stderr, "read_dir: %lu\n", nid);
// 
// 	offset = 0;
// 	while (offset < inode->i_size) {
// 		erofs_off_t maxsize = min_t(erofs_off_t,
// 					inode->i_size - offset, EROFS_BLKSIZ);
// 		struct erofs_dirent *de = (void *)buf;
// 		struct erofs_dirent *end;
// 		unsigned int nameoff;
// 
// 		ret = erofs_pread(inode, buf, maxsize, offset);
// 		if (ret)
// 			return ret;
// 
// 		nameoff = le16_to_cpu(de->nameoff);
// 
// 		if (nameoff < sizeof(struct erofs_dirent) ||
// 		    nameoff >= PAGE_SIZE) {
// 			erofs_err("invalid de[0].nameoff %u @ nid %llu",
// 				  nameoff, nid | 0ULL);
// 			return -EFSCORRUPTED;
// 		}
// 
// 		end = (void *)buf + nameoff;
// 		while (de < end) {
// 
// 			const char *de_name;
// 			unsigned int de_namelen;
// 
// 			nameoff = le16_to_cpu(de->nameoff);
// 			de_name = (char *)buf + nameoff;
// 
// 			/* the last dirent in the block? */
// 			if (de + 1 >= end)
// 				de_namelen = strnlen(de_name, maxsize - nameoff);
// 			else
// 				de_namelen = le16_to_cpu(de[1].nameoff) - nameoff;
// 
// 			/* a corrupted entry is found */
// 			if (nameoff + de_namelen > maxsize ||
// 			    de_namelen > EROFS_NAME_LEN) {
// 				erofs_err("bogus dirent @ nid %llu", le64_to_cpu(de->nid) | 0ULL);
// 				DBG_BUGON(1);
// 				return -EFSCORRUPTED;
// 			}
// 
// 			struct erofs_inode child = { .nid = de->nid };
// 			ret = erofs_read_inode_from_disk(&child);
// 			if (ret < 0) {
// 				fprintf(stderr, "read child inode failed\n");
// 				return 0;
// 			}
// 
// 			if (child.i_ino[0] == dumpcfg.ino) {
// 				char filename[255] = {0};
// 				memcpy(filename, de_name, de_namelen);
// 				fprintf(stderr, "Filename:		%s\n", filename);
// 				return de->nid;
// 			}
// 			if (de->file_type == EROFS_FT_DIR && de->nid != parent_nid && de->nid != nid) {
// 				target = read_dir_for_ino(de->nid, nid, &child);
// 				if (target > 0) {
// 					memcpy(inode, &child, sizeof(struct erofs_inode));
// 					return target;
// 				}
// 			}
// 			++de;
// 		}
// 		offset += maxsize;
// 	}
// 	return 0;
// }


static void dumpfs_print_inode()
{
	erofs_nid_t nid = dumpcfg.ino;
	struct erofs_inode inode = {.nid = nid};
	int err = 0;

	fprintf(stderr, "Inode %lu info: \n", dumpcfg.ino);

	// err = erofs_read_inode_from_disk(&inode);
	// if (err < 0) {
	// 	fprintf(stderr, "get root inode failed!\n");
	// 	return;
	// }

	// if (dumpcfg.ino != 0) 
	// 	nid = read_dir_for_ino(sbi.root_nid, sbi.root_nid, &inode);
	// if (nid == 0) {
	// 	fprintf(stderr, "read inode failed\n");
	// 	return;
	// }
	
	// inode.nid = nid;
	err = erofs_read_inode_from_disk(&inode);
	if (err < 0) {
		fprintf(stderr, "error occured\n");
		return;
	}

	fprintf(stderr, "nid:			%lu\n", nid);

	fprintf(stderr, "File inode:		%lu\n", inode.i_ino[0]);
	fprintf(stderr, "File size:		%lu\n", inode.i_size);
	fprintf(stderr, "File nid:		%lu\n", inode.nid);

	fprintf(stderr, "File extent size:	%u\n", inode.extent_isize);
	fprintf(stderr, "File xattr size:	%u\n", inode.xattr_isize);

	//get file type
	switch (inode.i_mode & S_IFMT) {
		case S_IFREG:
			fprintf(stderr, "File is Regular File\n");
			break;
		case S_IFDIR:
			fprintf(stderr, "File is Directory File\n");
			break;
		case S_IFLNK:
			fprintf(stderr, "File is Link File\n");
			break;
		case S_IFCHR:
			fprintf(stderr, "File is CharDev File\n");
			break;
		case S_IFBLK:
			fprintf(stderr, "File is BLKDev File\n");
			break;
		case S_IFIFO:
			fprintf(stderr, "File is FIFO File\n");
			break;
		case S_IFSOCK:
			fprintf(stderr, "File is Sock File\n");
			break;
		default:
			break;
		}


	erofs_off_t size = erofs_get_file_actual_size(&inode);
	fprintf(stderr, "File Original size:	%lu\n"
			"File On-Disk size:	%lu\n", inode.i_size, size);

	switch (inode.datalayout)
	{
	case EROFS_INODE_FLAT_PLAIN:
		fprintf(stderr, "File datalayout:	EROFS_INODE_FLAT_PLAIN\n");
		break;
	case EROFS_INODE_FLAT_COMPRESSION_LEGACY:
		fprintf(stderr, "File datalayout:	EROFS_INODE_FLAT_COMPRESSION_LEGACY\n");
		break;
	case EROFS_INODE_FLAT_INLINE:
		fprintf(stderr, "File datalayout:	EROFS_INODE_FLAT_INLINE\n");
		break;
	case EROFS_INODE_FLAT_COMPRESSION:
		fprintf(stderr, "File datalayout:	EROFS_INODE_FLAT_COMPRESSION\n");
		break;
	default:
		break;
	}

	time_t t = inode.i_ctime;
	fprintf(stderr, "File create time:	%s\n", ctime(&t));
	
	fprintf(stderr, "File uid:		%u\n", inode.i_uid);
	fprintf(stderr, "File gid:		%u\n", inode.i_gid);
	fprintf(stderr, "File hard-link count:	%u\n", inode.i_nlink);

	return;

}

static void dumpfs_print_inode_phy()
{
	int err = 0;
	erofs_nid_t nid = dumpcfg.ino;
	struct erofs_inode inode = {.nid = nid};
	//struct erofs_inode inode = { .i_ino[0] = dumpcfg.ino };
	fprintf(stderr, "Inode %lu on-disk info: \n", dumpcfg.ino);

	// err = erofs_read_inode_from_disk(&inode);
	// if (err < 0) {
	// 	fprintf(stderr, "get root inode failed!\n");
	// 	return;
	// }
	// if (dumpcfg.ino != 0) 
	// 	nid = read_dir_for_ino(sbi.root_nid, sbi.root_nid, &inode);
	// if (nid == 0) {
	// 	fprintf(stderr, "read inode failed\n");
	// 	return;
	// }

	// inode.nid = nid;
	err = erofs_read_inode_from_disk(&inode);
	if (err < 0) {
		fprintf(stderr, "error occured\n");
		return;
	}

	const erofs_off_t ibase = iloc(inode.nid);
	const erofs_off_t pos = Z_EROFS_VLE_LEGACY_INDEX_ALIGN(ibase + inode.inode_isize + inode.xattr_isize);
	struct z_erofs_vle_decompressed_index *first;
	erofs_blk_t blocks = inode.u.i_blocks;
	erofs_blk_t start = 0;
	erofs_blk_t end = 0;
	void *compressdata;
	switch (inode.datalayout) {
	case EROFS_INODE_FLAT_INLINE:
	case EROFS_INODE_FLAT_PLAIN:
		start = inode.u.i_blkaddr + 1;
		end = start + BLK_ROUND_UP(inode.i_size) - 1;
		fprintf(stderr, "Inode ino:	%lu\n", inode.i_ino[0]);
		fprintf(stderr, "Filesize:	%lu\n", inode.i_size);
		fprintf(stderr, "Plain Block Address:		%u - %u\n", start, end);
		break;

	case EROFS_INODE_FLAT_COMPRESSION_LEGACY:
	case EROFS_INODE_FLAT_COMPRESSION:
		
		inode.extent_isize = vle_compressmeta_capacity(inode.i_size);
		compressdata = malloc(inode.extent_isize);
		if (!compressdata) {
			fprintf(stderr, "malloc failed!\n");
			return;
		}
		
		err = dev_read(compressdata, pos, sizeof(struct z_erofs_vle_decompressed_index));
		if (err < 0) {
			fprintf(stderr, "read compressmeta failed!\n");
			return;
		}

		first = compressdata;
		start = first->di_u.blkaddr;
		end = start - 1 + blocks;
		fprintf(stderr, "Compressed Block Address:		%u - %u\n", start, end);
		break;
	}
	return;
}
// file num、file size、file type
static int read_dir(erofs_nid_t nid, erofs_nid_t parent_nid) 
{
	struct erofs_inode vi = { .nid = nid};
	int ret;
	char buf[EROFS_BLKSIZ];
	erofs_off_t offset;
	
	//fprintf(stderr, "read_dir: %lu\n", nid);

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
			if (de->nid != nid && de->nid != parent_nid)
				statistics.files++;

			erofs_off_t actual_size = 0;
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
				if (de->nid != nid && de->nid != parent_nid) {	
					statistics.dir_files++;
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


	fprintf(stderr, "Filesystem total original file size:	%lu Bytes\n", statistics.files_total_origin_size);
	fprintf(stderr, "Filesystem total file size:	%lu Bytes\n", statistics.files_total_size);

	return;
}

int main(int argc, char** argv) 
{
	int err = 0;
	struct erofs_inode *root_inode;
	// init config
	erofs_init_configure();

	
	err = dumpfs_parse_options_cfg(argc, argv);	
	if (err) {
		fprintf(stderr, "parse config failed\n");
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

	if (dumpcfg.print_superblock) {
		dumpfs_print_superblock();
	}

	root_inode = erofs_new_inode();
	err = erofs_ilookup("/", root_inode);	
	if (err) {
		fprintf(stderr, "failed to look up root inode");
		return 1;
	}

	if (dumpcfg.print_inode)
		dumpfs_print_inode();
	else if (dumpcfg.print_inode_phy)
		dumpfs_print_inode_phy();
	
	if (dumpcfg.print_statistic)
		dumpfs_print_statistic();

	return 0;
}