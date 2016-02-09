/*
 * MTD SPI-NOR driver for ST M25Pxx (and similar) serial flash chips
 *
 * Copyright (C) 2016 Jagan Teki <jteki@openedev.com>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <dm.h>
#include <errno.h>
#include <spi.h>
#include <linux/mtd/mtd.h>

static int m25p_probe(struct udevice *dev)
{
	struct spi_slave *spi = dev_get_parent_priv(dev);
	struct mtd_info	*mtd = dev_get_uclass_priv(dev);

	return 0;
}

static const struct udevice_id m25p_ids[] = {
	/*
	 * Generic compatibility for SPI NOR that can be identified by the
	 * JEDEC READ ID opcode (0x9F). Use this, if possible.
	 */
	{ .compatible = "jedec,spi-nor" },
	{ }
};

U_BOOT_DRIVER(m25p80) = {
	.name		= "m25p80",
	.id		= UCLASS_MTD,
	.of_match	= m25p_ids,
	.probe		= m25p_probe,
};
