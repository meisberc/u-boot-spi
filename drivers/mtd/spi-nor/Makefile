#
# Copyright (C) 2016 Jagan Teki <jteki@openedev.com>
#
# SPDX-License-Identifier:	GPL-2.0+

ifdef CONFIG_SPL_BUILD
obj-$(CONFIG_SPL_SPI_LOAD)	+= spi_spl_load.o
obj-$(CONFIG_SPL_SPI_BOOT)	+= fsl_espi_spl.o
endif

ifdef CONFIG_MTD_SPI_NOR
obj-y += spi-nor.o
obj-y += spi-nor-ids.o

obj-$(CONFIG_DM_MTD_SPI_NOR)	+= spi-nor-probe.o
endif

obj-$(CONFIG_MTD_M25P80)	+= m25p80.o
