/*
 * SPI NOR Core - cloned most of the code from the spi_flash.c
 *
 * Copyright (C) 2016 Jagan Teki <jteki@openedev.com>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <div64.h>
#include <dm.h>
#include <errno.h>
#include <malloc.h>
#include <mapmem.h>

#include <linux/math64.h>
#include <linux/log2.h>
#include <linux/mtd/mtd.h>
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

static void spi_nor_addr(u32 addr, u8 *cmd)
{
	/* cmd[0] is actual command */
	cmd[1] = addr >> 16;
	cmd[2] = addr >> 8;
	cmd[3] = addr >> 0;
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

#if defined(CONFIG_SPI_FLASH_SPANSION) || defined(CONFIG_SPI_FLASH_WINBOND)
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

#ifdef CONFIG_SPI_FLASH_STMICRO
static int read_evcr(struct spi_nor *nor)
{
	u8 evcr;
	int ret;

	ret = nor->read_reg(nor, SPINOR_OP_RD_EVCR, &evcr, 1);
	if (ret < 0) {
		debug("spi-nor: fail to read EVCR\n");
		return ret;
	}

	return evcr;
}

static int write_evcr(struct spi_nor *nor, u8 evcr)
{
	nor->cmd_buf[0] = evcr;
	return nor->write_reg(nor, SPINOR_OP_WD_EVCR, nor->cmd_buf, 1);
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

#ifdef CONFIG_SPI_NOR_BAR
static int spi_nor_write_bar(struct spi_nor *nor, u32 offset)
{
	u8 bank_sel;
	int ret;

	bank_sel = offset / (SNOR_16MB_BOUN << nor->shift);
	if (bank_sel == nor->bank_curr)
		goto bar_end;

	write_enable(nor);

	nor->cmd_buf[0] = bank_sel;
	ret = nor->write_reg(nor, nor->bar_program_opcode, nor->cmd_buf, 1);
	if (ret < 0) {
		debug("spi-nor: fail to write bank register\n");
		return ret;
	}

	ret = spi_nor_wait_till_ready(nor, SNOR_READY_WAIT_PROG);
	if (ret < 0)
		return ret;

bar_end:
	nor->bank_curr = bank_sel;
	return nor->bank_curr;
}

static int spi_nor_read_bar(struct spi_nor *nor, const struct spi_nor_info *info)
{
	struct mtd_info *mtd = nor->mtd;
	u8 curr_bank = 0;
	int ret;

	if (mtd->size <= SNOR_16MB_BOUN)
		goto bar_end;

	switch (JEDEC_MFR(info)) {
	case SNOR_MFR_SPANSION:
		nor->bar_read_opcode = SNOR_OP_BRRD;
		nor->bar_program_opcode = SNOR_OP_BRWR;
		break;
	default:
		nor->bar_read_opcode = SNOR_OP_RDEAR;
		nor->bar_program_opcode = SNOR_OP_WREAR;
	}

	ret = nor->read_reg(nor, nor->bar_read_opcode, &curr_bank, 1);
	if (ret) {
		debug("spi-nor: fail to read bank addr register\n");
		return ret;
	}

bar_end:
	nor->bank_curr = curr_bank;
	return 0;
}
#endif

#ifdef CONFIG_SF_DUAL_FLASH
static void spi_nor_dual(struct spi_nor *nor, u32 *addr)
{
	struct mtd_info *mtd = nor->mtd;

	switch (nor->dual) {
	case SNOR_DUAL_STACKED:
		if (*addr >= (mtd->size >> 1)) {
			*addr -= mtd->size >> 1;
			nor->flags |= SNOR_F_U_PAGE;
		} else {
			nor->flags &= ~SNOR_F_U_PAGE;
		}
		break;
	case SNOR_DUAL_PARALLEL:
		*addr >>= nor->shift;
		break;
	default:
		debug("spi-nor: Unsupported dual_flash=%d\n", nor->dual);
		break;
	}
}
#endif

#if defined(CONFIG_SPI_FLASH_STMICRO) || defined(CONFIG_SPI_FLASH_SST)
static void stm_get_locked_range(struct spi_nor *nor, u8 sr, loff_t *ofs,
				 uint64_t *len)
{
	struct mtd_info *mtd = nor->mtd;
	u8 mask = SR_BP2 | SR_BP1 | SR_BP0;
	int shift = ffs(mask) - 1;
	int pow;

	if (!(sr & mask)) {
		/* No protection */
		*ofs = 0;
		*len = 0;
	} else {
		pow = ((sr & mask) ^ mask) >> shift;
		*len = mtd->size >> pow;
		*ofs = mtd->size - *len;
	}
}

/*
 * Return 1 if the entire region is locked, 0 otherwise
 */
static int stm_is_locked_sr(struct spi_nor *nor, loff_t ofs, uint64_t len,
			    u8 sr)
{
	loff_t lock_offs;
	uint64_t lock_len;

	stm_get_locked_range(nor, sr, &lock_offs, &lock_len);

	return (ofs + len <= lock_offs + lock_len) && (ofs >= lock_offs);
}

/*
 * Lock a region of the flash. Compatible with ST Micro and similar flash.
 * Supports only the block protection bits BP{0,1,2} in the status register
 * (SR). Does not support these features found in newer SR bitfields:
 *   - TB: top/bottom protect - only handle TB=0 (top protect)
 *   - SEC: sector/block protect - only handle SEC=0 (block protect)
 *   - CMP: complement protect - only support CMP=0 (range is not complemented)
 *
 * Sample table portion for 8MB flash (Winbond w25q64fw):
 *
 *   SEC  |  TB   |  BP2  |  BP1  |  BP0  |  Prot Length  | Protected Portion
 *  --------------------------------------------------------------------------
 *    X   |   X   |   0   |   0   |   0   |  NONE         | NONE
 *    0   |   0   |   0   |   0   |   1   |  128 KB       | Upper 1/64
 *    0   |   0   |   0   |   1   |   0   |  256 KB       | Upper 1/32
 *    0   |   0   |   0   |   1   |   1   |  512 KB       | Upper 1/16
 *    0   |   0   |   1   |   0   |   0   |  1 MB         | Upper 1/8
 *    0   |   0   |   1   |   0   |   1   |  2 MB         | Upper 1/4
 *    0   |   0   |   1   |   1   |   0   |  4 MB         | Upper 1/2
 *    X   |   X   |   1   |   1   |   1   |  8 MB         | ALL
 *
 * Returns negative on errors, 0 on success.
 */
static int stm_lock(struct spi_nor *nor, loff_t ofs, uint64_t len)
{
	struct mtd_info *mtd = nor->mtd;
	int status_old, status_new;
	u8 mask = SR_BP2 | SR_BP1 | SR_BP0;
	u8 shift = ffs(mask) - 1, pow, val;

	status_old = read_sr(nor);
	if (status_old < 0)
		return status_old;

	/* SPI NOR always locks to the end */
	if (ofs + len != mtd->size) {
		/* Does combined region extend to end? */
		if (!stm_is_locked_sr(nor, ofs + len, mtd->size - ofs - len,
				      status_old))
			return -EINVAL;
		len = mtd->size - ofs;
	}

	/*
	 * Need smallest pow such that:
	 *
	 *   1 / (2^pow) <= (len / size)
	 *
	 * so (assuming power-of-2 size) we do:
	 *
	 *   pow = ceil(log2(size / len)) = log2(size) - floor(log2(len))
	 */
	pow = ilog2(mtd->size) - ilog2(len);
	val = mask - (pow << shift);
	if (val & ~mask)
		return -EINVAL;
	/* Don't "lock" with no region! */
	if (!(val & mask))
		return -EINVAL;

	status_new = (status_old & ~mask) | val;

	/* Only modify protection if it will not unlock other areas */
	if ((status_new & mask) <= (status_old & mask))
		return -EINVAL;

	write_enable(nor);
	return write_sr(nor, status_new);
}

/*
 * Unlock a region of the flash. See stm_lock() for more info
 *
 * Returns negative on errors, 0 on success.
 */
static int stm_unlock(struct spi_nor *nor, loff_t ofs, uint64_t len)
{
	struct mtd_info *mtd = nor->mtd;
	int status_old, status_new;
	u8 mask = SR_BP2 | SR_BP1 | SR_BP0;
	u8 shift = ffs(mask) - 1, pow, val;

	status_old = read_sr(nor);
	if (status_old < 0)
		return status_old;

	/* Cannot unlock; would unlock larger region than requested */
	if (stm_is_locked_sr(nor, status_old, ofs - mtd->erasesize,
			     mtd->erasesize))
		return -EINVAL;

	/*
	 * Need largest pow such that:
	 *
	 *   1 / (2^pow) >= (len / size)
	 *
	 * so (assuming power-of-2 size) we do:
	 *
	 *   pow = floor(log2(size / len)) = log2(size) - ceil(log2(len))
	 */
	pow = ilog2(mtd->size) - order_base_2(mtd->size - (ofs + len));
	if (ofs + len == mtd->size) {
		val = 0; /* fully unlocked */
	} else {
		val = mask - (pow << shift);
		/* Some power-of-two sizes are not supported */
		if (val & ~mask)
			return -EINVAL;
	}

	status_new = (status_old & ~mask) | val;

	/* Only modify protection if it will not lock other areas */
	if ((status_new & mask) >= (status_old & mask))
		return -EINVAL;

	write_enable(nor);
	return write_sr(nor, status_new);
}

/*
 * Check if a region of the flash is (completely) locked. See stm_lock() for
 * more info.
 *
 * Returns 1 if entire region is locked, 0 if any portion is unlocked, and
 * negative on errors.
 */
static int stm_is_locked(struct spi_nor *nor, loff_t ofs, uint64_t len)
{
	int status;

	status = read_sr(nor);
	if (status < 0)
		return status;

	return stm_is_locked_sr(nor, ofs, len, status);
}
#endif

static int spi_nor_lock(struct mtd_info *mtd, loff_t ofs, uint64_t len)
{
	struct spi_nor *nor = mtd->priv;

	return nor->flash_lock(nor, ofs, len);
}

static int spi_nor_unlock(struct mtd_info *mtd, loff_t ofs, uint64_t len)
{
	struct spi_nor *nor = mtd->priv;

	return nor->flash_unlock(nor, ofs, len);
}

static int spi_nor_is_locked(struct mtd_info *mtd, loff_t ofs, uint64_t len)
{
	struct spi_nor *nor = mtd->priv;

	return nor->flash_is_locked(nor, ofs, len);
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

static int spi_nor_erase(struct mtd_info *mtd, struct erase_info *instr)
{
	struct spi_nor *nor = mtd->priv;
	u32 addr, len, erase_addr;
	u8 cmd[SNOR_MAX_CMD_SIZE];
	uint32_t rem;
	int ret = -1;

	div_u64_rem(instr->len, mtd->erasesize, &rem);
	if (rem)
		return -EINVAL;

	addr = instr->addr;
	len = instr->len;

	if (mtd->_is_locked) {
		if (mtd->_is_locked(mtd, addr, len) > 0) {
			printf("offset 0x%x is protected and cannot be erased\n",
			       addr);
			return -EINVAL;
		}
	}

	cmd[0] = nor->erase_opcode;
	while (len) {
		erase_addr = addr;

#ifdef CONFIG_SF_DUAL_FLASH
		if (nor->dual > SNOR_DUAL_SINGLE)
			spi_nor_dual(nor, &erase_addr);
#endif
#ifdef CONFIG_SPI_NOR_BAR
		ret = spi_nor_write_bar(nor, erase_addr);
		if (ret < 0)
			return ret;
#endif
		spi_nor_addr(erase_addr, cmd);

		debug("spi-nor: erase %2x %2x %2x %2x (%x)\n", cmd[0], cmd[1],
		      cmd[2], cmd[3], erase_addr);

		write_enable(nor);

		ret = nor->write(nor, cmd, sizeof(cmd), NULL, 0);
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

static int spi_nor_write(struct mtd_info *mtd, loff_t offset, size_t len,
			 size_t *retlen, const u_char *buf)
{
	struct spi_nor *nor = mtd->priv;
	u32 byte_addr, page_size, write_addr;
	size_t chunk_len, actual;
	u8 cmd[SNOR_MAX_CMD_SIZE];
	int ret = -1;

	if (mtd->_is_locked) {
		if (mtd->_is_locked(mtd, offset, len) > 0) {
			printf("offset 0x%llx is protected and cannot be written\n",
			       offset);
			return -EINVAL;
		}
	}

	page_size = nor->page_size;

	cmd[0] = nor->program_opcode;
	for (actual = 0; actual < len; actual += chunk_len) {
		write_addr = offset;

#ifdef CONFIG_SF_DUAL_FLASH
		if (nor->dual > SNOR_DUAL_SINGLE)
			spi_nor_dual(nor, &write_addr);
#endif
#ifdef CONFIG_SPI_NOR_BAR
		ret = spi_nor_write_bar(nor, write_addr);
		if (ret < 0)
			return ret;
#endif
		byte_addr = offset % page_size;
		chunk_len = min(len - actual, (size_t)(page_size - byte_addr));

		if (nor->max_write_size)
			chunk_len = min(chunk_len,
					(size_t)nor->max_write_size);

		spi_nor_addr(write_addr, cmd);

		debug("spi-nor: 0x%p => cmd = { 0x%02x 0x%02x%02x%02x } chunk_len = %zu\n",
		      buf + actual, cmd[0], cmd[1], cmd[2], cmd[3], chunk_len);

		write_enable(nor);

		ret = nor->write(nor, cmd, sizeof(cmd),
				 buf + actual, chunk_len);
		if (ret < 0)
			break;

		ret = spi_nor_wait_till_ready(nor, SNOR_READY_WAIT_PROG);
		if (ret < 0)
			return ret;

		offset += chunk_len;
		*retlen += chunk_len;
	}

	return ret;
}

static int spi_nor_read(struct mtd_info *mtd, loff_t from, size_t len,
			size_t *retlen, u_char *buf)
{
	struct spi_nor *nor = mtd->priv;
	u32 remain_len, read_len, read_addr;
	u8 *cmd, cmdsz;
	int bank_sel = 0;
	int ret = -1;

	/* Handle memory-mapped SPI */
	if (nor->memory_map) {
		ret = nor->read_mmap(nor, buf, nor->memory_map + from, len);
		if (ret) {
			debug("spi-nor: mmap read failed\n");
			return ret;
		}

		return ret;
	}

	cmdsz = SNOR_MAX_CMD_SIZE + nor->read_dummy;
	cmd = calloc(1, cmdsz);
	if (!cmd) {
		debug("spi-nor: Failed to allocate cmd\n");
		return -ENOMEM;
	}

	cmd[0] = nor->read_opcode;
	while (len) {
		read_addr = from;

#ifdef CONFIG_SF_DUAL_FLASH
		if (nor->dual > SNOR_DUAL_SINGLE)
			spi_nor_dual(nor, &read_addr);
#endif
#ifdef CONFIG_SPI_NOR_BAR
		ret = spi_nor_write_bar(nor, read_addr);
		if (ret < 0)
			return ret;
		bank_sel = nor->bank_curr;
#endif
		remain_len = ((SNOR_16MB_BOUN << nor->shift) *
				(bank_sel + 1)) - from;
		if (len < remain_len)
			read_len = len;
		else
			read_len = remain_len;

		spi_nor_addr(read_addr, cmd);

		ret = nor->read(nor, cmd, cmdsz, buf, read_len);
		if (ret < 0)
			break;

		from += read_len;
		len -= read_len;
		buf += read_len;
		*retlen += read_len;
	}

	free(cmd);
	return ret;
}

#ifdef CONFIG_SPI_FLASH_SST
static int sst_byte_write(struct spi_nor *nor, u32 offset,
			  const void *buf, size_t *retlen)
{
	int ret;
	u8 cmd[4] = {
		SNOR_OP_BP,
		offset >> 16,
		offset >> 8,
		offset,
	};

	debug("spi-nor: 0x%p => cmd = { 0x%02x 0x%06x }\n",
	      buf, cmd[0], offset);

	ret = write_enable(nor);
	if (ret)
		return ret;

	ret = nor->write(nor, cmd, sizeof(cmd), buf, 1);
	if (ret)
		return ret;

	*retlen += 1;

	return spi_nor_wait_till_ready(nor, SNOR_READY_WAIT_PROG);
}

static int sst_write_wp(struct mtd_info *mtd, loff_t offset, size_t len,
			size_t *retlen, const u_char *buf)
{
	struct spi_nor *nor = mtd->priv;
	size_t actual, cmd_len;
	int ret;
	u8 cmd[4];

	/* If the data is not word aligned, write out leading single byte */
	actual = offset % 2;
	if (actual) {
		ret = sst_byte_write(nor, offset, buf, retlen);
		if (ret)
			goto done;
	}
	offset += actual;

	ret = write_enable(nor);
	if (ret)
		goto done;

	cmd_len = 4;
	cmd[0] = SNOR_OP_AAI_WP;
	cmd[1] = offset >> 16;
	cmd[2] = offset >> 8;
	cmd[3] = offset;

	for (; actual < len - 1; actual += 2) {
		debug("spi-nor: 0x%p => cmd = { 0x%02x 0x%06llx }\n",
		      buf + actual, cmd[0], offset);

		ret = nor->write(nor, cmd, cmd_len, buf + actual, 2);
		if (ret) {
			debug("spi-nor: sst word program failed\n");
			break;
		}

		ret = spi_nor_wait_till_ready(nor, SNOR_READY_WAIT_PROG);
		if (ret)
			break;

		cmd_len = 1;
		offset += 2;
		*retlen += 2;
	}

	if (!ret)
		ret = write_disable(nor);

	/* If there is a single trailing byte, write it out */
	if (!ret && actual != len)
		ret = sst_byte_write(nor, offset, buf + actual, retlen);

 done:
	return ret;
}

static int sst_write_bp(struct mtd_info *mtd, loff_t offset, size_t len,
			size_t *retlen, const u_char *buf)
{
	struct spi_nor *nor = mtd->priv;
	size_t actual;
	int ret;

	for (actual = 0; actual < len; actual++) {
		ret = sst_byte_write(nor, offset, buf + actual, retlen);
		if (ret) {
			debug("spi-nor: sst byte program failed\n");
			break;
		}
		offset++;
	}

	if (!ret)
		ret = write_disable(nor);

	return ret;
}
#endif

#ifdef CONFIG_SPI_FLASH_MACRONIX
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

#if defined(CONFIG_SPI_FLASH_SPANSION) || defined(CONFIG_SPI_FLASH_WINBOND)
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

#ifdef CONFIG_SPI_FLASH_STMICRO
static int micron_quad_enable(struct spi_nor *nor)
{
	int ret, val;

	val = read_evcr(nor);
	if (val < 0)
		return val;

	if (!(val & EVCR_QUAD_EN_MICRON))
		return 0;

	ret = write_evcr(nor, val & ~EVCR_QUAD_EN_MICRON);
	if (ret < 0)
		return ret;

	/* read EVCR and check it */
	ret = read_evcr(nor);
	if (!(ret > 0 && !(ret & EVCR_QUAD_EN_MICRON))) {
		printf("spi-nor: Micron EVCR Quad bit not clear\n");
		return -EINVAL;
	}

	return ret;
}
#endif

static int set_quad_mode(struct spi_nor *nor, const struct spi_nor_info *info)
{
	switch (JEDEC_MFR(info)) {
#ifdef CONFIG_SPI_FLASH_MACRONIX
	case SNOR_MFR_MACRONIX:
		return macronix_quad_enable(nor);
#endif
#if defined(CONFIG_SPI_FLASH_SPANSION) || defined(CONFIG_SPI_FLASH_WINBOND)
	case SNOR_MFR_SPANSION:
	case SNOR_MFR_WINBOND:
		return spansion_quad_enable(nor);
#endif
#ifdef CONFIG_SPI_FLASH_STMICRO
	case SNOR_MFR_MICRON:
		return micron_quad_enable(nor);
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
	struct mtd_info *mtd = nor->mtd;
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

int spi_nor_scan(struct spi_nor *nor)
{
	struct mtd_info *mtd = nor->mtd;
	const struct spi_nor_info *info = NULL;
	static u8 flash_read_cmd[] = {
		SNOR_OP_READ,
		SNOR_OP_READ_FAST,
		SNOR_OP_READ_1_1_2,
		SNOR_OP_READ_1_1_4,
		SNOR_OP_READ_1_1_2_IO,
		SNOR_OP_READ_1_1_4_IO };
	u8 cmd;
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
	mtd->_erase = spi_nor_erase;
	mtd->_read = spi_nor_read;

	if (info->flags & USE_FSR)
		nor->flags |= SNOR_F_USE_FSR;

	if (info->flags & SST_WRITE)
		nor->flags |= SNOR_F_SST_WRITE;

	mtd->_write = spi_nor_write;
#if defined(CONFIG_SPI_FLASH_SST)
	if (nor->flags & SNOR_F_SST_WRITE) {
		if (nor->mode & SNOR_WRITE_1_1_BYTE)
			mtd->_write = sst_write_bp;
		else
			mtd->_write = sst_write_wp;
	}
#endif

#if defined(CONFIG_SPI_FLASH_STMICRO) || defined(CONFIG_SPI_FLASH_SST)
	/* NOR protection support for STmicro/Micron chips and similar */
	if (JEDEC_MFR(info) == SNOR_MFR_MICRON ||
	    JEDEC_MFR(info) == SNOR_MFR_SST) {
		nor->flash_lock = stm_lock;
		nor->flash_unlock = stm_unlock;
		nor->flash_is_locked = stm_is_locked;
	}
#endif

	if (nor->flash_lock && nor->flash_unlock && nor->flash_is_locked) {
		mtd->_lock = spi_nor_lock;
		mtd->_unlock = spi_nor_unlock;
		mtd->_is_locked = spi_nor_is_locked;
	}

	/* Compute the flash size */
	nor->shift = (nor->dual & SNOR_DUAL_PARALLEL) ? 1 : 0;
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
	nor->page_size <<= nor->shift;
	mtd->writebufsize = nor->page_size;
	mtd->size = (info->sector_size * info->n_sectors) << nor->shift;
#ifdef CONFIG_SF_DUAL_FLASH
	if (nor->dual & SNOR_DUAL_STACKED)
		mtd->size <<= 1;
#endif

#ifdef CONFIG_MTD_SPI_NOR_USE_4K_SECTORS
	/* prefer "small sector" erase if possible */
	if (info->flags & SECT_4K) {
		nor->erase_opcode = SNOR_OP_BE_4K;
		mtd->erasesize = 4096 << nor->shift;
	} else if (info->flags & SECT_4K_PMC) {
		nor->erase_opcode = SNOR_OP_BE_4K_PMC;
		mtd->erasesize = 4096;
	} else
#endif
	{
		nor->erase_opcode = SNOR_OP_SE;
		mtd->erasesize = info->sector_size << nor->shift;
	}

	if (info->flags & SPI_NOR_NO_ERASE)
		mtd->flags |= MTD_NO_ERASE;

	/* Look for the fastest read cmd */
	cmd = fls(info->flash_read & nor->read_mode);
	if (cmd) {
		cmd = flash_read_cmd[cmd - 1];
		nor->read_opcode = cmd;
	} else {
		/* Go for default supported read cmd */
		nor->read_opcode = SNOR_OP_READ_FAST;
	}

	/* Some devices cannot do fast-read */
	if (info->flags & SPI_NOR_NO_FR)
		nor->read_opcode = SNOR_OP_READ;

	/* Not require to look for fastest only two write cmds yet */
	if (info->flags & SNOR_WRITE_QUAD && nor->mode & SNOR_WRITE_1_1_4)
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

	/* read_dummy: dummy byte is determined based on the
	 * dummy cycles of a particular command.
	 * Fast commands - read_dummy = dummy_cycles/8
	 * I/O commands- read_dummy = (dummy_cycles * no.of lines)/8
	 * For I/O commands except cmd[0] everything goes on no.of lines
	 * based on particular command but incase of fast commands except
	 * data all go on single line irrespective of command.
	 */
	switch (nor->read_opcode) {
	case SNOR_OP_READ_1_1_4_IO:
		nor->read_dummy = 2;
		break;
	case SNOR_OP_READ:
		nor->read_dummy = 0;
		break;
	default:
		nor->read_dummy = 1;
	}

	/* Configure the BAR - discover bank cmds and read current bank */
#ifdef CONFIG_SPI_NOR_BAR
	ret = spi_nor_read_bar(nor, info);
	if (ret < 0)
		return ret;
#endif

#if CONFIG_IS_ENABLED(OF_CONTROL)
	ret = spi_nor_decode_fdt(gd->fdt_blob, nor);
	if (ret) {
		debug("spi-nor: FDT decode error\n");
		return -EINVAL;
	}
#endif

#ifndef CONFIG_SPL_BUILD
	printf("spi-nor: detected %s with page size ", mtd->name);
	print_size(nor->page_size, ", erase size ");
	print_size(mtd->erasesize, ", total ");
	print_size(mtd->size, "");
	if (nor->memory_map)
		printf(", mapped at %p", nor->memory_map);
	puts("\n");
#endif

#ifndef CONFIG_SPI_NOR_BAR
	if (((nor->dual == SNOR_DUAL_SINGLE) &&
	     (mtd->size > SNOR_16MB_BOUN)) ||
	     ((nor->dual > SNOR_DUAL_SINGLE) &&
	     (mtd->size > SNOR_16MB_BOUN << 1))) {
		puts("spi-nor: Warning - Only lower 16MiB accessible,");
		puts(" Full access #define CONFIG_SPI_NOR_BAR\n");
	}
#endif

	return ret;
}
