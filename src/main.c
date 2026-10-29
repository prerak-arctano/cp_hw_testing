#include "i2c_scanner.h"
#include "net_enable.h"

int main(void)
{
	(void)net_enable_init();
	(void)i2c_scanner_scan();
	return 0;
}
