/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2023  Intel Corporation
 *
 *
 */

#define BTD_DEVICE_SET_INTERFACE	"org.bluez.DeviceSet1"

void btd_set_add_device(struct btd_device *device, uint8_t *ltk,
				uint8_t sirk[16], uint8_t size, uint8_t rank);
