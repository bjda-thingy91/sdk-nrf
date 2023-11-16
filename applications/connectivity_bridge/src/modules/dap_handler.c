/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */


#include <zephyr/usb/usb_device.h>
#include <zephyr/kernel.h>

#include <cmsis_dap.h>
#include "cmsis_dap_usb.h"

#include <zephyr/retention/bootmode.h>
#include <zephyr/sys/reboot.h>

#include <zephyr/drivers/gpio.h>

#include <dk_buttons_and_leds.h>
//todo: reset button and LED indicator

#define ID_DAP_VENDOR14 (ID_DAP_VENDOR0 + 14)
#define ID_DAP_VENDOR_BOOTLOADER ID_DAP_VENDOR14

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(dap_handler, CONFIG_DAP_LOG_LEVEL);

static const struct gpio_dt_spec reset_pin =
	GPIO_DT_SPEC_GET_OR(DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_swdp_gpio), reset_gpios, {});

static int dap_vendor_handler(const uint8_t *request, uint8_t *response)
{
#ifdef CONFIG_RETENTION_BOOT_MODE
	if (*request == ID_DAP_VENDOR_BOOTLOADER) {
		bootmode_set(BOOT_MODE_TYPE_BOOTLOADER);
		sys_reboot(SYS_REBOOT_WARM);
		/* no return from here */
	}
#endif /* CONFIG_RETENTION_BOOT_MODE */
	response[0] = ID_DAP_INVALID;
	return 1U;
}

static void button_handler(uint32_t button_state, uint32_t has_changed)
{
	uint32_t button = button_state & has_changed;
	if (button & DK_BTN1_MSK) {
		gpio_pin_set_dt(&reset_pin, 0);
		k_sleep(K_MSEC(100));
		gpio_pin_set_dt(&reset_pin, 1);
	}
}

static int dap_handler_loop(void)
{
	int ret;

	if (device_is_ready(reset_pin.port)) {
		/* reset button emulation requires a physical reset pin for now */
		/* todo: figure out software/debug reset CMSIS-DAP commands */
		ret = dk_buttons_init(button_handler);
		if (ret) {
			LOG_ERR("Failed to init button handler");
		}
	}


	const struct device *const swd_dev = DEVICE_DT_GET_ONE(zephyr_swdp_gpio);

	(void) dap_install_vendor_callback(dap_vendor_handler);

	ret = cmsis_dap_usb_init(swd_dev);
	if (ret != 0) {
		LOG_ERR("Failed to init CMSIS-DAP");
		return 0;
	}

	while (true)
	{
		cmsis_dap_usb_process(K_FOREVER);
	}
	return 0;
}

K_THREAD_DEFINE(dap_handler_thread, CONFIG_DAP_HANDLER_STACK_SIZE,
		dap_handler_loop, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
