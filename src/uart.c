#include "uart.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(software_uart, LOG_LEVEL_INF);

#define SOFTWARE_UART_USER_NODE DT_PATH(zephyr_user)
#define SOFTWARE_UART_RX_NODE DT_NODELABEL(gpio2)
#define SOFTWARE_UART_RX_PIN 6U
#define SOFTWARE_UART_BAUD_RATE 9600U
#define SOFTWARE_UART_BIT_PERIOD_US 104U
#define SOFTWARE_UART_HALF_BIT_PERIOD_US 52U
#define SOFTWARE_UART_FIRST_DATA_SAMPLE_US 156U
#define SOFTWARE_UART_STOP_SAMPLE_US 988U
#define SOFTWARE_UART_RX_POLL_INTERVAL_US 16U
#define SOFTWARE_UART_RX_STACK_SIZE 1024U
#define SOFTWARE_UART_LOG_STACK_SIZE 1024U
#define SOFTWARE_UART_RX_PRIORITY 0
#define SOFTWARE_UART_LOG_PRIORITY 5
#define SOFTWARE_UART_RX_QUEUE_DEPTH 16U

#if !DT_NODE_HAS_PROP(SOFTWARE_UART_USER_NODE, soft_uart_tx_gpios)
#error "zephyr,user must define soft-uart-tx-gpios"
#endif

struct software_uart_rx_event {
	int status;
	uint8_t byte;
};

static const struct gpio_dt_spec software_uart_tx =
	GPIO_DT_SPEC_GET(SOFTWARE_UART_USER_NODE, soft_uart_tx_gpios);
static const struct device *const software_uart_rx_port =
	DEVICE_DT_GET(SOFTWARE_UART_RX_NODE);

static struct k_thread software_uart_rx_thread;
static struct k_thread software_uart_log_thread;
K_THREAD_STACK_DEFINE(software_uart_rx_stack, SOFTWARE_UART_RX_STACK_SIZE);
K_THREAD_STACK_DEFINE(software_uart_log_stack, SOFTWARE_UART_LOG_STACK_SIZE);
K_MSGQ_DEFINE(software_uart_rx_queue, sizeof(struct software_uart_rx_event),
	      SOFTWARE_UART_RX_QUEUE_DEPTH, 4);

static atomic_t software_uart_rx_started = ATOMIC_INIT(0);
static atomic_t software_uart_rx_dropped = ATOMIC_INIT(0);

static int software_uart_send_byte(uint8_t byte)
{
	int ret;

	/* Start bit. */
	ret = gpio_pin_set_dt(&software_uart_tx, 0);
	if (ret != 0) {
		return ret;
	}
	k_busy_wait(SOFTWARE_UART_BIT_PERIOD_US);

	/* Eight data bits, least-significant bit first. */
	for (uint8_t bit = 0U; bit < 8U; bit++) {
		ret = gpio_pin_set_dt(&software_uart_tx, (byte >> bit) & 0x01U);
		if (ret != 0) {
			return ret;
		}
		k_busy_wait(SOFTWARE_UART_BIT_PERIOD_US);
	}

	/* One stop bit. */
	ret = gpio_pin_set_dt(&software_uart_tx, 1);
	if (ret != 0) {
		return ret;
	}
	k_busy_wait(SOFTWARE_UART_BIT_PERIOD_US);

	return 0;
}

static void software_uart_wait_until(uint32_t deadline)
{
	while ((int32_t)(k_cycle_get_32() - deadline) < 0) {
		/* Busy-wait against an absolute deadline to avoid timing drift. */
	}
}

static int software_uart_receive_byte(uint32_t start_cycle, uint8_t *byte)
{
	unsigned int irq_key;
	uint8_t value = 0U;
	uint32_t deadline;
	int pin_state;
	int ret = 0;

	irq_key = irq_lock();

	deadline = start_cycle +
		   k_us_to_cyc_ceil32(SOFTWARE_UART_HALF_BIT_PERIOD_US);
	software_uart_wait_until(deadline);
	pin_state = gpio_pin_get(software_uart_rx_port, SOFTWARE_UART_RX_PIN);
	if (pin_state < 0) {
		ret = pin_state;
		goto out;
	}
	if (pin_state != 0) {
		ret = -EAGAIN;
		goto out;
	}

	for (uint8_t bit = 0U; bit < 8U; bit++) {
		deadline = start_cycle + k_us_to_cyc_ceil32(
			SOFTWARE_UART_FIRST_DATA_SAMPLE_US +
			(bit * SOFTWARE_UART_BIT_PERIOD_US));
		software_uart_wait_until(deadline);

		pin_state = gpio_pin_get(software_uart_rx_port,
					 SOFTWARE_UART_RX_PIN);
		if (pin_state < 0) {
			ret = pin_state;
			goto out;
		}
		value |= (uint8_t)((uint8_t)pin_state << bit);
	}

	deadline = start_cycle +
		   k_us_to_cyc_ceil32(SOFTWARE_UART_STOP_SAMPLE_US);
	software_uart_wait_until(deadline);
	pin_state = gpio_pin_get(software_uart_rx_port, SOFTWARE_UART_RX_PIN);
	if (pin_state < 0) {
		ret = pin_state;
		goto out;
	}
	if (pin_state == 0) {
		ret = -EBADMSG;
		goto out;
	}

	*byte = value;

out:
	irq_unlock(irq_key);
	return ret;
}

static void software_uart_queue_rx_event(int status, uint8_t byte)
{
	const struct software_uart_rx_event event = {
		.status = status,
		.byte = byte,
	};

	if (k_msgq_put(&software_uart_rx_queue, &event, K_NO_WAIT) != 0) {
		atomic_inc(&software_uart_rx_dropped);
	}
}

static void software_uart_rx_thread_entry(void *arg1, void *arg2, void *arg3)
{
	uint32_t previous_cycle;
	int previous_state;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	previous_state = gpio_pin_get(software_uart_rx_port, SOFTWARE_UART_RX_PIN);
	previous_cycle = k_cycle_get_32();

	while (true) {
		uint32_t current_cycle;
		int current_state;

		k_sleep(K_USEC(SOFTWARE_UART_RX_POLL_INTERVAL_US));
		current_cycle = k_cycle_get_32();
		current_state = gpio_pin_get(software_uart_rx_port,
					     SOFTWARE_UART_RX_PIN);

		if (current_state < 0) {
			software_uart_queue_rx_event(current_state, 0U);
			k_sleep(K_MSEC(100));
			previous_state = 1;
			previous_cycle = k_cycle_get_32();
			continue;
		}

		if ((previous_state > 0) && (current_state == 0)) {
			uint32_t start_cycle = previous_cycle +
				((current_cycle - previous_cycle) / 2U);
			uint8_t byte = 0U;
			int ret;

			ret = software_uart_receive_byte(start_cycle, &byte);
			software_uart_queue_rx_event(ret, byte);

			previous_state = gpio_pin_get(software_uart_rx_port,
						      SOFTWARE_UART_RX_PIN);
			previous_cycle = k_cycle_get_32();
			continue;
		}

		previous_state = current_state;
		previous_cycle = current_cycle;
	}
}

static void software_uart_log_thread_entry(void *arg1, void *arg2, void *arg3)
{
	struct software_uart_rx_event event;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (true) {
		k_msgq_get(&software_uart_rx_queue, &event, K_FOREVER);

		if (event.status == 0) {
			if ((event.byte >= 0x20U) && (event.byte <= 0x7eU)) {
				LOG_INF("Software UART RX: 0x%02x '%c'",
					event.byte, event.byte);
			} else {
				LOG_INF("Software UART RX: 0x%02x", event.byte);
			}
		} else if (event.status != -EAGAIN) {
			LOG_ERR("Software UART RX frame failed: %d", event.status);
		}

		if (atomic_set(&software_uart_rx_dropped, 0) != 0) {
			LOG_WRN("Software UART RX queue overflowed");
		}
	}
}

int software_uart_receive_start(void)
{
	int ret;

	if (!device_is_ready(software_uart_rx_port)) {
		LOG_ERR("Software UART RX GPIO port is not ready");
		return -ENODEV;
	}

	if (!atomic_cas(&software_uart_rx_started, 0, 1)) {
		return 0;
	}

	ret = gpio_pin_configure(software_uart_rx_port, SOFTWARE_UART_RX_PIN,
				 GPIO_INPUT | GPIO_PULL_UP);
	if (ret != 0) {
		LOG_ERR("Software UART RX configuration failed: %d", ret);
		atomic_clear(&software_uart_rx_started);
		return ret;
	}

	k_thread_create(&software_uart_log_thread, software_uart_log_stack,
			K_THREAD_STACK_SIZEOF(software_uart_log_stack),
			software_uart_log_thread_entry, NULL, NULL, NULL,
			K_PRIO_PREEMPT(SOFTWARE_UART_LOG_PRIORITY), 0, K_NO_WAIT);
	k_thread_name_set(&software_uart_log_thread, "software_uart_log");

	k_thread_create(&software_uart_rx_thread, software_uart_rx_stack,
			K_THREAD_STACK_SIZEOF(software_uart_rx_stack),
			software_uart_rx_thread_entry, NULL, NULL, NULL,
			K_PRIO_PREEMPT(SOFTWARE_UART_RX_PRIORITY), 0, K_NO_WAIT);
	k_thread_name_set(&software_uart_rx_thread, "software_uart_rx");

	LOG_INF("Software UART RX polling P2.6 at %u baud, 8-N-1",
		SOFTWARE_UART_BAUD_RATE);
	return 0;
}

int software_uart_send_hello(void)
{
	static const uint8_t message[] = {'H', 'E', 'L', 'L', 'O'};
	unsigned int irq_key;
	int ret = 0;

	if (!gpio_is_ready_dt(&software_uart_tx)) {
		LOG_ERR("Software UART TX GPIO is not ready");
		return -ENODEV;
	}

	/* UART is non-inverted and idles high. */
	ret = gpio_pin_configure_dt(&software_uart_tx, GPIO_OUTPUT_HIGH);
	if (ret != 0) {
		LOG_ERR("Software UART TX configuration failed: %d", ret);
		return ret;
	}
	k_busy_wait(SOFTWARE_UART_BIT_PERIOD_US);

	/* Prevent interrupt latency from changing bit timing. */
	irq_key = irq_lock();
	for (size_t i = 0U; i < sizeof(message); i++) {
		ret = software_uart_send_byte(message[i]);
		if (ret != 0) {
			break;
		}
	}

	/* Always return the line to the idle state before enabling IRQs. */
	(void)gpio_pin_set_dt(&software_uart_tx, 1);
	irq_unlock(irq_key);

	if (ret != 0) {
		LOG_ERR("Software UART transmission failed: %d", ret);
		return ret;
	}

	LOG_INF("Sent HELLO on P2.7 at %u baud, 8-N-1",
		SOFTWARE_UART_BAUD_RATE);

	return software_uart_receive_start();
}
