// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 *  OBEX Server
 *
 *  Copyright (C) 2007-2010  Marcel Holtmann <marcel@holtmann.org>
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

#include "obexd.h"
#include "plugin.h"
#include "log.h"

/*
 * Plugins that are using libraries with threads and their own mainloop
 * will crash on exit. This is a bug inside these libraries, but there is
 * nothing much that can be done about it. One bad example is libebook.
 */
#ifdef NEED_THREADS
#define PLUGINFLAG (RTLD_NOW | RTLD_NODELETE)
#else
#define PLUGINFLAG (RTLD_NOW)
#endif

static GSList *plugins = NULL;

struct obex_plugin {
	struct obex_plugin_desc *desc;
};

static void add_plugin(struct obex_plugin_desc *desc)
{
	struct obex_plugin *plugin;

	if (desc->init == NULL)
		return;

	plugin = g_try_new0(struct obex_plugin, 1);
	if (plugin == NULL)
		return;

	plugin->desc = desc;

	if (desc->init() < 0) {
		g_free(plugin);
		return;
	}

	plugins = g_slist_append(plugins, plugin);
	DBG("Plugin %s loaded", desc->name);
}

static gboolean check_plugin(struct obex_plugin_desc *desc,
				char **patterns, char **excludes)
{
	if (excludes) {
		for (; *excludes; excludes++)
			if (g_pattern_match_simple(*excludes, desc->name))
				break;
		if (*excludes) {
			info("Excluding %s", desc->name);
			return FALSE;
		}
	}

	if (patterns) {
		for (; *patterns; patterns++)
			if (g_pattern_match_simple(*patterns, desc->name))
				break;
		if (*patterns == NULL) {
			info("Ignoring %s", desc->name);
			return FALSE;
		}
	}

	return TRUE;
}


#include "builtin.h"

void plugin_init(const char *pattern, const char *exclude)
{
	char **patterns = NULL;
	char **excludes = NULL;
	unsigned int i;

	if (pattern)
		patterns = g_strsplit_set(pattern, ":, ", -1);

	if (exclude)
		excludes = g_strsplit_set(exclude, ":, ", -1);

	DBG("Loading builtin plugins");

	for (i = 0; __obex_builtin[i]; i++) {
		if (check_plugin(__obex_builtin[i],
					patterns, excludes) == FALSE)
			continue;

		add_plugin(__obex_builtin[i]);
	}

	g_strfreev(patterns);
	g_strfreev(excludes);
}

void plugin_cleanup(void)
{
	GSList *list;

	DBG("Cleanup plugins");

	for (list = plugins; list; list = list->next) {
		struct obex_plugin *plugin = list->data;

		if (plugin->desc->exit)
			plugin->desc->exit();

		g_free(plugin);
	}

	g_slist_free(plugins);
}
