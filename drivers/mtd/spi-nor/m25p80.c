/*
 * MTD SPI-NOR driver for ST M25Pxx (and similar) serial flash chips
 *
 * Copyright (C) 2016 Jagan Teki <jteki@openedev.com>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <dm.h>
#include <dma.h>
#include <errno.h>
#include <spi.h>

#include <dm/device-internal.h>

#include <linux/mtd/mtd.h>
#include <linux/mtd/spi-nor.h>

struct m25p {
	struct spi_slave	*spi;
	struct spi_nor		spi_nor;
#ifndef CONFIG_DM_MTD_SPI_NOR
	struct mtd_info		mtd;
#endif
};

static int spi_read_then_write(struct spi_slave *spi, const u8 *cmd,
			       size_t cmd_len, const u8 *data_out,
			       u8 *data_in, size_t data_len)
{
	unsigned long flags = SPI_XFER_BEGIN;
	int ret;

	if (data_len == 0)
		flags |= SPI_XFER_END;

	ret = spi_xfer(spi, cmd_len * 8, cmd, NULL, flags);
	if (ret) {
		debug("SF: Failed to send command (%zu bytes): %d\n",
		      cmd_len, ret);
	} else if (data_len != 0) {
		ret = spi_xfer(spi, data_len * 8, data_out, data_in,
					SPI_XFER_END);
		if (ret)
			debug("SF: Failed to transfer %zu bytes of data: %d\n",
			      data_len, ret);
	}

	return ret;
}

static int m25p80_read_reg(struct spi_nor *nor, u8 cmd, u8 *val, int len)
{
	struct m25p *flash = nor->priv;
	struct spi_slave *spi = flash->spi;
	int ret;

	ret = spi_claim_bus(spi);
	if (ret < 0) {
		debug("m25p80: unable to claim SPI bus\n");
		return ret;
	}

	if (nor->flags & SNOR_F_U_PAGE)
		spi->flags |= SPI_XFER_U_PAGE;

	ret = spi_read_then_write(spi, &cmd, 1, NULL, val, len);
	if (ret < 0) {
		debug("m25p80: error %d reading register %x\n", ret, cmd);
		return ret;
	}

	spi_release_bus(spi);

	return ret;
}

static int m25p80_write_reg(struct spi_nor *nor, u8 cmd, u8 *buf, int len)
{
	struct m25p *flash = nor->priv;
	struct spi_slave *spi = flash->spi;
	int ret;

	ret = spi_claim_bus(spi);
	if (ret < 0) {
		debug("m25p80: unable to claim SPI bus\n");
		return ret;
	}

	if (nor->flags & SNOR_F_U_PAGE)
		spi->flags |= SPI_XFER_U_PAGE;

	ret = spi_read_then_write(spi, &cmd, 1, buf, NULL, len);
	if (ret < 0) {
		debug("m25p80: error %d writing register %x\n", ret, cmd);
		return ret;
	}

	spi_release_bus(spi);

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

static int m25p80_read_mmap(struct spi_nor *nor, void *data,
			    void *offset, size_t len)
{
	struct m25p *flash = nor->priv;
	struct spi_slave *spi = flash->spi;
	int ret;

	ret = spi_claim_bus(spi);
	if (ret) {
		debug("m25p80: unable to claim SPI bus\n");
		return ret;
	}

	spi_xfer(spi, 0, NULL, NULL, SPI_XFER_MMAP);
	flash_copy_mmap(data, offset, len);
	spi_xfer(spi, 0, NULL, NULL, SPI_XFER_MMAP_END);

	spi_release_bus(spi);

	return ret;
}

static int m25p80_read(struct spi_nor *nor, const u8 *cmd, size_t cmd_len,
				void *data, size_t data_len)
{
	struct m25p *flash = nor->priv;
	struct spi_slave *spi = flash->spi;
	int ret;

	ret = spi_claim_bus(spi);
	if (ret < 0) {
		debug("m25p80: unable to claim SPI bus\n");
		return ret;
	}

	if (nor->flags & SNOR_F_U_PAGE)
		spi->flags |= SPI_XFER_U_PAGE;

	ret = spi_read_then_write(spi, cmd, cmd_len, NULL, data, data_len);
	if (ret < 0) {
		debug("m25p80: error %d reading %x\n", ret, *cmd);
		return ret;
	}

	spi_release_bus(spi);

	return ret;
}

static int m25p80_write(struct spi_nor *nor, const u8 *cmd, size_t cmd_len,
				const void *data, size_t data_len)
{
	struct m25p *flash = nor->priv;
	struct spi_slave *spi = flash->spi;
	int ret;

	ret = spi_claim_bus(spi);
	if (ret < 0) {
		debug("m25p80: unable to claim SPI bus\n");
		return ret;
	}

	if (nor->flags & SNOR_F_U_PAGE)
		spi->flags |= SPI_XFER_U_PAGE;

	ret = spi_read_then_write(spi, cmd, cmd_len, data, NULL, data_len);
	if (ret < 0) {
		debug("m25p80: error %d writing %x\n", ret, *cmd);
		return ret;
	}

	spi_release_bus(spi);

	return ret;
}

static int m25p80_spi_nor(struct spi_nor *nor)
{
	struct mtd_info *mtd = nor->mtd;
	struct m25p *flash = nor->priv;
	struct spi_slave *spi = flash->spi;
	int ret;

	/* install hooks */
	nor->read_mmap = m25p80_read_mmap;
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

	switch (spi->mode_rx) {
	case SPI_RX_SLOW:
		nor->read_mode = SNOR_READ;
		break;
	case SPI_RX_DUAL:
		nor->read_mode = SNOR_READ_1_1_2;
		break;
	case SPI_RX_QUAD:
		nor->read_mode = SNOR_READ_1_1_4;
		break;
	}

	switch (spi->mode) {
	case SPI_TX_BYTE:
		nor->mode = SNOR_WRITE_1_1_BYTE;
		break;
	case SPI_TX_QUAD:
		nor->mode = SNOR_WRITE_1_1_4;
		break;
	}

	nor->memory_map = spi->memory_map;
	nor->max_write_size = spi->max_write_size;

	/* TODO: unrelated to spi_slave{} */
	if (spi->option & SPI_CONN_DUAL_SHARED)
		nor->dual = SNOR_DUAL_STACKED;
	else if (spi->option & SPI_CONN_DUAL_SEPARATED)
		nor->dual = SNOR_DUAL_PARALLEL;

	ret = spi_nor_scan(nor);
	if (ret)
		goto err_scan;

	ret = add_mtd_device(mtd);
	if (ret)
		goto err_mtd;

	return 0;

err_scan:
	spi_release_bus(spi);
err_mtd:
	spi_free_slave(spi);
	return ret;
}

#ifdef CONFIG_DM_MTD_SPI_NOR
static int m25p_probe(struct udevice *dev)
{
	struct spi_slave *spi = dev_get_parent_priv(dev);
	struct mtd_info	*mtd = dev_get_uclass_priv(dev);
	struct m25p *flash = dev_get_priv(dev);
	struct spi_nor *nor;
	int ret;

	nor = &flash->spi_nor;

	nor->mtd = mtd;
	nor->priv = flash;
	flash->spi = spi;

	ret = m25p80_spi_nor(nor);
	if (ret) {
		device_remove(dev);
		return ret;
	}

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
	.priv_auto_alloc_size = sizeof(struct m25p),
};

#else

static struct mtd_info *m25p80_probe_tail(struct spi_slave *bus)
{
	struct m25p *flash;
	struct spi_nor *nor;
	int ret;

	flash = calloc(1, sizeof(*flash));
	if (!flash) {
		debug("mp25p80: failed to allocate m25p\n");
		return NULL;
	}

	nor = &flash->spi_nor;
	nor->mtd = &flash->mtd;

	nor->priv = flash;
	flash->spi = bus;

	ret = m25p80_spi_nor(nor);
	if (ret) {
		free(flash);
		return NULL;
	}

	return nor->mtd;
}

struct mtd_info *spi_flash_probe(unsigned int busnum, unsigned int cs,
				 unsigned int max_hz, unsigned int spi_mode)
{
	struct spi_slave *bus;

	bus = spi_setup_slave(busnum, cs, max_hz, spi_mode);
	if (!bus)
		return NULL;
	return m25p80_probe_tail(bus);
}

#ifdef CONFIG_OF_SPI_FLASH
struct mtd_info *spi_flash_probe_fdt(const void *blob, int slave_node,
				     int spi_node)
{
	struct spi_slave *bus;

	bus = spi_setup_slave_fdt(blob, slave_node, spi_node);
	if (!bus)
		return NULL;
	return m25p80_probe_tail(bus);
}
#endif

void spi_flash_free(struct mtd_info *info)
{
	struct spi_nor *nor = info->priv;
	struct m25p *flash = nor->priv;

	del_mtd_device(info);
	spi_free_slave(flash->spi);
	free(flash);
}

#endif /* CONFIG_DM_MTD_SPI_NOR */
