// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2024  Intel Corporation
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>

#include <glib.h>
#include <dbus/dbus.h>
#include "gdbus/gdbus.h"

#include "src/log.h"
#include "src/dbus-common.h"
#include "src/error.h"
#include "telephony.h"

#define BLUEZ_TELEPHONY_INTERFACE "org.bluez.telephonyCtrl"

struct call_callback {
	const struct telephony_control_callback *cbs;
	void *user_data;
};

void telephony_update_call_Info(struct telephony_ctrl *tc)
{
	DBG("");
	g_dbus_emit_property_changed(btd_get_dbus_connection(), tc->path,
				     BLUEZ_TELEPHONY_INTERFACE, "call_state");
}

static DBusMessage *telephony_answer_call(DBusConnection *conn,
					  DBusMessage *msg, void *data)
{
	struct telephony_ctrl *tc = data;
	struct call_callback *cb = tc->cb;
	int err;

	DBG("");
	if (!cb->cbs->call_answer)
		return btd_error_not_supported(msg);

	err = cb->cbs->call_answer(tc, cb->user_data);
	if (err < 0)
		return btd_error_failed(msg, strerror(-err));

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static DBusMessage *telephony_reject_call(DBusConnection *conn,
					  DBusMessage *msg, void *data)
{
	struct telephony_ctrl *tc = data;
	struct call_callback *cb = tc->cb;
	int err;

	DBG("");

	if (!cb->cbs->call_reject)
		return btd_error_not_supported(msg);

	err = cb->cbs->call_reject(tc, cb->user_data);
	if (err < 0)
		return btd_error_failed(msg, strerror(-err));

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static gboolean ccp_get_index(const GDBusPropertyTable *property,
			      DBusMessageIter *iter, void *data)
{
	struct telephony_ctrl *tc = data;
	uint32_t index = tc->call_index;

	DBG("");

	dbus_message_iter_append_basic(iter, DBUS_TYPE_UINT32, &index);

	return TRUE;
}

static const GDBusSignalTable telephony_signals[] = {
};

/* methods exposed to client to perform call operations */
static const GDBusMethodTable telephony_methods[] = {
	{ GDBUS_METHOD("answer", NULL, NULL, telephony_answer_call) },
	{ GDBUS_METHOD("reject", NULL, NULL, telephony_reject_call) },
	{ }
};

/*
 * Inform registered clients on property changed events
 * use g_dbus_emit_property_changed() API
 */
static const GDBusPropertyTable telephony_properties[] = {
	{ "call_state", "u", ccp_get_index, NULL, NULL },
	{ }
};

void telephony_destroy_device(struct telephony_ctrl *tc)
{
	DBG("%s", tc->path);

	g_dbus_unregister_interface(btd_get_dbus_connection(),
				    tc->path, BLUEZ_TELEPHONY_INTERFACE);

	if (tc->path)
		g_free(tc->cb);
	if (tc->path)
		g_free(tc->path);
	if (tc->device)
		g_free(tc->device);

	if (tc)
		g_free(tc);
}

struct telephony_ctrl *telephony_create_device(const char *path, uint16_t id)
{
	struct telephony_ctrl *tc;

	DBG("");
	tc = g_new0(struct telephony_ctrl, 1);
	tc->device = g_strdup(path);
	tc->path = g_strdup_printf("%s/Caller%u", path, id);

	if (!g_dbus_register_interface(btd_get_dbus_connection(),
				       tc->path, BLUEZ_TELEPHONY_INTERFACE,
				       telephony_methods,
				       telephony_signals,
				       telephony_properties, tc, NULL)) {
		error("D-Bus failed to register %s path", tc->path);
		telephony_destroy_device(tc);
		return NULL;
	}

	DBG("%s", tc->path);

	return tc;
}

void telephony_set_callbacks(struct telephony_ctrl *tp,
			     const struct telephony_control_callback *cbs,
			     void *user_data)
{
	struct call_callback *cb;

	DBG("");

	if (tp->cb)
		g_free(tp->cb);

	cb = g_new0(struct call_callback, 1);
	cb->cbs = cbs;
	cb->user_data = user_data;

	tp->cb = cb;
}
