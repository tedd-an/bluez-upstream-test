/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2012-2014  Intel Corporation. All rights reserved.
 *
 *
 */

#include <glib.h>

guint io_glib_add_err_watch(GIOChannel *io, GIOCondition events,
				GIOFunc func, gpointer user_data);
guint io_glib_add_err_watch_full(GIOChannel *io, gint priority,
				GIOCondition events, GIOFunc func,
				gpointer user_data,
				GDestroyNotify notify);

bool io_set_use_err_watch(struct io *io, bool err_watch);
