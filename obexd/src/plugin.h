/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *
 *  OBEX Server
 *
 *  Copyright (C) 2007-2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 */

struct obex_plugin_desc {
	const char *name;
	int (*init) (void);
	void (*exit) (void);
};

#define OBEX_PLUGIN_DEFINE(name, init, exit) \
		struct obex_plugin_desc __obex_builtin_ ## name = { \
			#name, init, exit \
		};
