#include "i2c_scanner.h"
#include "net_enable.h"
#include "qspi.h"
#include "rtc.h"
#include "uart.h"

int main(void)
{
	(void)net_enable_init();
	(void)software_uart_send_hello();
	(void)qspi_flash_self_test();
	(void)i2c_scanner_scan();
	(void)rtc_test_start();
	return 0;
}
