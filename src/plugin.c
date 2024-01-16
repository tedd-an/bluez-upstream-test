// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2004-2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <dlfcn.h>
#include <string.h>
#include <sys/stat.h>

#include <glib.h>

#include "lib/bluetooth.h"

#include "btio/btio.h"
#include "src/plugin.h"
#include "src/log.h"
#include "src/btd.h"

static GSList *plugins = NULL;

struct bluetooth_plugin {
	gboolean active;
	struct bluetooth_plugin_desc *desc;
};

static int compare_priority(gconstpointer a, gconstpointer b)
{
	const struct bluetooth_plugin *plugin1 = a;
	const struct bluetooth_plugin *plugin2 = b;

	return plugin2->desc->priority - plugin1->desc->priority;
}

static void add_plugin(struct bluetooth_plugin_desc *desc)
{
	struct bluetooth_plugin *plugin;

	if (desc->init == NULL)
		return;

	if (g_str_equal(desc->version, VERSION) == FALSE) {
		error("Version mismatch for %s", desc->name);
		return;
	}

	DBG("Loading %s plugin", desc->name);

	plugin = g_try_new0(struct bluetooth_plugin, 1);
	if (plugin == NULL)
		return;

	plugin->active = FALSE;
	plugin->desc = desc;

	__btd_enable_debug(desc->debug_start, desc->debug_stop);

	plugins = g_slist_insert_sorted(plugins, plugin, compare_priority);
}

static gboolean enable_plugin(const char *name, char **cli_enable,
							char **cli_disable)
{
	if (cli_disable) {
		for (; *cli_disable; cli_disable++)
			if (g_pattern_match_simple(*cli_disable, name))
				break;
		if (*cli_disable) {
			info("Excluding (cli) %s", name);
			return FALSE;
		}
	}

	if (cli_enable) {
		for (; *cli_enable; cli_enable++)
			if (g_pattern_match_simple(*cli_enable, name))
				break;
		if (!*cli_enable) {
			info("Ignoring (cli) %s", name);
			return FALSE;
		}
	}

	return TRUE;
}

#include "src/builtin.h"

void plugin_init(const char *enable, const char *disable)
{
	GSList *list;
	char **cli_disabled, **cli_enabled;
	unsigned int i;

	/* Make a call to BtIO API so its symbols got resolved before the
	 * plugins are loaded. */
	bt_io_error_quark();

	if (enable)
		cli_enabled = g_strsplit_set(enable, ", ", -1);
	else
		cli_enabled = NULL;

	if (disable)
		cli_disabled = g_strsplit_set(disable, ", ", -1);
	else
		cli_disabled = NULL;

	DBG("Loading builtin plugins");

	for (i = 0; __bluetooth_builtin[i]; i++) {
		if (!enable_plugin(__bluetooth_builtin[i]->name, cli_enabled,
								cli_disabled))
			continue;

		add_plugin(__bluetooth_builtin[i]);
	}

	for (list = plugins; list; list = list->next) {
		struct bluetooth_plugin *plugin = list->data;
		int err;

		err = plugin->desc->init();
		if (err < 0) {
			if (err == -ENOSYS || err == -ENOTSUP)
				warn("System does not support %s plugin",
							plugin->desc->name);
			else
				error("Failed to init %s plugin",
							plugin->desc->name);
			continue;
		}

		plugin->active = TRUE;
	}

	g_strfreev(cli_enabled);
	g_strfreev(cli_disabled);
}

void plugin_cleanup(void)
{
	GSList *list;

	DBG("Cleanup plugins");

	for (list = plugins; list; list = list->next) {
		struct bluetooth_plugin *plugin = list->data;

		if (plugin->active == TRUE && plugin->desc->exit)
			plugin->desc->exit();

		g_free(plugin);
	}

	g_slist_free(plugins);
}
