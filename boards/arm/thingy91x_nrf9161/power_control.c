/*
 * Copyright (c) 2023 Nordic Semiconductor ASA.
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/sys/util_macro.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/drivers/regulator.h>

#include <soc.h>

LOG_MODULE_REGISTER(thingy91x_nrf9161, CONFIG_LOG_DEFAULT_LEVEL);

const struct gpio_dt_spec short_range_frontend_enable = {
	.dt_flags = DT_GPIO_HOG_FLAGS_BY_IDX(DT_NODELABEL(ldsw_rf_fe_sr_en), 0),
	.pin = DT_GPIO_HOG_PIN_BY_IDX(DT_NODELABEL(ldsw_rf_fe_sr_en), 0),
	.port = DEVICE_DT_GET(DT_PARENT(DT_NODELABEL(ldsw_rf_fe_sr_en))),
};

static const struct device *ldsw_wifi = DEVICE_DT_GET(DT_NODELABEL(ldsw_npm60_en));

int power_on_wifi(void)
{
	int ret;

	// power on wifi
	ret = regulator_enable(ldsw_wifi);
	if (ret) {
		LOG_ERR("couldn't turn on wifi ldsw, err: %d", ret);
		return ret;
	}
#ifdef CONFIG_WIFI
	// init wifi
	struct net_if *iface = net_if_get_first_wifi();
	if (iface && !net_if_is_admin_up(iface)) {
		ret = net_if_up(iface);
		if (ret) {
			LOG_ERR("Cannot bring up wifi iface, err: %d", ret);
		}
	}
#endif /* CONFIG_WIFI */
	return ret;
}

int power_off_wifi(void)
{
	int ret = 0;

#ifdef CONFIG_WIFI
	// uninit wifi
	struct net_if *iface = net_if_get_first_wifi();
	if (iface && net_if_is_admin_up(iface)) {
		ret = net_if_down(iface);
		if (ret) {
			LOG_ERR("Cannot bring down wifi iface, err: %d", ret);
			return ret;
		}
	}
#endif /* CONFIG_WIFI */

	// cut off power to wifi after init
	ret = regulator_disable(ldsw_wifi);
	if (ret) {
		LOG_ERR("Cannot turn off wifi ldsw, err: %d", ret);
	}

	return ret;
}

int power_on_2_4_ghz_switch(void)
{
	return gpio_pin_set_dt(&short_range_frontend_enable, 1);
}

int power_off_2_4_ghz_switch(void)
{
	return gpio_pin_set_dt(&short_range_frontend_enable, 0);
}

static int board_config(void)
{
	/* to save power in the range of milliAmps, turn off WiFi by default */
	power_off_wifi();
	power_off_2_4_ghz_switch();
	return 0;
}

SYS_INIT(board_config, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
