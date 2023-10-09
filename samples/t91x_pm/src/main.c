#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/eeprom.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/mfd/npm1300.h>

#include <zephyr/logging/log.h>
#define MODULE main
LOG_MODULE_REGISTER(MODULE);

/* 1000 msec = 1 sec */
#define SLEEP_TIME_MS   1000

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_vbus = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec led_usb = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);

static const struct device *pmic_main = DEVICE_DT_GET(DT_NODELABEL(pmic_main));
static const struct device *eeprom = DEVICE_DT_GET(DT_NODELABEL(eeprom));

static bool host_connected = false;
static bool vbus_connected = false;

#define HOST_CHECK_TIMEOUT K_MSEC(100)

void check_usb_connected_work_handler(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(check_usb_connected_work, check_usb_connected_work_handler);

void check_usb_connected_work_handler(struct k_work *work)
{
	int ret;
	uint16_t usb_status;

	ret = eeprom_read(eeprom, 0x0, (void*)&usb_status, sizeof(usb_status));
	if (!ret) {
		LOG_ERR("USB STATUS: %d", usb_status);
		if (usb_status == USB_DC_CONFIGURED) {
			host_connected = true;
		}
	}
	if (!host_connected && vbus_connected) {
		k_work_schedule(&check_usb_connected_work, HOST_CHECK_TIMEOUT);
	}
}

void pmic_callback_handler(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins)
{
	if (pins & BIT(NPM1300_EVENT_VBUS_DETECTED)) {
		vbus_connected = true;
		k_work_schedule(&check_usb_connected_work, HOST_CHECK_TIMEOUT);
	}
	if (pins & BIT(NPM1300_EVENT_VBUS_REMOVED)) {
		host_connected = false;
		vbus_connected = false;
	}
	LOG_ERR("pmic_callback_handler 0x%X", pins);
}

int main(void)
{
	int ret = 0;
	uint8_t reg = 0;

	if (!gpio_is_ready_dt(&led)) {
		LOG_ERR("gpio_is_ready_dt");
		return 0;
	}

	ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		LOG_ERR("gpio_pin_configure_dt(led) err: %d", ret);
		return 0;
	}

	ret = gpio_pin_configure_dt(&led_vbus, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("gpio_pin_configure_dt(led_vbus) err: %d", ret);
		return 0;
	}

	ret = gpio_pin_configure_dt(&led_usb, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("gpio_pin_configure_dt(led_usb) err: %d", ret);
		return 0;
	}

	struct gpio_callback pmic_callback = {
		.pin_mask = 0xFF,
		.handler = pmic_callback_handler,
	};

	ret = mfd_npm1300_add_callback(pmic_main, &pmic_callback);
	if (ret) {
		LOG_ERR("mfd_npm1300_add_callback err: %d", ret);
		return 0;
	}

	/* initial status check without interrupts */
	ret = mfd_npm1300_reg_read(pmic_main, 0x02, 0x07, &reg);
	if (!ret && (reg & BIT(0))) {
		vbus_connected = true;
	}
	k_work_schedule(&check_usb_connected_work, HOST_CHECK_TIMEOUT);

	while (1) {
		gpio_pin_set_dt(&led_usb, host_connected);
		gpio_pin_set_dt(&led_vbus, vbus_connected);
		ret = gpio_pin_toggle_dt(&led);
		if (ret < 0) {
			return 0;
		}
		k_msleep(SLEEP_TIME_MS);
	}
	return 0;
}
