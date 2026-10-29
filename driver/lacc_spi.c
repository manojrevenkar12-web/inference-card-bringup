// SPDX-License-Identifier: GPL-2.0
/*
 * The card's SPI controller, exposed as a spi_controller with one chip select. The kernel's
 * spi-nor driver identifies the flash by its JEDEC ID and registers it as MTD with the board's
 * partitions: the two firmware slots, the boot configuration sector, and the rest.
 * Firmware updates are then ordinary MTD writes (/dev/mtdN), done by laccctl.
 */

#include <linux/iopoll.h>
#include <linux/unaligned.h>
#include <linux/mtd/partitions.h>
#include <linux/spi/flash.h>
#include <linux/spi/spi.h>

#include "lacc.h"

#define LACC_SPI_FIFO 256

static const struct mtd_partition lacc_flash_parts[] = {
	{ .name = "lacc-fw-a", .offset = LACC_FLASH_SLOT_A, .size = LACC_FLASH_SLOT_SIZE },
	{ .name = "lacc-fw-b", .offset = LACC_FLASH_SLOT_B, .size = LACC_FLASH_SLOT_SIZE },
	{ .name = "lacc-config", .offset = LACC_FLASH_CONFIG, .size = LACC_FLASH_CONFIG_SIZE },
	{ .name = "lacc-spare", .offset = MTDPART_OFS_APPEND, .size = MTDPART_SIZ_FULL },
};

static struct flash_platform_data lacc_flash_data = {
	.name = "lacc-flash",
	.parts = (struct mtd_partition *)lacc_flash_parts,
	.nr_parts = ARRAY_SIZE(lacc_flash_parts),
};

static void lacc_spi_set_cs(struct spi_device *spi, bool level)
{
	struct lacc_dev *ld = spi_controller_get_devdata(spi->controller);

	/* the core passes the electrical level; chip select is active low */
	lacc_wr(ld, LACC_REG_SPI_CTRL, level ? 0 : LACC_SPI_CS);
}

static int lacc_spi_transfer_one(struct spi_controller *ctlr, struct spi_device *spi,
				 struct spi_transfer *t)
{
	struct lacc_dev *ld = spi_controller_get_devdata(ctlr);
	const u8 *tx = t->tx_buf;
	u8 *rx = t->rx_buf;
	u32 done = 0, st, i;
	int ret = 0;

	mutex_lock(&ld->bus_lock);
	while (done < t->len) {
		u32 n = min_t(u32, t->len - done, LACC_SPI_FIFO);
		u8 buf[LACC_SPI_FIFO];

		if (tx)
			memcpy(buf, tx + done, n);
		else
			memset(buf, 0xff, n);
		for (i = 0; i < round_up(n, 4); i += 4)
			lacc_wr(ld, LACC_REG_SPI_TX + i, get_unaligned_le32(buf + i));
		lacc_wr(ld, LACC_REG_SPI_LEN, n);
		lacc_wr(ld, LACC_REG_SPI_CTRL, LACC_SPI_CS | LACC_SPI_START);
		ret = read_poll_timeout(lacc_rd, st, st & LACC_SPI_DONE, 5, 10000, false, ld,
					LACC_REG_SPI_STATUS);
		if (ret)
			break;
		if (rx) {
			for (i = 0; i < round_up(n, 4); i += 4)
				put_unaligned_le32(lacc_rd(ld, LACC_REG_SPI_RX + i), buf + i);
			memcpy(rx + done, buf, n);
		}
		done += n;
	}
	mutex_unlock(&ld->bus_lock);
	return ret;
}

int lacc_spi_init(struct lacc_dev *ld)
{
	struct spi_board_info info = {
		.modalias = "spi-nor",
		.max_speed_hz = 50000000,
		.chip_select = 0,
		.mode = SPI_MODE_0,
		.platform_data = &lacc_flash_data,
	};
	struct spi_controller *ctlr;
	int ret;

	ctlr = spi_alloc_host(&ld->pdev->dev, 0);
	if (!ctlr)
		return -ENOMEM;
	spi_controller_set_devdata(ctlr, ld);
	ctlr->bus_num = -1;
	ctlr->num_chipselect = 1;
	ctlr->mode_bits = 0;
	ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
	ctlr->set_cs = lacc_spi_set_cs;
	ctlr->transfer_one = lacc_spi_transfer_one;
	ret = spi_register_controller(ctlr);
	if (ret) {
		spi_controller_put(ctlr);
		return ret;
	}
	ld->spi = ctlr;
	ld->flash = spi_new_device(ctlr, &info);
	if (!ld->flash)
		dev_warn(&ld->pdev->dev, "SPI NOR flash not instantiated\n");
	return 0;
}

void lacc_spi_fini(struct lacc_dev *ld)
{
	if (!ld->spi)
		return;
	if (ld->flash)
		spi_unregister_device(ld->flash);
	spi_unregister_controller(ld->spi);
	ld->spi = NULL;
	ld->flash = NULL;
}
