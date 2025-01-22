/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2025 Bastien Nocera <hadess@hadess.net>
 *
 *
 */

#include <stddef.h>
#include <glib.h>
#include "error.h"

struct {
	const char *error_code;
	const char *str;
} error_codes[] = {
	{ "br-connection-profile-unavailable", "Exhausted the list of BR/EDR profiles to connect to" },
	{ "br-connection-busy", "Cannot connect, connection busy" },
	{ "br-connection-adapter-not-powered", "Cannot connect, adapter is not powered" },
};

const char *error_code_to_str(const char *error_code)
{
	unsigned int i;

	if (error_code == NULL)
		return NULL;

	for (i = 0; i < G_N_ELEMENTS(error_codes); i++) {
		if (g_str_equal(error_codes[i].error_code, error_code))
			return error_codes[i].str;
	}
	return error_code;
}
