
#define MAX_CONSECUTIVE_BAD_BLOCKS 4

#define PROGRAM_NAME    "ubiec"

#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <getopt.h>
#include <fcntl.h>

#include <libubi.h>
#include <libmtd.h>
#include <libscan.h>
#include <libubigen.h>
#include <mtd_swab.h>
#include <crc32.h>
#include "common.h"


/* The variables below are set by command line arguments */
struct args {
	unsigned int yes:1;
	unsigned int quiet:1;
	unsigned int verbose:1;
	unsigned int manual_subpage;
	int subpage_size;
	int vid_hdr_offs;
	int ubi_ver;
	uint32_t image_seq;
	off_t image_sz;
	long long ec;
	const char *image;
	const char *node;
	int node_fd;
};

static struct args args =
{
	.ubi_ver   = 1,
};

static const char doc[] = PROGRAM_NAME " version " VERSION
		" - a tool to show erase counts of UBI erase blocks";

static const char optionsstr[] =
"-y, --yes                    assume the answer is \"yes\" for all question\n"
"                             this program would otherwise ask\n"
"-v, --verbose                be verbose\n"
"-h, -?, --help               print help message\n"
"-V, --version                print program version\n";

static const char usage[] =
"Usage: " PROGRAM_NAME " <MTD device node file name> ";

static const struct option long_options[] = {
	{ .name = "yes",             .has_arg = 0, .flag = NULL, .val = 'y' },
	{ .name = "erase-counter",   .has_arg = 1, .flag = NULL, .val = 'e' },
	{ .name = "verbose",         .has_arg = 0, .flag = NULL, .val = 'v' },
	{ .name = "ubi-ver",         .has_arg = 1, .flag = NULL, .val = 'x' },
	{ .name = "help",            .has_arg = 0, .flag = NULL, .val = 'h' },
	{ .name = "version",         .has_arg = 0, .flag = NULL, .val = 'V' },
	{ NULL, 0, NULL, 0},
};

static int parse_opt(int argc, char * const argv[])
{
	util_srand();
	args.image_seq = rand();

	while (1) {
		int key, error = 0;
		unsigned long int image_seq;

		key = getopt_long(argc, argv, "nh?Vyqve:x:s:O:f:S:", long_options, NULL);
		if (key == -1)
			break;

		switch (key) {
		case 'O':
			args.vid_hdr_offs = simple_strtoul(optarg, &error);
			if (error || args.vid_hdr_offs <= 0)
				return errmsg("bad VID header offset: \"%s\"", optarg);
			break;

		case 'f':
			args.image = optarg;
			break;

		case 'S':
			args.image_sz = util_get_bytes(optarg);
			if (args.image_sz <= 0)
				return errmsg("bad image-size: \"%s\"", optarg);
			break;

		case 'y':
			args.yes = 1;
			break;

		case 'x':
			args.ubi_ver = simple_strtoul(optarg, &error);
			if (error || args.ubi_ver < 0)
				return errmsg("bad UBI version: \"%s\"", optarg);
			break;

		case 'Q':
			image_seq = simple_strtoul(optarg, &error);
			if (error || image_seq > 0xFFFFFFFF)
				return errmsg("bad UBI image sequence number: \"%s\"", optarg);
			args.image_seq = image_seq;
			break;


		case 'v':
			args.verbose = 1;
			break;

		case 'V':
			common_print_version();
			exit(EXIT_SUCCESS);

		case 'h':
			printf("%s\n\n", doc);
			printf("%s\n\n", usage);
			printf("%s\n", optionsstr);
			exit(EXIT_SUCCESS);
		case '?':
			printf("%s\n\n", doc);
			printf("%s\n\n", usage);
			printf("%s\n", optionsstr);
			return -1;

		case ':':
			return errmsg("parameter is missing");

		default:
			fprintf(stderr, "Use -h for help\n");
			return -1;
		}
	}

	if (args.quiet && args.verbose)
		return errmsg("using \"-q\" and \"-v\" at the same time does not make sense");

	if (optind == argc)
		return errmsg("MTD device name was not specified (use -h for help)");
	else if (optind != argc - 1)
		return errmsg("more then one MTD device specified (use -h for help)");

	args.node = argv[optind];
	return 0;
}

static int want_exit(void)
{
	return prompt("continue?", false) == true ? 0 : 1;
}

static void print_bad_eraseblocks(const struct mtd_dev_info *mtd,
				  const struct ubi_scan_info *si)
{
	int first = 1, eb;

	if (si->bad_cnt == 0)
		return;

	normsg_cont("%d bad eraseblocks found, numbers: ", si->bad_cnt);
	for (eb = 0; eb < mtd->eb_cnt; eb++) {
		if (si->ec[eb] != EB_BAD)
			continue;
		if (first) {
			printf("%d", eb);
			first = 0;
		} else
			printf(", %d", eb);
	}
	printf("\n");
}


int main(int argc, char * const argv[])
{
	int err, verbose;
	libmtd_t libmtd;
	struct mtd_info mtd_info;
	struct mtd_dev_info mtd;
	struct ubi_scan_info *si;


	err = parse_opt(argc, argv);
	if (err)
		return -1;

	libmtd = libmtd_open();
	if (!libmtd)
		return errmsg("MTD subsystem is not present");

	err = mtd_get_info(libmtd, &mtd_info);
	if (err) {
		sys_errmsg("cannot get MTD information");
		goto out_close_mtd;
	}

	err = mtd_get_dev_info(libmtd, args.node, &mtd);
	if (err) {
		sys_errmsg("cannot get information about \"%s\"", args.node);
		goto out_close_mtd;
	}

	if (!is_power_of_2(mtd.min_io_size)) {
		errmsg("min. I/O size is %d, but should be power of 2",
		       mtd.min_io_size);
		goto out_close_mtd;
	}

	if (!mtd_info.sysfs_supported) {
		/*
		 * Linux kernels older than 2.6.30 did not support sysfs
		 * interface, and it is impossible to find out sub-page
		 * size in these kernels. This is why users should
		 * provide -s option.
		 */
		if (args.subpage_size == 0) {
			warnmsg("your MTD system is old and it is impossible "
				"to detect sub-page size. Use -s to get rid "
				"of this warning");
			normsg("assume sub-page to be %d", mtd.subpage_size);
		} else {
			mtd.subpage_size = args.subpage_size;
			args.manual_subpage = 1;
		}
	} else if (args.subpage_size && args.subpage_size != mtd.subpage_size) {
		mtd.subpage_size = args.subpage_size;
		args.manual_subpage = 1;
	}

	if (args.manual_subpage) {
		/* Do some sanity check */
		if (args.subpage_size > mtd.min_io_size) {
			errmsg("sub-page cannot be larger than min. I/O unit");
			goto out_close_mtd;
		}

		if (mtd.min_io_size % args.subpage_size) {
			errmsg("min. I/O unit size should be multiple of "
			       "sub-page size");
			goto out_close_mtd;
		}
	}

	args.node_fd = open(args.node, O_RDWR);
	if (args.node_fd == -1) {
		sys_errmsg("cannot open \"%s\"", args.node);
		goto out_close_mtd;
	}

	/* Validate VID header offset if it was specified */
	if (args.vid_hdr_offs != 0) {
		if (args.vid_hdr_offs % 8) {
			errmsg("VID header offset has to be multiple of min. I/O unit size");
			goto out_close;
		}
		if (args.vid_hdr_offs + (int)UBI_VID_HDR_SIZE > mtd.eb_size) {
			errmsg("bad VID header offset");
			goto out_close;
		}
	}

	if (!args.quiet) {
		normsg_cont("mtd%d (%s), size ", mtd.mtd_num, mtd.type_str);
		util_print_bytes(mtd.size, 1);
		printf(", %d eraseblocks of ", mtd.eb_cnt);
		util_print_bytes(mtd.eb_size, 1);
		printf(", min. I/O size %d bytes\n", mtd.min_io_size);
	}

	if (args.quiet)
		verbose = 0;
	else if (args.verbose)
		verbose = 2;
	else
		verbose = 1;
	err = ubi_show_ec(&mtd, args.node_fd, &si, verbose);
	if (err) {
		errmsg("failed to scan mtd%d (%s)", mtd.mtd_num, args.node);
		goto out_close;
	}

	if (si->good_cnt == 0) {
		errmsg("all %d eraseblocks are bad", si->bad_cnt);
		goto out_free;
	}

	if (si->good_cnt < 2) {
		errmsg("too few non-bad eraseblocks (%d) on mtd%d",
		       si->good_cnt, mtd.mtd_num);
		goto out_free;
	}

	if (!args.quiet) {
		if (si->ok_cnt)
			normsg("%d eraseblocks have valid erase counter, mean value is %lld",
			       si->ok_cnt, si->mean_ec);
		if (si->empty_cnt)
			normsg("%d eraseblocks are supposedly empty", si->empty_cnt);
		if (si->corrupted_cnt)
			normsg("%d corrupted erase counters", si->corrupted_cnt);
		print_bad_eraseblocks(&mtd, si);
	}

	if (si->alien_cnt) {
		if (!args.yes || !args.quiet)
			warnmsg("%d of %d eraseblocks contain non-UBI data",
				si->alien_cnt, si->good_cnt);
		if (!args.yes && want_exit()) {
			if (args.yes && !args.quiet)
				printf("yes\n");
			goto out_free;
		}
	}

	ubi_scan_free(si);
	close(args.node_fd);
	libmtd_close(libmtd);
	return 0;

out_free:
	ubi_scan_free(si);
out_close:
	close(args.node_fd);
out_close_mtd:
	libmtd_close(libmtd);
	return -1;
}

