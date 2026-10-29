#include "qspi.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(qspi_flash_test, LOG_LEVEL_INF);

#define QSPI_TEST_PARTITION DT_NODELABEL(qspi_test_partition)
#define QSPI_TEST_OFFSET DT_REG_ADDR(QSPI_TEST_PARTITION)
#define QSPI_TEST_SIZE DT_REG_SIZE(QSPI_TEST_PARTITION)

static const struct device *const qspi_flash =
	DEVICE_DT_GET(DT_MTD_FROM_FIXED_PARTITION(QSPI_TEST_PARTITION));

static const uint8_t test_pattern[32] = "Arctano";

BUILD_ASSERT(DT_NODE_HAS_STATUS(QSPI_TEST_PARTITION, okay),
	     "qspi_test_partition must be enabled");
BUILD_ASSERT(QSPI_TEST_SIZE >= sizeof(test_pattern),
	     "qspi_test_partition is too small");

static int verify_erased(void)
{
	uint8_t buffer[sizeof(test_pattern)];
	int ret;

	ret = flash_read(qspi_flash, QSPI_TEST_OFFSET, buffer, sizeof(buffer));
	if (ret != 0) {
		LOG_ERR("Flash read after erase failed: %d", ret);
		return ret;
	}

	for (size_t i = 0; i < sizeof(buffer); i++) {
		if (buffer[i] != 0xffU) {
			LOG_ERR("Erase verification failed at byte %u: 0x%02x",
				(unsigned int)i, buffer[i]);
			return -EIO;
		}
	}

	return 0;
}

int qspi_flash_self_test(void)
{
	uint8_t read_buffer[sizeof(test_pattern)] = {0};
	int ret;

	if (!device_is_ready(qspi_flash)) {
		LOG_ERR("QSPI flash device %s is not ready", qspi_flash->name);
		return -ENODEV;
	}

	LOG_INF("Testing %s at offset 0x%lx", qspi_flash->name,
		(unsigned long)QSPI_TEST_OFFSET);

	ret = flash_erase(qspi_flash, QSPI_TEST_OFFSET, QSPI_TEST_SIZE);
	if (ret != 0) {
		LOG_ERR("QSPI test-sector erase failed: %d", ret);
		return ret;
	}

	ret = verify_erased();
	if (ret != 0) {
		return ret;
	}

	ret = flash_write(qspi_flash, QSPI_TEST_OFFSET,
			  test_pattern, sizeof(test_pattern));
	if (ret != 0) {
		LOG_ERR("QSPI test write failed: %d", ret);
		return ret;
	}

	ret = flash_read(qspi_flash, QSPI_TEST_OFFSET,
			 read_buffer, sizeof(read_buffer));
	if (ret != 0) {
		LOG_ERR("QSPI test read failed: %d", ret);
		return ret;
	}

	if (memcmp(test_pattern, read_buffer, sizeof(test_pattern)) != 0) {
		LOG_ERR("QSPI readback does not match written data");
		return -EIO;
	}

	LOG_INF("QSPI flash test passed; read back: \"%s\"", read_buffer);

	ret = flash_erase(qspi_flash, QSPI_TEST_OFFSET, QSPI_TEST_SIZE);
	if (ret != 0) {
		LOG_ERR("QSPI test-sector cleanup failed: %d", ret);
		return ret;
	}

	LOG_INF("QSPI test partition restored to erased state");
	return 0;
}
