#include "rtc.h"

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(rtc_test, LOG_LEVEL_INF);

#define RTC_I2C_NODE DT_ALIAS(protocol_i2c)
#define PCF85363A_I2C_ADDRESS 0x51U
#define PCF85363A_TIME_REGISTER 0x00U
#define PCF85363A_STOP_ENABLE_REGISTER 0x2eU
#define PCF85363A_CPR_RESET 0xa4U
#define PCF85363A_OSCILLATOR_STOP_FLAG 0x80U
#define RTC_REPORT_INTERVAL K_SECONDS(30)

#if !DT_NODE_HAS_STATUS(RTC_I2C_NODE, okay)
#error "protocol-i2c alias must reference an enabled I2C controller"
#endif

struct rtc_datetime {
	uint16_t year;
	uint8_t month;
	uint8_t day;
	uint8_t weekday;
	uint8_t hour;
	uint8_t minute;
	uint8_t second;
	uint8_t hundredths;
};

static const struct device *const rtc_i2c_bus = DEVICE_DT_GET(RTC_I2C_NODE);

static uint8_t bin_to_bcd(uint8_t value)
{
	return (uint8_t)(((value / 10U) << 4) | (value % 10U));
}

static int bcd_to_bin(uint8_t value, uint8_t minimum, uint8_t maximum,
		      uint8_t *result)
{
	uint8_t tens = (value >> 4) & 0x0fU;
	uint8_t ones = value & 0x0fU;
	uint8_t decoded;

	if ((tens > 9U) || (ones > 9U)) {
		return -EINVAL;
	}

	decoded = (uint8_t)(tens * 10U + ones);
	if ((decoded < minimum) || (decoded > maximum)) {
		return -EINVAL;
	}

	*result = decoded;
	return 0;
}

static int rtc_read_datetime(struct rtc_datetime *datetime)
{
	uint8_t register_address = PCF85363A_TIME_REGISTER;
	uint8_t registers[8];
	uint8_t year;
	int ret;

	ret = i2c_write_read(rtc_i2c_bus, PCF85363A_I2C_ADDRESS,
			      &register_address, sizeof(register_address),
			      registers, sizeof(registers));
	if (ret != 0) {
		return ret;
	}

	if ((registers[1] & PCF85363A_OSCILLATOR_STOP_FLAG) != 0U) {
		return -ENODATA;
	}

	ret = bcd_to_bin(registers[0], 0U, 99U, &datetime->hundredths);
	if (ret != 0) {
		return ret;
	}

	ret = bcd_to_bin(registers[1] & 0x7fU, 0U, 59U, &datetime->second);
	if (ret != 0) {
		return ret;
	}

	ret = bcd_to_bin(registers[2] & 0x7fU, 0U, 59U, &datetime->minute);
	if (ret != 0) {
		return ret;
	}

	ret = bcd_to_bin(registers[3] & 0x3fU, 0U, 23U, &datetime->hour);
	if (ret != 0) {
		return ret;
	}

	ret = bcd_to_bin(registers[4] & 0x3fU, 1U, 31U, &datetime->day);
	if (ret != 0) {
		return ret;
	}

	datetime->weekday = registers[5] & 0x07U;
	if (datetime->weekday > 6U) {
		return -EINVAL;
	}

	ret = bcd_to_bin(registers[6] & 0x1fU, 1U, 12U, &datetime->month);
	if (ret != 0) {
		return ret;
	}

	ret = bcd_to_bin(registers[7], 0U, 99U, &year);
	if (ret != 0) {
		return ret;
	}

	datetime->year = 2000U + year;
	return 0;
}

static void rtc_log_datetime(const struct rtc_datetime *datetime)
{
	LOG_INF("RTC: %04u-%02u-%02u weekday=%u %02u:%02u:%02u.%02u",
		datetime->year, datetime->month, datetime->day,
		datetime->weekday, datetime->hour, datetime->minute,
		datetime->second, datetime->hundredths);
}

static void rtc_report_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(rtc_report_work, rtc_report_work_handler);

static void rtc_report_work_handler(struct k_work *work)
{
	struct rtc_datetime datetime;
	int ret;

	ARG_UNUSED(work);

	ret = rtc_read_datetime(&datetime);
	if (ret == 0) {
		rtc_log_datetime(&datetime);
	} else {
		LOG_ERR("PCF85363A time read failed at address 0x%02x: %d",
			PCF85363A_I2C_ADDRESS, ret);
	}

	(void)k_work_schedule(&rtc_report_work, RTC_REPORT_INTERVAL);
}

static int rtc_set_test_datetime(void)
{
	/* 2025-01-01 Wednesday 12:34:56.00. Weekday 0 is Sunday. */
	uint8_t stop_and_reset[] = {
		PCF85363A_STOP_ENABLE_REGISTER, 0x01U, PCF85363A_CPR_RESET
	};
	uint8_t time_registers[] = {
		PCF85363A_TIME_REGISTER,
		bin_to_bcd(0U),
		bin_to_bcd(56U),
		bin_to_bcd(34U),
		bin_to_bcd(12U),
		bin_to_bcd(1U),
		3U,
		bin_to_bcd(1U),
		bin_to_bcd(25U),
	};
	uint8_t restart[] = {PCF85363A_STOP_ENABLE_REGISTER, 0x00U};
	int write_ret;
	int restart_ret;
	int ret;

	ret = i2c_write(rtc_i2c_bus, stop_and_reset,
			 sizeof(stop_and_reset), PCF85363A_I2C_ADDRESS);
	if (ret != 0) {
		LOG_ERR("PCF85363A stop/reset failed: %d", ret);
		return ret;
	}

	write_ret = i2c_write(rtc_i2c_bus, time_registers,
			       sizeof(time_registers), PCF85363A_I2C_ADDRESS);
	restart_ret = i2c_write(rtc_i2c_bus, restart,
				 sizeof(restart), PCF85363A_I2C_ADDRESS);

	if (write_ret != 0) {
		LOG_ERR("PCF85363A time write failed: %d", write_ret);
		return write_ret;
	}

	if (restart_ret != 0) {
		LOG_ERR("PCF85363A restart failed: %d", restart_ret);
		return restart_ret;
	}

	return 0;
}

int rtc_test_start(void)
{
	struct rtc_datetime datetime;
	int ret;

	if (!device_is_ready(rtc_i2c_bus)) {
		LOG_ERR("RTC I2C bus %s is not ready", rtc_i2c_bus->name);
		return -ENODEV;
	}

	ret = rtc_set_test_datetime();
	if (ret != 0) {
		return ret;
	}

	ret = rtc_read_datetime(&datetime);
	if (ret != 0) {
		LOG_ERR("PCF85363A initial readback failed: %d", ret);
		return ret;
	}

	LOG_INF("PCF85363A detected at I2C address 0x%02x",
		PCF85363A_I2C_ADDRESS);
	rtc_log_datetime(&datetime);
	(void)k_work_schedule(&rtc_report_work, RTC_REPORT_INTERVAL);

	return 0;
}
