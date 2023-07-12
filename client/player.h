// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2020 Intel Corporation. All rights reserved.
 *
 *
 */

void player_add_submenu(void);
void player_remove_submenu(void);
void player_add_bcast_source(GDBusProxy *proxy,
		uint8_t *service_data, int len);
void player_remove_bcast_source(GDBusProxy *proxy);
