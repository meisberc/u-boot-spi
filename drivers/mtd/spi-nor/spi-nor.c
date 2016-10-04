/*
 * SPI NOR Core framework.
 *
 * Copyright (C) 2016 Jagan Teki <jagan@openedev.com>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <dm.h>
#include <errno.h>
#include <mapmem.h>
#include <mtd.h>

#include <linux/math64.h>
#include <linux/types.h>
#include <linux/mtd/spi-nor.h>

DECLARE_GLOBAL_DATA_PTR;

/* Set write enable latch with Write Enable command */
static inline int write_enable(struct spi_nor *nor)
{
	return nor->write_reg(nor, SNOR_OP_WREN, NULL, 0);
}

/* Re-set write enable latch with Write Disable command */
static inline int write_disable(struct spi_nor *nor)
{
	return nor->write_reg(nor, SNOR_OP_WRDI, NULL, 0);
}

static int read_sr(struct spi_nor *nor)
{
	u8 sr;
	int ret;

	ret = nor->read_reg(nor, SNOR_OP_RDSR, &sr, 1);
	if (ret < 0) {
		debug("spi-nor: fail to read status register\n");
		return ret;
	}

	return sr;
}

static int read_fsr(struct spi_nor *nor)
{
	u8 fsr;
	int ret;

	ret = nor->read_reg(nor, SNOR_OP_RDFSR, &fsr, 1);
	if (ret < 0) {
		debug("spi-nor: fail to read flag status register\n");
		return ret;
	}

	return fsr;
}

static int write_sr(struct spi_nor *nor, u8 ws)
{
	nor->cmd_buf[0] = ws;
	return nor->write_reg(nor, SNOR_OP_WRSR, nor->cmd_buf, 1);
}

#if defined(CONFIG_SPI_NOR_SPANSION) || defined(CONFIG_SPI_NOR_WINBOND)
static int read_cr(struct spi_nor *nor)
{
	u8 cr;
	int ret;

	ret = nor->read_reg(nor, SNOR_OP_RDCR, &cr, 1);
	if (ret < 0) {
		debug("spi-nor: fail to read config register\n");
		return ret;
	}

	return cr;
}

/*
 * Write status Register and configuration register with 2 bytes
 * - First byte will be written to the status register.
 * - Second byte will be written to the configuration register.
 * Return negative if error occured.
 */
static int write_sr_cr(struct spi_nor *nor, u16 val)
{
	nor->cmd_buf[0] = val & 0xff;
	nor->cmd_buf[1] = (val >> 8);

	return nor->write_reg(nor, SNOR_OP_WRSR, nor->cmd_buf, 2);
}
#endif

static int spi_nor_sr_ready(struct spi_nor *nor)
{
	int sr = read_sr(nor);
	if (sr < 0)
		return sr;
	else
		return !(sr & SR_WIP);
}

static int spi_nor_fsr_ready(struct spi_nor *nor)
{
	int fsr = read_fsr(nor);
	if (fsr < 0)
		return fsr;
	else
		return fsr & FSR_READY;
}

static int spi_nor_ready(struct spi_nor *nor)
{
	int sr, fsr;

	sr = spi_nor_sr_ready(nor);
	if (sr < 0)
		return sr;

	fsr = 1;
	if (nor->flags & SNOR_F_USE_FSR) {
		fsr = spi_nor_fsr_ready(nor);
		if (fsr < 0)
			return fsr;
	}

	return sr && fsr;
}

static int spi_nor_wait_till_ready(struct spi_nor *nor, unsigned long timeout)
{
	int timebase, ret;

	timebase = get_timer(0);

	while (get_timer(timebase) < timeout) {
		ret = spi_nor_ready(nor);
		if (ret < 0)
			return ret;
		if (ret)
			return 0;
	}

	printf("spi-nor: Timeout!\n");

	return -ETIMEDOUT;
}

static const struct spi_nor_info *spi_nor_id(struct spi_nor *nor)
{
	int				tmp;
	u8				id[SPI_NOR_MAX_ID_LEN];
	const struct spi_nor_info	*info;

	tmp = nor->read_reg(nor, SNOR_OP_RDID, id, SPI_NOR_MAX_ID_LEN);
	if (tmp < 0) {
		printf("spi-nor: error %d reading JEDEC ID\n", tmp);
		return ERR_PTR(tmp);
	}

	info = spi_nor_ids;
	for (; info->name != NULL; info++) {
		if (info->id_len) {
			if (!memcmp(info->id, id, info->id_len))
				return info;
		}
	}

	printf("spi-nor: unrecognized JEDEC id bytes: %02x, %2x, %2x\n",
	       id[0], id[1], id[2]);
	return ERR_PTR(-ENODEV);
}

static int spi_nor_erase(struct udevice *dev, struct erase_info *instr)
{
	struct mtd_info *mtd = mtd_get_info(dev);
	struct spi_nor *nor = mtd->priv;
	u32 addr, len, erase_addr;
	uint32_t rem;
	int ret = -1;

	div_u64_rem(instr->len, mtd->erasesize, &rem);
	if (rem)
		return -EINVAL;

	addr = instr->addr;
	len = instr->len;

	while (len) {
		erase_addr = addr;

		write_enable(nor);

		ret = nor->write(nor, erase_addr, 0, NULL);
		if (ret < 0)
			goto erase_err;

		ret = spi_nor_wait_till_ready(nor, SNOR_READY_WAIT_ERASE);
		if (ret < 0)
			goto erase_err;

		addr += mtd->erasesize;
		len -= mtd->erasesize;
	}

	write_disable(nor);

	instr->state = MTD_ERASE_DONE;
	mtd_erase_callback(instr);

	return ret;

erase_err:
	instr->state = MTD_ERASE_FAILED;
	return ret;
}

static int spi_nor_write(struct udevice *dev, loff_t to, size_t len,
			 size_t *retlen, const u_char *buf)
{
	struct mtd_info *mtd = mtd_get_info(dev);
	struct spi_nor *nor = mtd->priv;
	size_t addr, byte_addr;
	size_t chunk_len, actual;
	uint32_t page_size;
	int ret = -1;

	page_size = mtd->writebufsize;

	for (actual = 0; actual < len; actual += chunk_len) {
		addr = to;

		byte_addr = addr % page_size;
		chunk_len = min(len - actual, (size_t)(page_size - byte_addr));

		if (nor->max_write_size)
			chunk_len = min(chunk_len,
					(size_t)nor->max_write_size);

		write_enable(nor);

		ret = nor->write(nor, addr, chunk_len, buf + actual);
		if (ret < 0)
			break;

		ret = spi_nor_wait_till_ready(nor, SNOR_READY_WAIT_PROG);
		if (ret < 0)
			return ret;

		to += chunk_len;
		*retlen += chunk_len;
	}

	return ret;
}

static int spi_nor_read(struct udevice *dev, loff_t from, size_t len,
			size_t *retlen, u_char *buf)
{
	struct mtd_info *mtd = mtd_get_info(dev);
	struct spi_nor *nor = mtd->priv;
	int ret;

	/* Handle memory-mapped SPI */
	if (nor->memory_map) {
		ret = nor->read(nor, from, len, buf);
		if (ret) {
			debug("spi-nor: mmap read failed\n");
			return ret;
		}

		return ret;
	}

	ret = nor->read(nor, from, len, buf);
	if (ret < 0)
		return ret;

	*retlen += len;

	return ret;
}

#ifdef CONFIG_SPI_NOR_SST
static int sst_byte_write(struct spi_nor *nor, u32 addr, const void *buf,
			  size_t *retlen)
{
	int ret;

	ret = write_enable(nor);
	if (ret)
		return ret;

	nor->program_opcode = SNOR_OP_BP;

	ret = nor->write(nor, addr, 1, buf);
	if (ret)
		return ret;

	*retlen += 1;

	return spi_nor_wait_till_ready(nor, SNOR_READY_WAIT_PROG);
}

static int sst_write_wp(struct udevice *dev, loff_t to, size_t len,
			size_t *retlen, const u_char *buf)
{
	struct mtd_info *mtd = mtd_get_info(dev);
	struct spi_nor *nor = mtd->priv;
	size_t actual;
	int ret;

	/* If the data is not word aligned, write out leading single byte */
	actual = to % 2;
	if (actual) {
		ret = sst_byte_write(nor, to, buf, retlen);
		if (ret)
			goto done;
	}
	to += actual;

	ret = write_enable(nor);
	if (ret)
		goto done;

	for (; actual < len - 1; actual += 2) {
		nor->program_opcode = SNOR_OP_AAI_WP;

		ret = nor->write(nor, to, 2, buf + actual);
		if (ret) {
			debug("spi-nor: sst word program failed\n");
			break;
		}

		ret = spi_nor_wait_till_ready(nor, SNOR_READY_WAIT_PROG);
		if (ret)
			break;

		to += 2;
		*retlen += 2;
	}

	if (!ret)
		ret = write_disable(nor);

	/* If there is a single trailing byte, write it out */
	if (!ret && actual != len)
		ret = sst_byte_write(nor, to, buf + actual, retlen);

 done:
	return ret;
}

static int sst_write_bp(struct udevice *dev, loff_t to, size_t len,
			size_t *retlen, const u_char *buf)
{
	struct mtd_info *mtd = mtd_get_info(dev);
	struct spi_nor *nor = mtd->priv;
	size_t actual;
	int ret;

	for (actual = 0; actual < len; actual++) {
		ret = sst_byte_write(nor, to, buf + actual, retlen);
		if (ret) {
			debug("spi-nor: sst byte program failed\n");
			break;
		}
		to++;
	}

	if (!ret)
		ret = write_disable(nor);

	return ret;
}
#endif

#ifdef CONFIG_SPI_NOR_MACRONIX
static int macronix_quad_enable(struct spi_nor *nor)
{
	int ret, val;

	val = read_sr(nor);
	if (val < 0)
		return val;

	if (val & SR_QUAD_EN_MX)
		return 0;

	write_enable(nor);

	ret = write_sr(nor, val | SR_QUAD_EN_MX);
	if (ret < 0)
		return ret;

	if (spi_nor_wait_till_ready(nor, SNOR_READY_WAIT_PROG))
		return 1;

	ret = read_sr(nor);
	if (!(ret > 0 && (ret & SR_QUAD_EN_MX))) {
		printf("spi-nor: Macronix Quad bit not set\n");
		return -EINVAL;
	}

	return 0;
}
#endif

#if defined(CONFIG_SPI_NOR_SPANSION) || defined(CONFIG_SPI_NOR_WINBOND)
static int spansion_quad_enable(struct spi_nor *nor)
{
	int ret, val;

	val = read_cr(nor);
	if (val < 0)
		return val;

	if (val & CR_QUAD_EN_SPAN)
		return 0;

	write_enable(nor);

	ret = write_sr_cr(nor, val | CR_QUAD_EN_SPAN);
	if (ret < 0)
		return ret;

	if (spi_nor_wait_till_ready(nor, SNOR_READY_WAIT_PROG))
		return 1;

	/* read back and check it */
	ret = read_cr(nor);
	if (!(ret > 0 && (ret & CR_QUAD_EN_SPAN))) {
		printf("spi-nor: Spansion Quad bit not set\n");
		return -EINVAL;
	}

	return 0;
}
#endif

static int set_quad_mode(struct spi_nor *nor, const struct spi_nor_info *info)
{
	switch (JEDEC_MFR(info)) {
#ifdef CONFIG_SPI_NOR_MACRONIX
	case SNOR_MFR_MACRONIX:
		return macronix_quad_enable(nor);
#endif
#if defined(CONFIG_SPI_NOR_SPANSION) || defined(CONFIG_SPI_NOR_WINBOND)
	case SNOR_MFR_SPANSION:
	case SNOR_MFR_WINBOND:
		return spansion_quad_enable(nor);
#endif
#ifdef CONFIG_SPI_NOR_STMICRO
	case SNOR_MFR_MICRON:
		return 0;
#endif
	default:
		printf("spi-nor: Need set QEB func for %02x flash\n",
		       JEDEC_MFR(info));
		return -1;
	}
}

#if CONFIG_IS_ENABLED(OF_CONTROL)
int spi_nor_decode_fdt(const void *blob, struct spi_nor *nor)
{
	struct udevice *dev = nor->dev;
	struct mtd_info *mtd = mtd_get_info(dev);
	fdt_addr_t addr;
	fdt_size_t size;
	int node;

	/* If there is no node, do nothing */
	node = fdtdec_next_compatible(blob, 0, COMPAT_GENERIC_SPI_FLASH);
	if (node < 0)
		return 0;

	addr = fdtdec_get_addr_size(blob, node, "memory-map", &size);
	if (addr == FDT_ADDR_T_NONE) {
		debug("%s: Cannot decode address\n", __func__);
		return 0;
	}

	if (mtd->size != size) {
		debug("%s: Memory map must cover entire device\n", __func__);
		return -1;
	}
	nor->memory_map = map_sysmem(addr, size);

	return 0;
}
#endif /* CONFIG_IS_ENABLED(OF_CONTROL) */

static int spi_nor_check(struct spi_nor *nor)
{
	if (!nor->read || !nor->write ||
	    !nor->read_reg || !nor->write_reg) {
		pr_err("spi-nor: please fill all the necessary fields!\n");
		return -EINVAL;
	}

	return 0;
}

int spi_nor_scan(struct udevice *dev)
{
	struct mtd_info *mtd = mtd_get_info(dev);
	struct spi_nor *nor = mtd->priv;
	struct dm_mtd_ops *ops = mtd_get_ops(dev);
	const struct spi_nor_info *info = NULL;
	int ret;

	ret = spi_nor_check(nor);
	if (ret)
		return ret;

	info = spi_nor_id(nor);
	if (IS_ERR_OR_NULL(info))
		return -ENOENT;

	/*
	 * Atmel, SST, Macronix, and others serial NOR tend to power up
	 * with the software protection bits set
	 */
	if (JEDEC_MFR(info) == SNOR_MFR_ATMEL ||
	    JEDEC_MFR(info) == SNOR_MFR_MACRONIX ||
	    JEDEC_MFR(info) == SNOR_MFR_SST) {
		write_enable(nor);
		write_sr(nor, 0);
	}

	mtd->name = info->name;
	mtd->priv = nor;
	mtd->type = MTD_NORFLASH;
	mtd->writesize = 1;
	mtd->flags = MTD_CAP_NORFLASH;
	ops->_erase = spi_nor_erase;
	ops->_read = spi_nor_read;

	if (info->flags & E_FSR)
		nor->flags |= SNOR_F_USE_FSR;

	if (info->flags & SST_WR)
		nor->flags |= SNOR_F_SST_WRITE;

	ops->_write = spi_nor_write;
#if defined(CONFIG_SPI_NOR_SST)
	if (nor->flags & SNOR_F_SST_WRITE) {
		if (nor->mode & SNOR_WRITE_1_1_BYTE)
			ops->_write = sst_write_bp;
		else
			ops->_write = sst_write_wp;
	}
#endif

	/* Compute the flash size */
	nor->page_size = info->page_size;
	/*
	 * The Spansion S25FL032P and S25FL064P have 256b pages, yet use the
	 * 0x4d00 Extended JEDEC code. The rest of the Spansion flashes with
	 * the 0x4d00 Extended JEDEC code have 512b pages. All of the others
	 * have 256b pages.
	 */
	if (JEDEC_EXT(info) == 0x4d00) {
		if ((JEDEC_ID(info) != 0x0215) &&
		    (JEDEC_ID(info) != 0x0216))
			nor->page_size = 512;
	}
	mtd->writebufsize = nor->page_size;
	mtd->size = info->sector_size * info->n_sectors;

#ifdef CONFIG_MTD_SPI_NOR_USE_4K_SECTORS
	/* prefer "small sector" erase if possible */
	if (info->flags & SECT_4K) {
		nor->erase_opcode = SNOR_OP_BE_4K;
		mtd->erasesize = 4096;
	} else
#endif
	{
		nor->erase_opcode = SNOR_OP_SE;
		mtd->erasesize = info->sector_size;
	}

	/* Look for read opcode */
	nor->read_opcode = SNOR_OP_READ_FAST;
	if (nor->mode & SNOR_READ)
		nor->read_opcode = SNOR_OP_READ;
	else if (nor->mode & SNOR_READ_1_1_4 && info->flags & RD_QUAD)
		nor->read_opcode = SNOR_OP_READ_1_1_4;
	else if (nor->mode & SNOR_READ_1_1_2 && info->flags & RD_DUAL)
		nor->read_opcode = SNOR_OP_READ_1_1_2;

	/* Look for program opcode */
	if (info->flags & WR_QPP && nor->mode & SNOR_WRITE_1_1_4)
		nor->program_opcode = SNOR_OP_QPP;
	else
		/* Go for default supported write cmd */
		nor->program_opcode = SNOR_OP_PP;

	/* Set the quad enable bit - only for quad commands */
	if ((nor->read_opcode == SNOR_OP_READ_1_1_4) ||
	    (nor->read_opcode == SNOR_OP_READ_1_1_4_IO) ||
	    (nor->program_opcode == SNOR_OP_QPP)) {
		ret = set_quad_mode(nor, info);
		if (ret) {
			debug("spi-nor: quad mode not supported for %02x\n",
			      JEDEC_MFR(info));
			return ret;
		}
	}

	nor->addr_width = 3;

	/* Dummy cycles for read */
	switch (nor->read_opcode) {
	case SNOR_OP_READ_1_1_4_IO:
		nor->read_dummy = 16;
		break;
	case SNOR_OP_READ:
		nor->read_dummy = 0;
		break;
	default:
		nor->read_dummy = 8;
	}

#if CONFIG_IS_ENABLED(OF_CONTROL)
	ret = spi_nor_decode_fdt(gd->fdt_blob, nor);
	if (ret) {
		debug("spi-nor: FDT decode error\n");
		return -EINVAL;
	}
#endif

#ifndef CONFIG_SPL_BUILD
	printf("SPI-NOR: detected %s with page size ", mtd->name);
	print_size(mtd->writebufsize, ", erase size ");
	print_size(mtd->erasesize, ", total ");
	print_size(mtd->size, "");
	if (nor->memory_map)
		printf(", mapped at %p", nor->memory_map);
	puts("\n");
#endif

	return ret;
}
