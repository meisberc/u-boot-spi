/*
 * Command for accessing MTD device.
 *
 * Copyright (C) 2016 Jagan Teki <jagan@openedev.com>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <dm.h>
#include <mtd.h>
#include <spi.h>

#include <asm/io.h>
#include <jffs2/jffs2.h>

static struct udevice *mtd_cur_dev;
static bool dev_type = false;

static int cmd_mtd_set_devnum(unsigned int devnum)
{
	struct udevice *dev;
	int ret;

	ret = uclass_get_device_by_seq(UCLASS_SPI, devnum, &dev);
	if (ret) {
		debug("%s: No MTD device %d\n", __func__, devnum);
		goto mtd;
	}
	dev_type = true;
	mtd_cur_dev = dev;

	return 0;
mtd:
	ret = uclass_get_device_by_seq(UCLASS_MTD, devnum, &dev);
	if (ret) {
		debug("%s: No MTD device %d\n", __func__, devnum);
		return ret;
	}
	mtd_cur_dev = dev;

	return 0;
}

static int mtd_get_cur_dev(struct udevice **devp)
{
	if (!mtd_cur_dev) {
		puts("No MTD device selected\n");
		return -ENODEV;
	}
	*devp = mtd_cur_dev;

	return 0;
}

static int do_mtd_write_read(int argc, char * const argv[])
{
	struct udevice *dev;
	struct mtd_info *mtd;
	loff_t offset, addr, len, maxsize;
	u_char *buf;
	char *endp;
	int idx = 0;
	int ret;

	if (argc < 3)
		return -1;

	ret = mtd_get_cur_dev(&dev);
	if (ret)
		return CMD_RET_FAILURE;

	addr = simple_strtoul(argv[1], &endp, 16);
	if (*argv[1] == 0 || *endp != 0)
		return -1;

	mtd = mtd_get_info(dev);
	if (mtd_arg_off_size(argc - 2, &argv[2], &idx, &offset, &len,
			     &maxsize, MTD_DEV_TYPE_NOR, mtd->size))
		return -1;

	buf = map_physmem(addr, len, MAP_WRBACK);
	if (!buf) {
		puts("failed to map physical memory\n");
		return 1;
	}

	if (strcmp(argv[0], "write") == 0)
		ret = dm_mtd_write(dev, offset, len, (size_t *)&len, buf);
	else if (strcmp(argv[0], "read") == 0)
		ret = dm_mtd_read(dev, offset, len, (size_t *)&len, buf);

	printf("MTD: %zu bytes @ %#llx %s: ", (size_t)len, offset,
	       (strcmp(argv[0], "read") == 0) ? "Read" : "Written");
	if (ret)
		printf("ERROR %d\n", ret);
	else
		printf("OK\n");

	unmap_physmem(buf, len);

	return ret == 0 ? 0 : 1;
}

static int mtd_parse_len_arg(struct mtd_info *mtd, char *arg, loff_t *len)
{
	char *ep;
	char round_up_len; /* indicates if the "+length" form used */
	ulong len_arg;

	round_up_len = 0;
	if (*arg == '+') {
		round_up_len = 1;
		++arg;
	}

	len_arg = simple_strtoul(arg, &ep, 16);
	if (ep == arg || *ep != '\0')
		return -1;

	if (round_up_len && mtd->erasesize > 0)
		*len = ROUND(len_arg, mtd->erasesize);
	else
		*len = len_arg;

	return 1;
}

static int do_mtd_erase(int argc, char * const argv[])
{
	struct udevice *dev;
	struct mtd_info *mtd;
	struct erase_info instr;
	loff_t addr, len, maxsize;
	int idx = 0;
	int ret;

	if (argc < 3)
		return -1;

	ret = mtd_get_cur_dev(&dev);
	if (ret)
		return CMD_RET_FAILURE;

	mtd = mtd_get_info(dev);
	if (mtd_arg_off(argv[1], &idx, &addr, &len, &maxsize,
			MTD_DEV_TYPE_NOR, mtd->size))
		return -1;

	ret = mtd_parse_len_arg(mtd, argv[2], &len);
	if (ret != 1)
		return -1;

	instr.mtd = mtd;
	instr.addr = addr;
	instr.len = len;
	instr.callback = 0;
	ret = dm_mtd_erase(dev, &instr);
	printf("MTD: %zu bytes @ %#llx Erased: %s\n", (size_t)len, addr,
	       ret ? "ERROR" : "OK");

	return ret == 0 ? 0 : 1;
}

static int do_mtd_probe(int argc, char * const argv[])
{
	struct udevice *dev, *devp;
	int devnum, cs = 0;
	ulong speed = 0, mode = 0;
	int ret;

	devnum = simple_strtoul(argv[1], NULL, 10);

	debug("Setting MTD device to %d\n", devnum);
	ret = cmd_mtd_set_devnum(devnum);
	if (ret) {
		printf("failing to set MTD device %d\n", devnum);
		return CMD_RET_FAILURE;
	}

	ret = mtd_get_cur_dev(&dev);
	if (ret)
		return CMD_RET_FAILURE;

	if (dev_type) {
		ret = dm_spi_probe(devnum, cs, speed, mode, dev, &devp);
		if (ret) {
			printf("failed to probe SPI device %d\n", devnum);
			goto err;
		}
	} else {
		ret = dm_mtd_probe(dev, &devp);
		if (ret) {
			printf("failed to probe MTD device %d\n", devnum);
			goto err;
		}
	}

	return 0;
err:
	return CMD_RET_FAILURE;
}

static int do_mtd_info(void)
{
	struct udevice *dev;
	struct mtd_info *mtd;
	int ret;

	ret = mtd_get_cur_dev(&dev);
	if (ret)
		return CMD_RET_FAILURE;

	mtd = mtd_get_info(dev);
	printf("MTD Device %d: %s\n", dev->req_seq, mtd->name);
	printf(" Page size:\t%d B\n Erase size:\t", mtd->writebufsize);
	print_size(mtd->erasesize, "\n Size:\t\t");
	print_size(mtd->size, "");
	printf("\n");

	return 0;
}

static int do_mtd_list(void)
{
	struct udevice *dev, *dev1;
	struct uclass *uc, *uc1;
	int ret;

	ret = uclass_get(UCLASS_SPI, &uc);
	if (ret)
		goto mtd;

	uclass_foreach_dev(dev, uc) {
		printf("MTD %d:\t%s", dev->req_seq, dev->name);
			if (device_active(dev))
				printf("  (active %d)", dev->seq);
		printf("\n");
	}

mtd:
	ret = uclass_get(UCLASS_MTD, &uc1);
	if (ret)
		return CMD_RET_FAILURE;

	uclass_foreach_dev(dev1, uc1) {
		printf("MTD %d:\t%s", dev1->req_seq, dev1->name);
			if (device_active(dev1))
				printf("  (active %d)", dev1->seq);
		printf("\n");
	}

	return 0;
}

static int do_mtd(cmd_tbl_t *cmdtp, int flag, int argc,
			char * const argv[])
{
	const char *cmd;
	int ret = 0;

	cmd = argv[1];
	if (strcmp(cmd, "list") == 0) {
		if (argc > 2)
			goto usage;

		ret = do_mtd_list();
		goto done;
	}

	if (strcmp(cmd, "info") == 0) {
		if (argc > 2)
			goto usage;

		ret = do_mtd_info();
		goto done;
	}

	if (argc < 3)
		goto usage;

	--argc;
	++argv;

	if (strcmp(cmd, "probe") == 0) {
		ret = do_mtd_probe(argc, argv);
		goto done;
	}

	if (strcmp(cmd, "erase") == 0) {
		ret = do_mtd_erase(argc, argv);
		goto done;
	}

	if (strcmp(cmd, "write") == 0 || strcmp(cmd, "read") == 0) {
		ret = do_mtd_write_read(argc, argv);
		goto done;
	}

done:
	if (ret != -1)
		return ret;

usage:
	return CMD_RET_USAGE;
}

static char mtd_help_text[] =
	"list			- show list of MTD devices\n"
	"mtd info			- show current MTD device info\n"
	"mtd probe devnum		- probe the 'devnum' MTD device\n"
	"mtd erase offset len		- erase 'len' bytes from 'offset'\n"
	"mtd write addr to len		- write 'len' bytes to 'to' from 'addr'\n"
	"mtd read addr from len		- read 'len' bytes from 'from' to 'addr'";

U_BOOT_CMD(
	mtd, 5, 1, do_mtd,
	"MTD Sub-system",
	mtd_help_text
);
