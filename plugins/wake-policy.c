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

#include "src/plugin.h"
#include "src/adapter.h"
#include "src/device.h"
#include "src/log.h"
#include "src/textfile.h"

static GKeyFile * key_file;

static gboolean is_allowed_to_wake(const char *major, guint minor)
{
	guint *minor_list;
	gsize length;
	gboolean allowed = false;
	GError *gerr = NULL;

	if (!g_key_file_has_key(key_file, "WakeAllowed", major, NULL))
		return true;

	allowed = g_key_file_get_boolean(key_file, "WakeAllowed", major, &gerr);
	if (!gerr)
		return allowed;

	g_error_free(gerr);
	gerr = NULL;

	minor_list = (guint *)g_key_file_get_integer_list(key_file,
						"WakeAllowed",
						major, &length, &gerr);
	if (gerr) {
		DBG("Failed to get allowed minor list for %s", major);
		return false;
	}

	for (gsize i = 0; i < length; i++) {
		if (minor_list[i] == minor) {
			allowed = true;
			break;
		}
	}

	return allowed;
}

static const struct {
	uint8_t val;
	const char *str;
} major_class_table[] = {
	{ 0x00, "Miscellaneous"	},
	{ 0x01, "Computer"	},
	{ 0x02, "Phone"		},
	{ 0x03, "LAN/Network"	},
	{ 0x04, "Audio/Video"	},
	{ 0x05, "Peripheral"	},
	{ 0x06, "Imaging"	},
	{ 0x07, "Wearable"	},
	{ 0x08, "Toy"		},
	{ 0x09, "Health"	},
	{ 0x1f, "Uncategorized"	},
	{ }
};

static gboolean is_class_allowed_to_wake(uint32_t class)
{
	uint8_t major = (class & 0x1f00) >> 8;
	uint8_t minor = (class & 0x00fc) >> 2;

	if ((major >= 0x01 && major <= 0x09) || major == 0x1f)
		return is_allowed_to_wake(major_class_table[major].str, minor);

	return true;
}

static void wake_policy_device_resolved(struct btd_adapter *adapter,
				     struct btd_device *device)
{
	char *filename;
	GKeyFile *device_key_file;
	GError *gerr = NULL;

	if (key_file == NULL)
		return;

	// Does device support to wake the host?
	if (!device_get_wake_support(device))
		return;

	// Check if WakeAllowed has already been stored,
	// if yes do not change it
	filename = btd_device_get_storage_path(device, "info");
	device_key_file = g_key_file_new();
	if (!g_key_file_load_from_file(device_key_file, filename, 0, &gerr)) {
		error("Unable to load key file from %s: (%s)", filename,
							gerr->message);
		g_clear_error(&gerr);
	} else {
		if (g_key_file_has_key(device_key_file, "General",
					"WakeAllowed", NULL)) {
			DBG("%s WakeAllowed already stored",
				device_get_path(device));
			return;
		}
	}
	g_key_file_free(device_key_file);
	g_free(filename);

	// Check if Class of Device is allowed to wake up the host
	if (!is_class_allowed_to_wake(btd_device_get_class(device))) {
		DBG("%s Force WakeAllowed to false", device_get_path(device));
		device_set_wake_override(device, false);
		device_set_wake_allowed(device, false, -1U);
	}
}

static int wake_policy_probe(struct btd_adapter *adapter)
{
	GError *gerr = NULL;

	DBG("");
	key_file = g_key_file_new();
	if (!g_key_file_load_from_file(key_file,
					CONFIGDIR "/wake-policy.conf",
					0,
					&gerr)) {
		error("Unable to load key file from %s: (%s)",
			CONFIGDIR "/wake-policy.conf",
			gerr->message);
		g_clear_error(&gerr);
		g_key_file_free(key_file);
		key_file = NULL;
	}

	return 0;
}

static void wake_policy_remove(struct btd_adapter *adapter)
{
	DBG("");
	if (key_file)
		g_key_file_free(key_file);
}

static struct btd_adapter_driver wake_policy_driver = {
	.name	= "wake-policy",
	.probe	= wake_policy_probe,
	.remove	= wake_policy_remove,
	.device_resolved = wake_policy_device_resolved,
};

static int wake_policy_init(void)
{
	return btd_register_adapter_driver(&wake_policy_driver);
}

static void wake_policy_exit(void)
{
	btd_unregister_adapter_driver(&wake_policy_driver);
}

BLUETOOTH_PLUGIN_DEFINE(wake_policy, VERSION, BLUETOOTH_PLUGIN_PRIORITY_LOW,
			wake_policy_init, wake_policy_exit)
