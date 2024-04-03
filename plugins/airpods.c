// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2024 Frédéric Danis <frederic.danis@collabora.com>
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdbool.h>

#include <glib.h>

#include "bluetooth/bluetooth.h"

#include "lib/mgmt.h"
#include "src/plugin.h"
#include "src/adapter.h"
#include "src/device.h"
#include "src/eir.h"
#include "src/log.h"
#include "src/shared/mgmt.h"
#include "src/shared/util.h"

#define APPLE_INC_VENDOR_ID 0x004c

static struct mgmt *mgmt;

static bool eir_msd_is_apple_inc(GSList *msd_list)
{
	GSList *msd_l, *msd_next;

	for (msd_l = msd_list; msd_l != NULL; msd_l = msd_next) {
		const struct eir_msd *msd = msd_l->data;

		msd_next = g_slist_next(msd_l);

		if (msd->company == APPLE_INC_VENDOR_ID)
			return true;
	}

	return false;
}

static void airpods_device_found_callback(uint16_t index, uint16_t length,
					const void *param, void *user_data)
{
	struct btd_device *dev;
	const struct mgmt_ev_device_found *ev = param;
	struct btd_adapter *adapter = user_data;
	uint16_t eir_len;
	uint32_t flags = le32_to_cpu(ev->flags);
	struct eir_data eir_data;

	dev = btd_adapter_find_device(adapter,  &ev->addr.bdaddr,
					ev->addr.type);
	if (!dev)
		return;

	if (length < sizeof(*ev)) {
		warn("Too short device found event (%u bytes)", length);
		return;
	}

	eir_len = btohs(ev->eir_len);
	if (length != sizeof(*ev) + eir_len) {
		warn("Device found event size mismatch (%u != %zu)",
					length, sizeof(*ev) + eir_len);
		return;
	}

	if (eir_len == 0)
		return;

	memset(&eir_data, 0, sizeof(eir_data));
	eir_parse(&eir_data, ev->eir, eir_len);

	if (eir_msd_is_apple_inc(eir_data.msd_list) &&
				(flags & MGMT_DEV_FOUND_NOT_CONNECTABLE) &&
				(ev->addr.type == BDADDR_LE_PUBLIC)) {
		DBG("Force BREDR last seen");
		device_set_bredr_support(dev);
		device_update_last_seen(dev, BDADDR_BREDR, true);
	}
}

static int airpods_probe(struct btd_adapter *adapter)
{
	if (!mgmt)
		mgmt = mgmt_new_default();

	if (!mgmt) {
		fprintf(stderr, "Failed to open management socket\n");
		return 0;
	}

	mgmt_register(mgmt, MGMT_EV_DEVICE_FOUND,
					btd_adapter_get_index(adapter),
					airpods_device_found_callback,
					adapter, NULL);

	return 0;
}

static void airpods_remove(struct btd_adapter *adapter)
{
	mgmt_unregister_index(mgmt, btd_adapter_get_index(adapter));
}

static struct btd_adapter_driver airpods_driver = {
	.name	= "airpods",
	.probe	= airpods_probe,
	.remove	= airpods_remove,
};

static int airpods_init(void)
{
	return btd_register_adapter_driver(&airpods_driver);
}

static void airpods_exit(void)
{
	btd_unregister_adapter_driver(&airpods_driver);
}

BLUETOOTH_PLUGIN_DEFINE(airpods, VERSION,
		BLUETOOTH_PLUGIN_PRIORITY_LOW, airpods_init, airpods_exit)
