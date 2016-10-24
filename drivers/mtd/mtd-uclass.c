/*
 * Copyright (C) 2016 Jagan Teki <jagan@openedev.com>
 * Copyright (C) 2015 Thomas Chou <thomas@wytron.com.tw>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <dm.h>
#include <errno.h>
#include <mtd.h>
#include <linux/log2.h>

int dm_mtd_read(struct udevice *dev, loff_t from, size_t len, size_t *retlen,
		u_char *buf)
{
	struct mtd_info *mtd = mtd_get_info(dev);

	*retlen = 0;
	if (from < 0 || from > mtd->size || len > mtd->size - from)
		return -EINVAL;
	if (!len)
		return 0;

	return mtd_get_ops(dev)->_read(dev, from, len, retlen, buf);
}

int dm_mtd_erase(struct udevice *dev, struct erase_info *instr)
{
	struct mtd_info *mtd = mtd_get_info(dev);

	if (instr->addr > mtd->size || instr->len > mtd->size - instr->addr)
		return -EINVAL;
	if (!(mtd->flags & MTD_WRITEABLE))
		return -EROFS;
	instr->fail_addr = MTD_FAIL_ADDR_UNKNOWN;
	if (!instr->len) {
		instr->state = MTD_ERASE_DONE;
		return 0;
	}

	return mtd_get_ops(dev)->_erase(dev, instr);
}

int dm_mtd_write(struct udevice *dev, loff_t to, size_t len, size_t *retlen,
		 const u_char *buf)
{
	struct mtd_info *mtd = mtd_get_info(dev);

	*retlen = 0;
	if (to < 0 || to > mtd->size || len > mtd->size - to)
		return -EINVAL;
	if (!mtd_get_ops(dev)->_write || !(mtd->flags & MTD_WRITEABLE))
		return -EROFS;
	if (!len)
		return 0;

	return mtd_get_ops(dev)->_write(dev, to, len, retlen, buf);
}

int dm_add_mtd_device(struct udevice *dev)
{
	struct mtd_info *mtd = mtd_get_info(dev);

	BUG_ON(mtd->writesize == 0);
	mtd->usecount = 0;

	if (is_power_of_2(mtd->erasesize))
		mtd->erasesize_shift = ffs(mtd->erasesize) - 1;
	else
		mtd->erasesize_shift = 0;

	if (is_power_of_2(mtd->writesize))
		mtd->writesize_shift = ffs(mtd->writesize) - 1;
	else
		mtd->writesize_shift = 0;

	mtd->erasesize_mask = (1 << mtd->erasesize_shift) - 1;
	mtd->writesize_mask = (1 << mtd->writesize_shift) - 1;

	return 0;
}

/*
 * Implement a MTD uclass which should include most flash drivers.
 * The uclass private is pointed to mtd_info.
 */

UCLASS_DRIVER(mtd) = {
	.id		= UCLASS_MTD,
	.name		= "mtd",
	.flags		= DM_UC_FLAG_SEQ_ALIAS,
	.per_device_auto_alloc_size = sizeof(struct mtd_info),
};
