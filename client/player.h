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

#ifdef MICP_MICS_PTS_FLAG
void mics_set_proxy(void *proxy);
#endif /*MICP_MICS_PTS_FLAG*/
