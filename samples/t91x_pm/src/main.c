#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/eeprom.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/mfd/npm1300.h>
#include <zephyr/drivers/regulator.h>

#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/ethernet_mgmt.h>

#include <zephyr/pm/device.h>

#include <modem/nrf_modem_lib.h>

#include <zephyr/logging/log.h>
#define MODULE main
LOG_MODULE_REGISTER(MODULE);

/* 1000 msec = 1 sec */
#define SLEEP_TIME_MS   1000

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static const struct device *pmic_main = DEVICE_DT_GET(DT_NODELABEL(pmic_main));
static const struct device *eeprom = DEVICE_DT_GET(DT_NODELABEL(eeprom));

static const struct device *ldsw_wifi = DEVICE_DT_GET(DT_NODELABEL(ldsw_npm60_en));
static const struct device *ldsw_sensors = DEVICE_DT_GET(DT_NODELABEL(ldsw_sensors));
static const struct device *buck2 = DEVICE_DT_GET(DT_NODELABEL(reg_3v3));
const struct gpio_dt_spec short_range_frontend_enable = {
	.dt_flags = DT_GPIO_HOG_FLAGS_BY_IDX(DT_NODELABEL(ldsw_rf_fe_sr_en), 0),
	.pin = DT_GPIO_HOG_PIN_BY_IDX(DT_NODELABEL(ldsw_rf_fe_sr_en), 0),
	.port = DEVICE_DT_GET(DT_PARENT(DT_NODELABEL(ldsw_rf_fe_sr_en))),
};

static const struct device *uart0_dev = DEVICE_DT_GET(DT_NODELABEL(uart0));
static const struct device *pwm_dev = DEVICE_DT_GET(DT_NODELABEL(pwm0));
static const struct device *i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c2));
static const struct device *spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi3));


K_SEM_DEFINE(usb_host_power, 1, 1);

#define RUN_PM_ACTION_ON_DEVICE(dev, action) \
	ret = pm_device_action_run(dev, action); \
	if (ret) { \
		LOG_ERR("Failed to run action %d on device %s, err: %d", action, dev->name, ret); \
	}

#define CONFIGURE_LED(alias, mode) \
	const struct gpio_dt_spec alias##_gpio = GPIO_DT_SPEC_GET(DT_ALIAS(alias), gpios); \
	if (gpio_is_ready_dt(&alias##_gpio) || gpio_pin_configure_dt(&alias##_gpio, mode)) { \
		LOG_ERR("Failed to configure LED GPIO!"); \
	}

#define PM_DEVICES \
	X(uart0_dev) \
	X(pwm_dev) \
	X(i2c_dev) \
	X(spi_dev)

#define LED_ALIASES \
	X(led0) \
	X(led1) \
	X(led2)

static bool host_connected = false;
static bool vbus_connected = false;
static bool power_save = false;

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
			k_sem_give(&usb_host_power);
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

void enter_power_save(void)
{
	int ret = 0;
	// TODO: uninit ADXL367
	// TODO: uninit BMM150
	// TODO: uninit BMI270
	// TODO: uninit BME688
	/* DISABLING SENSOR RAIL MAKES MATTERS WORSE AT v0.10.0
	ret = regulator_disable(ldsw_sensors);
	if (ret) {
		LOG_ERR("Cannot turn off sensors ldsw, err: %d", ret);
	}
	*/



	// uninit wifi
	struct net_if *iface = net_if_get_first_wifi();
	if (iface && net_if_is_admin_up(iface)) {
		ret = net_if_down(iface);
		if (ret) {
			LOG_ERR("Cannot bring down wifi iface, err: %d", ret);
		}
	}

	// cut off power to wifi
	ret = regulator_disable(ldsw_wifi);
	if (ret) {
		LOG_ERR("Cannot turn off wifi ldsw, err: %d", ret);
	} else {
		LOG_INF("wifi ldsw disabled");
	}

	if (!gpio_is_ready_dt(&short_range_frontend_enable)) {
		LOG_ERR("GPIO not available!");
	} else {
		LOG_INF("GPIO OK");
	}

	if (gpio_pin_set_dt(&short_range_frontend_enable, 0)) {
		LOG_ERR("cannot set GPIO");
	} else {
		LOG_INF("GPIO set ok");
	}

	#define X(dev) RUN_PM_ACTION_ON_DEVICE(dev, PM_DEVICE_ACTION_SUSPEND)
		PM_DEVICES
	#undef X

	#define X(alias) CONFIGURE_LED(alias, GPIO_INPUT)
		LED_ALIASES
	#undef X
}

void exit_power_save(void)
{
	int ret = 0;

	#define X(dev) RUN_PM_ACTION_ON_DEVICE(dev, PM_DEVICE_ACTION_RESUME)
		PM_DEVICES
	#undef X

	#define X(alias) CONFIGURE_LED(alias, GPIO_OUTPUT_INACTIVE)
		LED_ALIASES
	#undef X

	ret = regulator_enable(ldsw_sensors);
	if (ret) {
		LOG_ERR("couldn't turn on sensors ldsw, err: %d", ret);
	}
	// TODO: init ADXL367
	// TODO: init BMM150
	// TODO: init BMI270
	// TODO: init BME688

	// power on wifi
	ret = regulator_enable(ldsw_wifi);
	if (ret) {
		LOG_ERR("couldn't turn on wifi ldsw, err: %d", ret);
	}
	// init wifi
	struct net_if *iface = net_if_get_first_wifi();
	if (iface && !net_if_is_admin_up(iface)) {
		ret = net_if_up(iface);
		if (ret) {
			LOG_ERR("Cannot bring up wifi iface, err: %d", ret);
		}
	}
}

int main(void)
{
	nrf_modem_lib_init(); // actually saves a lot of power
	enter_power_save();
}
