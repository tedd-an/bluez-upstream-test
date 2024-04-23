// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2024  Intel Corporation. All rights reserved.
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include "gdbus/gdbus.h"
#include "lib/bluetooth.h"
#include "src/shared/shell.h"
#include "print.h"
#include "telephony_client.h"

/* String display constants */
#define COLORED_NEW	COLOR_GREEN "NEW" COLOR_OFF
#define COLORED_CHG	COLOR_YELLOW "CHG" COLOR_OFF

#define BLUEZ_TELEPHONY_INTERFACE "org.bluez.telephonyCtrl"

static DBusConnection *dbus_conn;
static GDBusProxy *default_call = NULL;
static GList *callList = NULL;
static GDBusClient *client = NULL;

static char *proxy_description(GDBusProxy *proxy, const char *title,
			       const char *description)
{
	const char *path;

	path = g_dbus_proxy_get_path(proxy);
	return g_strdup_printf("%s%s%s%s %s ",
					description ? "[" : "",
					description ? : "",
					description ? "] " : "",
					title, path);
}

static void print_info(void *data, void *user_data)
{
	GDBusProxy *proxy = data;
	const char *description = user_data;
	char *str;

	str = proxy_description(proxy, "Telephony", description);

	bt_shell_printf("%s%s\n", str,
			default_call == proxy ? "[default]" : "");

	g_free(str);
}

static void call_reject_reply(DBusMessage *message, void *user_data)
{
	DBusError error;

	dbus_error_init(&error);

	if (dbus_set_error_from_message(&error, message) == TRUE) {
		bt_shell_printf("Failed to reject call: %s\n", error.name);
		dbus_error_free(&error);
		return bt_shell_noninteractive_quit(EXIT_FAILURE);
	}

	bt_shell_printf("operation completed\n");

	return bt_shell_noninteractive_quit(EXIT_SUCCESS);
}

static void cmd_reject(int argc, char *argv[])
{
	if (!default_call) {
		bt_shell_printf("No active calls present\n");
		return bt_shell_noninteractive_quit(EXIT_FAILURE);
	}

	if (g_dbus_proxy_method_call(default_call, "reject", NULL,
				     call_reject_reply, NULL, NULL) == FALSE) {
		bt_shell_printf("Failed to reject call\n");
		return bt_shell_noninteractive_quit(EXIT_FAILURE);
	}

}

static void call_answer_reply(DBusMessage *message, void *user_data)
{
	DBusError error;

	dbus_error_init(&error);

	if (dbus_set_error_from_message(&error, message) == TRUE) {
		bt_shell_printf("Failed to answer call: %s\n", error.name);
		dbus_error_free(&error);
		return bt_shell_noninteractive_quit(EXIT_FAILURE);
	}

	bt_shell_printf("operation completed\n");

	return bt_shell_noninteractive_quit(EXIT_SUCCESS);
}

static void cmd_answer(int argc, char *argv[])
{
	if (!default_call)
		return bt_shell_noninteractive_quit(EXIT_FAILURE);

	if (g_dbus_proxy_method_call(default_call, "answer", NULL,
				     call_answer_reply, NULL, NULL) == FALSE) {
		bt_shell_printf("Failed to answer the call\n");
		return bt_shell_noninteractive_quit(EXIT_FAILURE);
	}
}

static const struct bt_shell_menu call_menu = {
	.name = "telephony",
	.desc = "telephony Settings Submenu",
	.entries = {
	{ "answer", NULL, cmd_answer, "answer the active call" },
	{ "reject", NULL, cmd_reject, "reject the active call" },
	{} },
};

static void call_added(GDBusProxy *proxy)
{
	bt_shell_printf("[CHG] Telephony caller Added\n");
	callList = g_list_append(callList, proxy);

	if (!default_call)
		default_call = proxy;

	print_info(proxy, COLORED_NEW);
}

static void call_removed(GDBusProxy *proxy)
{
	bt_shell_printf("[CHG] Telephony caller Removed\n");

	if (default_call == proxy)
		default_call = NULL;

	callList = g_list_remove(callList, proxy);
}

static void proxy_added(GDBusProxy *proxy, void *user_data)
{
	const char *interface;

	interface = g_dbus_proxy_get_interface(proxy);

	if (!strcmp(interface, BLUEZ_TELEPHONY_INTERFACE))
		call_added(proxy);
}

static void proxy_removed(GDBusProxy *proxy, void *user_data)
{
	const char *interface;

	interface = g_dbus_proxy_get_interface(proxy);

	if (!strcmp(interface, BLUEZ_TELEPHONY_INTERFACE))
		call_removed(proxy);
}

static void telephony_property_changed(GDBusProxy *proxy, const char *name,
				       DBusMessageIter *iter)
{
	char *str;

	str = proxy_description(proxy, "Telephony", COLORED_CHG);
	print_iter(str, name, iter);
	g_free(str);

	bt_shell_printf("[CHG] Telephony property : %s\n", name);
}

static void property_changed(GDBusProxy *proxy, const char *name,
			     DBusMessageIter *iter, void *user_data)
{
	const char *interface;

	interface = g_dbus_proxy_get_interface(proxy);

	if (!strcmp(interface, BLUEZ_TELEPHONY_INTERFACE))
		telephony_property_changed(proxy, name, iter);
}

void telephony_add_submenu(void)
{
	bt_shell_add_submenu(&call_menu);

	dbus_conn = bt_shell_get_env("DBUS_CONNECTION");
	if (!dbus_conn || client)
		return;

	client = g_dbus_client_new(dbus_conn, "org.bluez", "/org/bluez");

	g_dbus_client_set_proxy_handlers(client, proxy_added, proxy_removed,
					 property_changed, NULL);
	g_dbus_client_set_disconnect_watch(client, NULL, NULL);
}

void telephony_remove_submenu(void)
{
	g_dbus_client_unref(client);
}
