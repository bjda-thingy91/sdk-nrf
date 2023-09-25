#
# Copyright (c) 2018-2020 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

add_subdirectory_ifdef(CONFIG_SENSOR sensor)
add_subdirectory_ifdef(CONFIG_NETWORKING net)

if (CONFIG_MPSL AND NOT CONFIG_MPSL_FEM_ONLY)
  add_subdirectory(mpsl)
endif()

add_subdirectory(hw_cc3xx)
add_subdirectory(entropy)
add_subdirectory(serial)
add_subdirectory_ifdef(CONFIG_DP_DRIVER dp)
add_subdirectory_ifdef(CONFIG_I2C_VIRTUAL_EEPROM_DRIVER i2c_virtual_eeprom)
add_subdirectory_ifdef(CONFIG_WIFI wifi)
