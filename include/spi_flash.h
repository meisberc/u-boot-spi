/*
 * Common SPI flash Interface
 *
 * Copyright (C) 2016 Jagan Teki <jteki@openedev.com>
 * Copyright (C) 2013 Jagannadha Sutradharudu Teki, Xilinx Inc.
 * Copyright (C) 2008 Atmel Corporation
 *
 * SPDX-License-Identifier:	GPL-2.0
 */

#ifndef _SPI_FLASH_H_
#define _SPI_FLASH_H_

#include <dm.h>	/* Because we dereference struct udevice here */
#include <linux/types.h>
#include <linux/mtd/mtd.h>

#ifndef CONFIG_SF_DEFAULT_SPEED
# define CONFIG_SF_DEFAULT_SPEED	1000000
#endif
#ifndef CONFIG_SF_DEFAULT_MODE
# define CONFIG_SF_DEFAULT_MODE		SPI_MODE_3
#endif
#ifndef CONFIG_SF_DEFAULT_CS
# define CONFIG_SF_DEFAULT_CS		0
#endif
#ifndef CONFIG_SF_DEFAULT_BUS
# define CONFIG_SF_DEFAULT_BUS		0
#endif

typedef struct mtd_info spi_flash_t;

static inline int spi_flash_read(spi_flash_t *info, u32 offset,
				 size_t len, void *buf)
{
	return mtd_read(info, offset, len, &len, (u_char *)buf);
}

static inline int spi_flash_write(spi_flash_t *info, u32 offset,
				  size_t len, const void *buf)
{
	return mtd_write(info, offset, len, &len, (u_char *)buf);
}

static inline int spi_flash_erase(spi_flash_t *info, u32 offset, size_t len)
{
	struct erase_info instr;

	instr.mtd = info;
	instr.addr = offset;
	instr.len = len;
	instr.callback = 0;

	return mtd_erase(info, &instr);
}

static inline int spi_flash_protect(spi_flash_t *info, u32 ofs,
				    u32 len, bool prot)
{
	if (prot)
		return mtd_lock(info, ofs, len);
	else
		return mtd_unlock(info, ofs, len);
}

#ifdef CONFIG_DM_MTD_SPI_NOR

struct sandbox_state;

int sandbox_sf_bind_emul(struct sandbox_state *state, int busnum, int cs,
			 struct udevice *bus, int of_offset, const char *spec);

void sandbox_sf_unbind_emul(struct sandbox_state *state, int busnum, int cs);

int spi_flash_probe_bus_cs(unsigned int busnum, unsigned int cs,
			   unsigned int max_hz, unsigned int spi_mode,
			   struct udevice **devp);

/* Compatibility function - this is the old U-Boot API */
spi_flash_t *spi_flash_probe(unsigned int bus, unsigned int cs,
			     unsigned int max_hz, unsigned int spi_mode);

/* Compatibility function - this is the old U-Boot API */
void spi_flash_free(spi_flash_t *flash);

#else

spi_flash_t *spi_flash_probe(unsigned int bus, unsigned int cs,
			     unsigned int max_hz, unsigned int spi_mode);

spi_flash_t *spi_flash_probe_fdt(const void *blob, int slave_node,
				 int spi_node);

void spi_flash_free(spi_flash_t *flash);

#endif /* CONFIG_DM_MTD_SPI_NOR */

void spi_boot(void) __noreturn;
void spi_spl_load_image(uint32_t offs, unsigned int size, void *vdst);

#endif /* _SPI_FLASH_H_ */
