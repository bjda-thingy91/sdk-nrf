#
# Copyright (c) 2023 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

zephyr_library()
zephyr_library_sources(CONFIG_I2C_VIRTUAL_EEPROM_DRIVER virtual_eeprom.c)

