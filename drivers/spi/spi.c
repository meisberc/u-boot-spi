/*
 * Copyright (c) 2011 The Chromium OS Authors.
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <fdtdec.h>
#include <malloc.h>
#include <spi.h>

int spi_set_wordlen(struct spi_slave *slave, unsigned int wordlen)
{
	if (wordlen == 0 || wordlen > 32) {
		printf("spi: invalid wordlen %d\n", wordlen);
		return -1;
	}

	slave->wordlen = wordlen;

	return 0;
}

void *spi_do_alloc_slave(int offset, int size, unsigned int bus,
			 unsigned int cs)
{
	struct spi_slave *slave;
	void *ptr;

	ptr = malloc(size);
	if (ptr) {
		memset(ptr, '\0', size);
		slave = (struct spi_slave *)(ptr + offset);
		slave->bus = bus;
		slave->cs = cs;
		slave->wordlen = SPI_DEFAULT_WORDLEN;
	}

	return ptr;
}

int __weak spi_xfer(struct spi_slave *slave, unsigned int bitlen,
		    const void *dout, void *din, unsigned long flags)
{
	return 0;
}

int spi_read_then_write(struct spi_slave *spi, const u8 *cmd,
			size_t cmd_len, const u8 *data_out,
			u8 *data_in, size_t data_len)
{
	unsigned long flags = SPI_XFER_BEGIN;
	int ret;

	if (data_len == 0)
		flags |= SPI_XFER_END;

	ret = spi_xfer(spi, cmd_len * 8, cmd, NULL, flags);
	if (ret) {
		debug("spi: failed to send command (%zu bytes): %d\n",
		      cmd_len, ret);
	} else if (data_len != 0) {
		ret = spi_xfer(spi, data_len * 8, data_out, data_in,
					SPI_XFER_END);
		if (ret)
			debug("spi: failed to transfer %zu bytes of data: %d\n",
			      data_len, ret);
	}

	return ret;
}

#ifdef CONFIG_OF_SPI
struct spi_slave *spi_base_setup_slave_fdt(const void *blob, int busnum,
					   int node)
{
	int cs, max_hz, mode = 0;

	cs = fdtdec_get_int(blob, node, "reg", -1);
	max_hz = fdtdec_get_int(blob, node, "spi-max-frequency", 100000);
	if (fdtdec_get_bool(blob, node, "spi-cpol"))
		mode |= SPI_CPOL;
	if (fdtdec_get_bool(blob, node, "spi-cpha"))
		mode |= SPI_CPHA;
	if (fdtdec_get_bool(blob, node, "spi-cs-high"))
		mode |= SPI_CS_HIGH;
	if (fdtdec_get_bool(blob, node, "spi-half-duplex"))
		mode |= SPI_PREAMBLE;
	return spi_setup_slave(busnum, cs, max_hz, mode);
}
#endif
