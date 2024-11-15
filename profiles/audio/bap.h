/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright 2024 NXP
 *
 */

typedef void (*bap_stream_cb_t)(uint8_t bis, uint8_t sgrp,
		struct iovec *caps, struct iovec *meta,
		struct bt_iso_qos *qos, void *user_data);

struct bt_bap *bap_get_session(struct btd_device *device);
void bap_scan_delegator_probe(struct btd_device *device);

bool parse_base(struct bt_iso_base *base, struct bt_iso_qos *qos,
		util_debug_func_t func, bap_stream_cb_t handler,
		void *user_data);
