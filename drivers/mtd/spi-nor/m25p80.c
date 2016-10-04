/*
 * MTD SPI-NOR driver for ST M25Pxx (and similar) serial flash chips
 *
 * Copyright (C) 2016 Jagan Teki <jagan@openedev.com>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <dm.h>
#include <dma.h>
#include <errno.h>
#include <mtd.h>
#include <spi.h>

#include <dm/device-internal.h>

#include <linux/mtd/mtd.h>
#include <linux/mtd/spi-nor.h>

#define MAX_CMD_SIZE		6
struct m25p {
	struct spi_slave	*spi;
	struct spi_nor		spi_nor;
	u8			command[MAX_CMD_SIZE];
};

static void m25p_addr2cmd(struct spi_nor *nor, unsigned int addr, u8 *cmd)
{
	/* opcode is in cmd[0] */
	cmd[1] = addr >> (nor->addr_width * 8 -  8);
	cmd[2] = addr >> (nor->addr_width * 8 - 16);
	cmd[3] = addr >> (nor->addr_width * 8 - 24);
}

static int m25p_cmdsz(struct spi_nor *nor)
{
	return 1 + nor->addr_width;
}

static int m25p80_read_reg(struct spi_nor *nor, u8 opcode, u8 *val, int len)
{
	struct m25p *flash = nor->priv;
	struct spi_slave *spi = flash->spi;
	int ret;

	ret = spi_write_then_read(spi, &opcode, 1, NULL, val, len);
	if (ret < 0) {
		debug("m25p80: error %d reading register %x\n", ret, opcode);
		return ret;
	}

	return ret;
}

static int m25p80_write_reg(struct spi_nor *nor, u8 opcode, u8 *buf, int len)
{
	struct m25p *flash = nor->priv;
	struct spi_slave *spi = flash->spi;
	int ret;

	ret = spi_write_then_read(spi, &opcode, 1, buf, NULL, len);
	if (ret < 0) {
		debug("m25p80: error %d writing register %x\n", ret, opcode);
		return ret;
	}

	return ret;
}

/*
 * TODO: remove the weak after all the other spi_flash_copy_mmap
 * implementations removed from drivers
 */
void __weak flash_copy_mmap(void *data, void *offset, size_t len)
{
#ifdef CONFIG_DMA
	if (!dma_memcpy(data, offset, len))
		return;
#endif
	memcpy(data, offset, len);
}

static int m25p80_read(struct spi_nor *nor, loff_t from, size_t len,
		       u_char *buf)
{
	struct m25p *flash = nor->priv;
	struct spi_slave *spi = flash->spi;
	unsigned int dummy = nor->read_dummy;
	int ret;

	if (nor->memory_map) {
		spi_xfer(spi, 0, NULL, NULL, SPI_XFER_MMAP);
		flash_copy_mmap(buf, nor->memory_map + from, len);
		spi_xfer(spi, 0, NULL, NULL, SPI_XFER_MMAP_END);
		spi_release_bus(spi);
		return 0;
	}

	/* convert the dummy cycles to the number of bytes */
	dummy /= 8;

	flash->command[0] = nor->read_opcode;
	m25p_addr2cmd(nor, from, flash->command);

	ret = spi_write_then_read(spi, flash->command, m25p_cmdsz(nor) + dummy,
				  NULL, buf, len);
	if (ret < 0) {
		debug("m25p80: error %d reading %x\n", ret, flash->command[0]);
		return ret;
	}

	return ret;
}

static int m25p80_write(struct spi_nor *nor, loff_t to, size_t len,
			const u_char *buf)
{
	struct m25p *flash = nor->priv;
	struct spi_slave *spi = flash->spi;
	int cmd_sz = m25p_cmdsz(nor);
	int ret;

	if ((nor->program_opcode == SNOR_OP_AAI_WP) && (buf != NULL))
		cmd_sz = 1;

	flash->command[0] = nor->program_opcode;
	if (buf == NULL)
		flash->command[0] = nor->erase_opcode;
	m25p_addr2cmd(nor, to, flash->command);

	ret = spi_write_then_read(spi, flash->command, cmd_sz, buf, NULL, len);
	if (ret < 0) {
		debug("m25p80: error %d writing %x\n", ret, flash->command[0]);
		return ret;
	}

	return ret;
}

static int m25p_probe(struct udevice *dev)
{
	struct mtd_info *mtd = mtd_get_info(dev);
	struct spi_slave *spi = dev_get_parent_priv(dev);
	struct m25p *flash = dev_get_priv(dev);
	struct spi_nor *nor;
	int ret;

	nor = &flash->spi_nor;

	flash->spi = spi;
	nor->priv = flash;
	mtd->priv = nor;
	nor->dev = dev;

	/* install hooks */
	nor->read = m25p80_read;
	nor->write = m25p80_write;
	nor->read_reg = m25p80_read_reg;
	nor->write_reg = m25p80_write_reg;

	/* claim spi bus */
	ret = spi_claim_bus(spi);
	if (ret) {
		debug("m25p80: failed to claim SPI bus: %d\n", ret);
		return ret;
	}

	if (spi->mode & SPI_RX_SLOW)
		nor->mode = SNOR_READ;
	else if (spi->mode & SPI_RX_DUAL)
		nor->mode = SNOR_READ_1_1_2;
	else if (spi->mode & SPI_RX_QUAD)
		nor->mode = SNOR_READ_1_1_4;

	if (spi->mode & SPI_TX_BYTE)
		nor->mode |= SNOR_WRITE_1_1_BYTE;
	else if (spi->mode & SPI_TX_QUAD)
		nor->mode |= SNOR_WRITE_1_1_4;

	nor->memory_map = spi->memory_map;
	nor->max_write_size = spi->max_write_size;

	ret = spi_nor_scan(dev);
	if (ret)
		goto err_scan;

	ret = dm_add_mtd_device(dev);
	if (ret)
		goto err_mtd;

	return 0;

err_mtd:
	device_remove(dev);
	spi_free_slave(spi);
err_scan:
	spi_release_bus(spi);
	return ret;
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
	.priv_auto_alloc_size = sizeof(struct m25p),
};
