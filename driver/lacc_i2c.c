// SPDX-License-Identifier: GPL-2.0
/*
 * The card's SMBus controller, exposed as an i2c_adapter. The kernel's own drivers bind to the
 * chips on it: the generic PMBus driver to the voltage regulator at 0x40 (hwmon: voltages,
 * currents, power, temperature) and at24 to the FRU EEPROM at 0x50 (nvmem).
 */

#include <linux/i2c.h>
#include <linux/iopoll.h>
#include <linux/unaligned.h>

#include "lacc.h"

#define LACC_I2C_START BIT(31)
#define LACC_I2C_READ BIT(12)

static int lacc_smbus_xfer(struct i2c_adapter *adap, u16 addr, unsigned short flags,
			   char read_write, u8 command, int size, union i2c_smbus_data *data)
{
	struct lacc_dev *ld = i2c_get_adapdata(adap);
	bool rd = read_write == I2C_SMBUS_READ;
	u32 op, len = 0, st, i;
	u8 buf[64] = {};
	int ret;

	switch (size) {
	case I2C_SMBUS_QUICK:
		op = LACC_I2C_OP_QUICK;
		break;
	case I2C_SMBUS_BYTE:
		op = LACC_I2C_OP_BYTE;
		break;
	case I2C_SMBUS_BYTE_DATA:
		op = LACC_I2C_OP_BYTE_DATA;
		if (!rd)
			buf[0] = data->byte;
		break;
	case I2C_SMBUS_WORD_DATA:
		op = LACC_I2C_OP_WORD_DATA;
		if (!rd) {
			buf[0] = data->word & 0xff;
			buf[1] = data->word >> 8;
		}
		break;
	case I2C_SMBUS_BLOCK_DATA:
		op = LACC_I2C_OP_BLOCK_DATA;
		if (!rd) {
			len = min_t(u32, data->block[0], I2C_SMBUS_BLOCK_MAX);
			memcpy(buf, &data->block[1], len);
		}
		break;
	case I2C_SMBUS_I2C_BLOCK_DATA:
		op = LACC_I2C_OP_I2C_BLOCK;
		len = min_t(u32, data->block[0], I2C_SMBUS_BLOCK_MAX);
		if (!rd)
			memcpy(buf, &data->block[1], len);
		break;
	default:
		return -EOPNOTSUPP;
	}

	mutex_lock(&ld->bus_lock);
	/* BYTE writes carry their byte in the command field (SMBus "send byte") */
	lacc_wr(ld, LACC_REG_I2C_CMD, command);
	lacc_wr(ld, LACC_REG_I2C_LEN, len);
	if (!rd)
		for (i = 0; i < sizeof(buf); i += 4)
			lacc_wr(ld, LACC_REG_I2C_DATA + i, get_unaligned_le32(buf + i));
	lacc_wr(ld, LACC_REG_I2C_CTRL,
		LACC_I2C_START | (rd ? LACC_I2C_READ : 0) | (op << 8) | (addr & 0x7f));
	ret = read_poll_timeout(lacc_rd, st, st & LACC_I2C_DONE, 10, 10000, false, ld,
				LACC_REG_I2C_STATUS);
	if (ret)
		goto out;
	if (st == ~0u || (st & LACC_I2C_NAK)) {
		ret = -ENXIO;
		goto out;
	}
	if (rd) {
		len = lacc_rd(ld, LACC_REG_I2C_LEN);
		for (i = 0; i < sizeof(buf); i += 4)
			put_unaligned_le32(lacc_rd(ld, LACC_REG_I2C_DATA + i), buf + i);
		switch (size) {
		case I2C_SMBUS_BYTE:
		case I2C_SMBUS_BYTE_DATA:
			data->byte = buf[0];
			break;
		case I2C_SMBUS_WORD_DATA:
			data->word = buf[0] | (buf[1] << 8);
			break;
		case I2C_SMBUS_BLOCK_DATA:
			len = min_t(u32, len, I2C_SMBUS_BLOCK_MAX);
			data->block[0] = len;
			memcpy(&data->block[1], buf, len);
			break;
		case I2C_SMBUS_I2C_BLOCK_DATA:
			memcpy(&data->block[1], buf, data->block[0]);
			break;
		}
	}
out:
	mutex_unlock(&ld->bus_lock);
	return ret;
}

static u32 lacc_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_SMBUS_QUICK | I2C_FUNC_SMBUS_BYTE | I2C_FUNC_SMBUS_BYTE_DATA |
	       I2C_FUNC_SMBUS_WORD_DATA | I2C_FUNC_SMBUS_BLOCK_DATA | I2C_FUNC_SMBUS_I2C_BLOCK;
}

static const struct i2c_algorithm lacc_i2c_algo = {
	.smbus_xfer = lacc_smbus_xfer,
	.functionality = lacc_i2c_func,
};

int lacc_i2c_init(struct lacc_dev *ld)
{
	struct i2c_board_info pmbus = { I2C_BOARD_INFO("pmbus", 0x40) };
	struct i2c_board_info eeprom = { I2C_BOARD_INFO("24c02", 0x50) };
	int ret;

	ld->i2c.owner = THIS_MODULE;
	ld->i2c.algo = &lacc_i2c_algo;
	ld->i2c.dev.parent = &ld->pdev->dev;
	ld->i2c.class = I2C_CLASS_HWMON;
	snprintf(ld->i2c.name, sizeof(ld->i2c.name), "%s board SMBus", ld->name);
	i2c_set_adapdata(&ld->i2c, ld);
	ret = i2c_add_adapter(&ld->i2c);
	if (ret)
		return ret;
	ld->pmbus = i2c_new_client_device(&ld->i2c, &pmbus);
	if (IS_ERR(ld->pmbus)) {
		dev_warn(&ld->pdev->dev, "PMBus regulator not instantiated\n");
		ld->pmbus = NULL;
	}
	ld->eeprom = i2c_new_client_device(&ld->i2c, &eeprom);
	if (IS_ERR(ld->eeprom)) {
		dev_warn(&ld->pdev->dev, "FRU EEPROM not instantiated\n");
		ld->eeprom = NULL;
	}
	return 0;
}

void lacc_i2c_fini(struct lacc_dev *ld)
{
	if (!ld->i2c.algo)
		return;
	i2c_unregister_device(ld->eeprom);
	i2c_unregister_device(ld->pmbus);
	i2c_del_adapter(&ld->i2c);
	ld->i2c.algo = NULL;
}
