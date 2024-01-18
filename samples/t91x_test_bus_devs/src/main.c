/*
 * Copyright (c) 2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/flash.h>

/* 1000 msec = 1 sec */
#define SLEEP_TIME_MS   1000

/*
 * A build error on this line means your board is unsupported.
 * See the sample documentation for information on how to fix this.
 */
static const struct gpio_dt_spec leds[] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios),
};

#define FLASH_NODE

const struct device *bme680 = DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(bosch_bme680));
const struct device *bmi270 = DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(bosch_bmi270));
const struct device *bmm150 = DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(bosch_bmm150));
const struct device *adxl367 = DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(adi_adxl367));
const struct device *pmic_wifi = DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(nordic_npm6001));
const struct device *pmic_main = DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(nordic_npm1300));
const struct device *const flash_dev = DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(jedec_spi_nor));

int main(void)
{
	int ret;
	uint8_t id[3];
	const struct device *sensors[] = {
		pmic_main,
		bme680,
		bmi270,
		bmm150,
		adxl367,
		pmic_wifi,
	};

	/* SPI FLASH */
	ret = flash_read_jedec_id(flash_dev, id);
	if (ret == 0) {
		printf("spi-flash jedec-id = [%02x %02x %02x];\n",
		       id[0], id[1], id[2]);
	} else {
		printf("JEDEC ID read failed: %d\n", ret);
	}

	/* SENSORS */
	for (size_t i = 0; i < ARRAY_SIZE(sensors); i++) {
		if (!device_is_ready(sensors[i])) {
			printk("sensor: device %s NOT ready.\n", sensors[i]->name);
		} else {
			printk("sensor: device %s ready.\n", sensors[i]->name);
		}
	}

	/* LEDS */
	for (size_t i = 0; i < ARRAY_SIZE(leds); ++i) {
		if (!gpio_is_ready_dt(&leds[i])) {
			return 0;
		}

		ret = gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_ACTIVE);
		if (ret < 0) {
			return 0;
		}

	}

	while (1) {
		for (size_t i = 0; i < ARRAY_SIZE(leds); ++i) {
			ret = gpio_pin_toggle_dt(&leds[i]);
			if (ret < 0) {
				return 0;
			}
		}
		k_msleep(SLEEP_TIME_MS);
	}
	return 0;
}
