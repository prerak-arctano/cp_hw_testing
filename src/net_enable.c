#include "net_enable.h"

#include <errno.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>

#define NET_ENABLE_USER_NODE DT_PATH(zephyr_user)

#if !DT_NODE_HAS_PROP(NET_ENABLE_USER_NODE, net_1_en_gpios)
#error "zephyr,user must define net-1-en-gpios"
#endif

#if !DT_NODE_HAS_PROP(NET_ENABLE_USER_NODE, net_2_en_gpios)
#error "zephyr,user must define net-2-en-gpios"
#endif

static const struct gpio_dt_spec net_1_en =
	GPIO_DT_SPEC_GET(NET_ENABLE_USER_NODE, net_1_en_gpios);
static const struct gpio_dt_spec net_2_en =
	GPIO_DT_SPEC_GET(NET_ENABLE_USER_NODE, net_2_en_gpios);

int net_enable_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&net_1_en) || !gpio_is_ready_dt(&net_2_en)) {
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&net_1_en, GPIO_OUTPUT_ACTIVE);
	if (ret != 0) {
		return ret;
	}

	return gpio_pin_configure_dt(&net_2_en, GPIO_OUTPUT_ACTIVE);
}
