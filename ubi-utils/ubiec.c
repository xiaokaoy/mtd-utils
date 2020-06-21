
#define MAX_CONSECUTIVE_BAD_BLOCKS 4

#define PROGRAM_NAME    "ubiformat"

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
	unsigned int override_ec:1;
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


int main(int argc, char * const argv[])
{
	int err, verbose;
	libmtd_t libmtd;
	struct mtd_info mtd_info;
	struct mtd_dev_info mtd;
	libubi_t libubi;
	struct ubigen_info ui;
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

	if (!mtd.writable) {
		errmsg("mtd%d (%s) is a read-only device", mtd.mtd_num, args.node);
		goto out_close;
	}
#if 0
	/* Make sure this MTD device is not attached to UBI */
	libubi = libubi_open();
	if (libubi) {
		int ubi_dev_num;

		err = mtd_num2ubi_dev(libubi, mtd.mtd_num, &ubi_dev_num);
		libubi_close(libubi);
		if (!err) {
			errmsg("please, first detach mtd%d (%s) from ubi%d",
			       mtd.mtd_num, args.node, ubi_dev_num);
			goto out_close;
		}
	}
#endif
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

	if (!args.override_ec && si->empty_cnt < si->good_cnt) {
		int percent = (si->ok_cnt * 100) / si->good_cnt;

		/*
		 * Make sure the majority of eraseblocks have valid
		 * erase counters.
		 */
		if (percent < 50) {
			if (!args.yes || !args.quiet) {
				warnmsg("only %d of %d eraseblocks have valid erase counter",
					si->ok_cnt, si->good_cnt);
				normsg("erase counter 0 will be used for all eraseblocks");
				normsg("note, arbitrary erase counter value may be specified using -e option");
			}
			if (!args.yes && want_exit()) {
				if (args.yes && !args.quiet)
					printf("yes\n");
				goto out_free;
			}
			 args.ec = 0;
			 args.override_ec = 1;
		} else if (percent < 95) {
			if (!args.yes || !args.quiet) {
				warnmsg("only %d of %d eraseblocks have valid erase counter",
					si->ok_cnt, si->good_cnt);
				normsg("mean erase counter %lld will be used for the rest of eraseblock",
				       si->mean_ec);
			}
			if (!args.yes && want_exit()) {
				if (args.yes && !args.quiet)
					printf("yes\n");
				goto out_free;
			}
			args.ec = si->mean_ec;
			args.override_ec = 1;
		}
	}

	if (!args.quiet && args.override_ec)
		normsg("use erase counter %lld for all eraseblocks", args.ec);

	ubigen_info_init(&ui, mtd.eb_size, mtd.min_io_size, mtd.subpage_size,
			 args.vid_hdr_offs, args.ubi_ver, args.image_seq);

	if (si->vid_hdr_offs != -1 && ui.vid_hdr_offs != si->vid_hdr_offs) {
		/*
		 * Hmm, what we read from flash and what we calculated using
		 * min. I/O unit size and sub-page size differs.
		 */
		if (!args.yes || !args.quiet) {
			warnmsg("VID header and data offsets on flash are %d and %d, "
				"which is different to requested offsets %d and %d",
				si->vid_hdr_offs, si->data_offs, ui.vid_hdr_offs,
				ui.data_offs);
			normsg_cont("use new offsets %d and %d? ",
				    ui.vid_hdr_offs, ui.data_offs);
		}
		if (args.yes || answer_is_yes(NULL)) {
			if (args.yes && !args.quiet)
				printf("yes\n");
		} else
			ubigen_info_init(&ui, mtd.eb_size, mtd.min_io_size, 0,
					 si->vid_hdr_offs, args.ubi_ver,
					 args.image_seq);
		normsg("use offsets %d and %d",  ui.vid_hdr_offs, ui.data_offs);
	}

#if 0
	if (args.image) {
		err = flash_image(libmtd, &mtd, &ui, si);
		if (err < 0)
			goto out_free;

		/*
		 * ubinize has create a UBI_LAYOUT_VOLUME_ID volume for image.
		 * So, we don't need to create again.
		 */
		err = format(libmtd, &mtd, &ui, si, err, 1);
		if (err)
			goto out_free;
	} else {
		err = format(libmtd, &mtd, &ui, si, 0, 0);
		if (err)
			goto out_free;
	}
#endif

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

