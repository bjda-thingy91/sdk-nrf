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

#define ID_DAP_VENDOR14 (ID_DAP_VENDOR0 + 14)
#define ID_DAP_VENDOR15 (ID_DAP_VENDOR0 + 15)
#define ID_DAP_VENDOR_BOOTLOADER ID_DAP_VENDOR14
#define ID_DAP_VENDOR_NRF91_BOOTLOADER ID_DAP_VENDOR15

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(dap_handler, CONFIG_DAP_LOG_LEVEL);

static const struct gpio_dt_spec reset_pin =
	GPIO_DT_SPEC_GET_OR(DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_swdp_gpio), reset_gpios, {});
static const struct gpio_dt_spec button1_pin =
	GPIO_DT_SPEC_GET_OR(DT_PATH(zephyr_user), button1_gpios, {});

static void nrf91_reset_work_handler(struct k_work *work);
static void nrf91_bootloader_work_handler(struct k_work *work);
static void turn_off_led3_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(nrf91_reset_work, nrf91_reset_work_handler);
K_WORK_DELAYABLE_DEFINE(nrf91_bootloader_work, nrf91_bootloader_work_handler);
K_WORK_DELAYABLE_DEFINE(turn_off_led3_work, turn_off_led3_work_handler);

static int dap_vendor_handler(const uint8_t *request, uint8_t *response)
{
#ifdef CONFIG_RETENTION_BOOT_MODE
	if (*request == ID_DAP_VENDOR_BOOTLOADER) {
		bootmode_set(BOOT_MODE_TYPE_BOOTLOADER);
		sys_reboot(SYS_REBOOT_WARM);
		/* no return from here */
	}
#endif /* CONFIG_RETENTION_BOOT_MODE */
	if (*request == ID_DAP_VENDOR_NRF91_BOOTLOADER
		&& device_is_ready(button1_pin.port)
		&& device_is_ready(reset_pin.port))
	{
		if (!k_work_busy_get(&nrf91_bootloader_work.work)) {
			k_work_reschedule(&nrf91_bootloader_work, K_NO_WAIT);
		}
		response[0] = DAP_OK;
		return 1U;
	}
	response[0] = ID_DAP_INVALID;
	return 1U;
}

static void button_handler(uint32_t button_state, uint32_t has_changed)
{
	uint32_t button = button_state & has_changed;
	if (button & DK_BTN1_MSK) {
		if (!k_work_busy_get(&nrf91_reset_work.work)) {
			k_work_reschedule(&nrf91_reset_work, K_NO_WAIT);
		}
	}
}

static void nrf91_reset_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	gpio_pin_set_dt(&reset_pin, 0);
	k_sleep(K_MSEC(100));
	gpio_pin_set_dt(&reset_pin, 1);
}

static void nrf91_bootloader_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	gpio_pin_set_dt(&reset_pin, 0);
	gpio_pin_set_dt(&button1_pin, 0);
	k_sleep(K_MSEC(100));
	gpio_pin_set_dt(&reset_pin, 1);
	k_sleep(K_MSEC(1000));
	gpio_pin_set_dt(&button1_pin, 1);
}

static void turn_off_led3_work_handler(struct k_work *work)
{
	dk_set_led_off(DK_LED3);
}

static int dap_handler_loop(void)
{
	int ret;

	dk_leds_init();

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
		dk_set_led_on(DK_LED3);
		k_work_reschedule(&turn_off_led3_work, K_MSEC(100));
	}
	return 0;
}

K_THREAD_DEFINE(dap_handler_thread, CONFIG_DAP_HANDLER_STACK_SIZE,
		dap_handler_loop, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
