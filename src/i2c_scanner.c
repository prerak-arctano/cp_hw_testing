#include "i2c_scanner.h"

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(i2c_scanner, LOG_LEVEL_INF);

#define I2C_SCANNER_NODE DT_ALIAS(protocol_i2c)
#define I2C_SCANNER_FIRST_ADDRESS 0x08U
#define I2C_SCANNER_LAST_ADDRESS  0x77U

#if !DT_NODE_HAS_STATUS(I2C_SCANNER_NODE, okay)
#error "protocol-i2c alias must reference an enabled I2C controller"
#endif

static const struct device *const i2c_scanner_bus =
	DEVICE_DT_GET(I2C_SCANNER_NODE);

int i2c_scanner_scan(void)
{
	uint8_t dummy = 0U;
	int device_count = 0;

	if (!device_is_ready(i2c_scanner_bus)) {
		LOG_ERR("I2C bus %s is not ready", i2c_scanner_bus->name);
		return -ENODEV;
	}

	LOG_INF("Scanning I2C bus %s from 0x%02x to 0x%02x",
		i2c_scanner_bus->name,
		I2C_SCANNER_FIRST_ADDRESS,
		I2C_SCANNER_LAST_ADDRESS);

	for (uint16_t address = I2C_SCANNER_FIRST_ADDRESS;
	     address <= I2C_SCANNER_LAST_ADDRESS;
	     address++) {
		int ret = i2c_write(i2c_scanner_bus, &dummy, 0U, address);

		if (ret == 0) {
			LOG_INF("I2C device found at 0x%02x", address);
			device_count++;
		}
	}

	LOG_INF("I2C scan complete: %d device(s) found", device_count);
	return device_count;
}
