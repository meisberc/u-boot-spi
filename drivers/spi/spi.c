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

int spi_write_then_read(struct spi_slave *slave, const u8 *opcode,
			size_t n_opcode, const u8 *txbuf, u8 *rxbuf,
			size_t n_buf)
{
	unsigned long flags = SPI_XFER_BEGIN;
	int ret;

	if (n_buf == 0)
		flags |= SPI_XFER_END;

	ret = spi_xfer(slave, n_opcode * 8, opcode, NULL, flags);
	if (ret) {
		debug("spi: failed to send command (%zu bytes): %d\n",
		      n_opcode, ret);
	} else if (n_buf != 0) {
		ret = spi_xfer(slave, n_buf * 8, txbuf, rxbuf, SPI_XFER_END);
		if (ret)
			debug("spi: failed to transfer %zu bytes of data: %d\n",
			      n_buf, ret);
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
