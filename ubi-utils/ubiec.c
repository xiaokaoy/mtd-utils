
#define PROGRAM_NAME    "ubiec"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdlib.h>
#include <getopt.h>

#include <mtd_swab.h>
#include <mtd/ubi-media.h>
#include <mtd/mtd-user.h>
#include <libubi.h>
#include <libmtd.h>
#include <libscan.h>
#include <libubigen.h>
#include <mtd_swab.h>
#include <crc32.h>
#include "common.h"

/* The variables below are set by command line arguments */
struct args {
	unsigned int manual_subpage;
	int subpage_size;
	int ubi_ver;
	long long ec;
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
"-h, -?, --help               print help message\n"
"-V, --version                print program version\n";

static const char usage[] =
"Usage: " PROGRAM_NAME " <MTD device node file name> ";

static const struct option long_options[] = {
	{ .name = "help",            .has_arg = 0, .flag = NULL, .val = 'h' },
	{ .name = "version",         .has_arg = 0, .flag = NULL, .val = 'V' },
	{ NULL, 0, NULL, 0},
};

static int parse_opt(int argc, char * const argv[])
{
	while (1) {
		int key;

		key = getopt_long(argc, argv, "nh?Vyqve:x:s:O:f:S:", long_options, NULL);
		if (key == -1)
			break;

		switch (key) {
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

	if (optind == argc)
		return errmsg("MTD device name was not specified (use -h for help)");
	else if (optind != argc - 1)
		return errmsg("more then one MTD device specified (use -h for help)");

	args.node = argv[optind];
	return 0;
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

static int all_ff(const void *buf, int len)
{
	int i;
	const uint8_t *p = buf;

	for (i = 0; i < len; i++)
		if (p[i] != 0xFF)
			return 0;
	return 1;
}

static int ubi_show_ec(struct mtd_dev_info *mtd, int fd, struct ubi_scan_info **info)
{
	int eb, v=1; // if v=true, show info on bad/empty blks
	struct ubi_scan_info *si;
	unsigned long long sum = 0;
	unsigned long long ec_latest = UINT64_MAX - 1;
	int first_eb_with_latest_ec = -1;

	si = calloc(1, sizeof(struct ubi_scan_info));
	if (!si)
		return sys_errmsg("cannot allocate %zd bytes of memory",
				  sizeof(struct ubi_scan_info));

	si->ec = calloc(mtd->eb_cnt, sizeof(uint32_t));
	if (!si->ec) {
		sys_errmsg("cannot allocate %zd bytes of memory",
			   sizeof(struct ubi_scan_info));
		goto out_si;
	}

	si->vid_hdr_offs = si->data_offs = -1;

#define ERASE_BLK_WIDTH  15
#define ERASE_CNT_WIDTH  20
	printf("%*s %*s\n", ERASE_BLK_WIDTH, "Erase Block#", ERASE_CNT_WIDTH, "Erase Count");

	for (eb = 0; eb < mtd->eb_cnt; eb++) {
		int ret;
		uint32_t crc;
		struct ubi_ec_hdr ech;
		unsigned long long ec;

		ret = mtd_is_bad(mtd, fd, eb);
		if (ret == -1)
			goto out_ec;
		if (ret) {
			si->bad_cnt += 1;
			si->ec[eb] = EB_BAD;
			if (v)
				printf(": bad\n");
			continue;
		}

		ret = mtd_read(mtd, fd, eb, 0, &ech, sizeof(struct ubi_ec_hdr));
		if (ret < 0)
			goto out_ec;

		if (be32_to_cpu(ech.magic) != UBI_EC_HDR_MAGIC) {
			if (all_ff(&ech, sizeof(struct ubi_ec_hdr))) {
				si->empty_cnt += 1;
				si->ec[eb] = EB_EMPTY;
				if (v)
					printf(": empty\n");
			} else {
				si->alien_cnt += 1;
				si->ec[eb] = EB_ALIEN;
				if (v)
					printf(": alien\n");
			}
			continue;
		}

		crc = mtd_crc32(UBI_CRC32_INIT, &ech, UBI_EC_HDR_SIZE_CRC);
		if (be32_to_cpu(ech.hdr_crc) != crc) {
			si->corrupted_cnt += 1;
			si->ec[eb] = EB_CORRUPTED;
			if (v)
				printf(": bad CRC %#08x, should be %#08x\n",
				       crc, be32_to_cpu(ech.hdr_crc));
			continue;
		}

		ec = be64_to_cpu(ech.ec);
		if (ec > EC_MAX) {			
			errmsg("erase counter in EB %d is %llu, while this "
			       "program expects them to be less than %u",
			       eb, ec, EC_MAX);
			goto out_ec;
		}

		if (si->vid_hdr_offs == -1) {
			si->vid_hdr_offs = be32_to_cpu(ech.vid_hdr_offset);
			si->data_offs = be32_to_cpu(ech.data_offset);
			if (si->data_offs % mtd->min_io_size) {
				if (v)
					printf(": corrupted because of the below\n");
				warnmsg("bad data offset %d at eraseblock %d (n"
					"of multiple of min. I/O unit size %d)",
					si->data_offs, eb, mtd->min_io_size);
				warnmsg("treat eraseblock %d as corrupted", eb);
				si->corrupted_cnt += 1;
				si->ec[eb] = EB_CORRUPTED;
				continue;

			}
		} else {
			if ((int)be32_to_cpu(ech.vid_hdr_offset) != si->vid_hdr_offs) {
				if (v)
					printf(": corrupted because of the below\n");
				warnmsg("inconsistent VID header offset: was "
					"%d, but is %d in eraseblock %d",
					si->vid_hdr_offs,
					be32_to_cpu(ech.vid_hdr_offset), eb);
				warnmsg("treat eraseblock %d as corrupted", eb);
				si->corrupted_cnt += 1;
				si->ec[eb] = EB_CORRUPTED;
				continue;
			}
			if ((int)be32_to_cpu(ech.data_offset) != si->data_offs) {
				if (v)
					printf(": corrupted because of the below\n");
				warnmsg("inconsistent data offset: was %d, but"
					" is %d in eraseblock %d",
					si->data_offs,
					be32_to_cpu(ech.data_offset), eb);
				warnmsg("treat eraseblock %d as corrupted", eb);
				si->corrupted_cnt += 1;
				si->ec[eb] = EB_CORRUPTED;
				continue;
			}
		}

		si->ok_cnt += 1;
		si->ec[eb] = ec;

		if (ec != ec_latest)
		{
			if (eb != 0)
			{
				if (eb - 1 == first_eb_with_latest_ec)
					printf("%*d %*llu\n", ERASE_BLK_WIDTH, eb - 1, ERASE_CNT_WIDTH, ec_latest);
				else
					printf("%*d-%-*d %*llu\n", ERASE_BLK_WIDTH/2, first_eb_with_latest_ec, ERASE_BLK_WIDTH/2, eb-1, ERASE_CNT_WIDTH, ec_latest);
			}

			ec_latest = ec;
			first_eb_with_latest_ec = eb;
		}
		
		if (eb == mtd->eb_cnt - 1)
		{
			if (eb == first_eb_with_latest_ec)
				printf("%*d %*llu\n", ERASE_BLK_WIDTH, eb, ERASE_CNT_WIDTH, ec_latest);
			else
				printf("%*d-%-*d %*llu\n", ERASE_BLK_WIDTH/2, first_eb_with_latest_ec, ERASE_BLK_WIDTH/2, eb, ERASE_CNT_WIDTH, ec_latest);
		}		
	}

	if (si->ok_cnt != 0) {
		/* Calculate mean erase counter */
		for (eb = 0; eb < mtd->eb_cnt; eb++) {
			if (si->ec[eb] > EC_MAX)
				continue;
			sum += si->ec[eb];
		}
		si->mean_ec = sum / si->ok_cnt;
	}

	si->good_cnt = mtd->eb_cnt - si->bad_cnt;
	printf("mean EC %lld, %d OK, %d corrupted, %d empty, %d "
		"alien, bad %d\n", si->mean_ec, si->ok_cnt, si->corrupted_cnt,
		si->empty_cnt, si->alien_cnt, si->bad_cnt);

	*info = si;
	return 0;

out_ec:
	free(si->ec);
out_si:
	free(si);
	*info = NULL;
	return -1;
}



int main(int argc, char * const argv[])
{
	int err;
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

	args.node_fd = open(args.node, O_RDWR);
	if (args.node_fd == -1) {
		sys_errmsg("cannot open \"%s\"", args.node);
		goto out_close_mtd;
	}

	normsg_cont("mtd%d (%s), size ", mtd.mtd_num, mtd.type_str);
	util_print_bytes(mtd.size, 1);
	printf(", %d eraseblocks of ", mtd.eb_cnt);
	util_print_bytes(mtd.eb_size, 1);
	printf(", min. I/O size %d bytes\n", mtd.min_io_size);

	err = ubi_show_ec(&mtd, args.node_fd, &si);
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

	if (si->ok_cnt)
		normsg("%d eraseblocks have valid erase counter, mean value is %lld",
				si->ok_cnt, si->mean_ec);
	if (si->empty_cnt)
		normsg("%d eraseblocks are supposedly empty", si->empty_cnt);
	if (si->corrupted_cnt)
		normsg("%d corrupted erase counters", si->corrupted_cnt);
	print_bad_eraseblocks(&mtd, si);	

	if (si->alien_cnt) {
		warnmsg("%d of %d eraseblocks contain non-UBI data",
			si->alien_cnt, si->good_cnt);
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

