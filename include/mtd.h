/*
 * Copyright (C) 2015 Thomas Chou <thomas@wytron.com.tw>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#ifndef _MTD_H_
#define _MTD_H_

#include <linux/mtd/mtd.h>

/*
 * Get mtd_info structure of the dev, which is stored as uclass private.
 *
 * @dev: The MTD device
 * @return: pointer to mtd_info, NULL on error
 */
static inline struct mtd_info *mtd_get_info(struct udevice *dev)
{
	return dev_get_uclass_priv(dev);
}

struct dm_mtd_ops {
	int (*_erase)(struct udevice *dev, struct erase_info *instr);
	int (*_read)(struct udevice *dev, loff_t from, size_t len,
		     size_t *retlen, u_char *buf);
	int (*_write)(struct udevice *dev, loff_t to, size_t len,
		      size_t *retlen, const u_char *buf);
};

/* Access the serial operations for a device */
#define mtd_get_ops(dev) ((struct dm_mtd_ops *)(dev)->driver->ops)

/**
 * dm_mtd_read() - Read data from MTD device
 *
 * @dev:	MTD device
 * @from:	Offset into device in bytes to read from
 * @len:	Length of bytes to read
 * @retlen:	Length of return bytes read to
 * @buf:	Buffer to put the data that is read
 * @return 0 if OK, -ve on error
 */
int dm_mtd_read(struct udevice *dev, loff_t from, size_t len, size_t *retlen,
		u_char *buf);

/**
 * dm_mtd_write() - Write data to MTD device
 *
 * @dev:	MTD device
 * @to:		Offset into device in bytes to write to
 * @len:	Length of bytes to write
 * @retlen:	Length of return bytes to write to
 * @buf:	Buffer containing bytes to write
 * @return 0 if OK, -ve on error
 */
int dm_mtd_write(struct udevice *dev, loff_t to, size_t len, size_t *retlen,
		 const u_char *buf);

/**
 * dm_mtd_erase() - Erase blocks of the MTD device
 *
 * @dev:	MTD device
 * @instr:	Erase info details of MTD device
 * @return 0 if OK, -ve on error
 */
int dm_mtd_erase(struct udevice *dev, struct erase_info *instr);

/**
 * dm_add_mtd_device() - Add MTD device
 *
 * @dev:	MTD device
 * @return 0 if OK, -ve on error
 */
int dm_add_mtd_device(struct udevice *dev);

/**
 * dm_mtd_probe() - Probe MTD device
 *
 * @dev:	MTD device
 * @devp:	MTD device pointer
 * @return 0 if OK, -ve on error
 */
int dm_mtd_probe(struct udevice *dev, struct udevice **devp);

#endif	/* _MTD_H_ */
