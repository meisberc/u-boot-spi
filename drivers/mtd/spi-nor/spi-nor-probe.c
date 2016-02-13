/*
 * Copyright (c) 2014 Google, Inc
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <dm.h>
#include <spi.h>
#include <spi_flash.h>

/*
 * TODO(sjg@chromium.org): This is an old-style function. We should remove
 * it when all SPI flash drivers use dm
 */
spi_flash_t *spi_flash_probe(unsigned int bus, unsigned int cs,
			     unsigned int max_hz, unsigned int spi_mode)
{
	struct udevice *dev;

	if (spi_flash_probe_bus_cs(bus, cs, max_hz, spi_mode, &dev))
		return NULL;

	return dev_get_uclass_priv(dev);
}

int spi_flash_probe_bus_cs(unsigned int busnum, unsigned int cs,
			   unsigned int max_hz, unsigned int spi_mode,
			   struct udevice **devp)
{
	struct spi_slave *slave;
	struct udevice *bus;
	char name[30], *str;
	int ret;

	snprintf(name, sizeof(name), "spi-nor@%d:%d", busnum, cs);
	str = strdup(name);
	ret = spi_get_bus_and_cs(busnum, cs, max_hz, spi_mode,
				 "m25p80", str, &bus, &slave);
	if (ret)
		return ret;

	*devp = slave->dev;
	return 0;
}
