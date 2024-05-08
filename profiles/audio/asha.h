// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2024  Asymptotic Inc.
 *
 *  Author: Arun Raghavan <arun@asymptotic.io>
 *
 *
 */

#include <stdbool.h>
#include <stdint.h>

struct asha_device;

typedef enum {
	ASHA_STOPPED = 0,
	ASHA_STARTING,
	ASHA_STARTED,
	ASHA_STOPPING,
} asha_state_t;

typedef void (*asha_cb_t)(int status, void *data);

uint16_t asha_device_get_render_delay(struct asha_device *asha);
asha_state_t asha_device_get_state(struct asha_device *asha);
int asha_device_get_fd(struct asha_device *asha);
uint16_t asha_device_get_mtu(struct asha_device *asha);

unsigned int asha_device_start(struct asha_device *asha, asha_cb_t cb,
		void *user_data);
unsigned int asha_device_stop(struct asha_device *asha, asha_cb_t cb,
		void *user_data);

int8_t asha_device_get_volume(struct asha_device *asha);
bool asha_device_set_volume(struct asha_device *asha, int8_t volume);
