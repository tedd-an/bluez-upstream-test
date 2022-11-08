// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2022  Intel Corporation.
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <fcntl.h>

#include <glib.h>

#include "lib/bluetooth.h"
#include "lib/uuid.h"
#include "src/shared/util.h"
#include "src/shared/io.h"
#include "src/shared/tester.h"
#include "src/shared/queue.h"
#include "src/shared/att.h"
#include "src/shared/gatt-db.h"
#include "src/shared/gatt-client.h"
#include "src/shared/bap.h"

static void client_ready_cb(bool success, uint8_t att_ecode, void *user_data)
{
	struct bt_gatt_client *client = user_data;

	if (success)
		tester_setup_complete();
	else
		tester_setup_failed();

	bt_gatt_client_unref(client);
}

static const struct iovec setup_data[] = {
	/* ATT: Exchange MTU Response (0x03) len 2
	 *   Server RX MTU: 64
	 */
	IOV_DATA(0x02, 0x40, 0x00),
	/* ATT: Exchange MTU Request (0x02) len 2
	 *    Client RX MTU: 64
	 */
	IOV_DATA(0x03, 0x40, 0x00),
	/* ATT: Read By Type Request (0x08) len 6
	 *   Handle range: 0x0001-0xffff
	 *   Attribute type: Server Supported Features (0x2b3a)
	 */
	IOV_DATA(0x08, 0x01, 0x00, 0xff, 0xff, 0x3a, 0x2b),
	/* ATT: Error Response (0x01) len 4
	 *   Read By Type Request (0x08)
	 *   Handle: 0x0001
	 *   Error: Attribute Not Found (0x0a)
	 */
	IOV_DATA(0x01, 0x08, 0x01, 0x00, 0x0a),
	/*
	 * ATT: Read By Group Type Request (0x10) len 6
	 *   Handle range: 0x0001-0xffff
	 *   Attribute group type: Primary Service (0x2800)
	 */
	IOV_DATA(0x10, 0x01, 0x00, 0xff, 0xff, 0x00, 0x28),
	/*
	 * ATT: Read By Group Type Response (0x11) len 37
	 *   Attribute data length: 6
	 *   Attribute group list: 2 entries
	 *   Handle range: 0x0001-0x0013
	 *   UUID: Published Audio Capabilities (0x1850)
	 *   Handle range: 0x0014-0x0023
	 *   UUID: Audio Stream Control (0x184e)
	 */
	IOV_DATA(0x11, 0x06,
		0x01, 0x00, 0x13, 0x00, 0x50, 0x18,
		0x14, 0x00, 0x23, 0x00, 0x4e, 0x18),
	/* ATT: Read By Group Type Request (0x10) len 6
	 *   Handle range: 0x0024-0xffff
	 *   Attribute group type: Primary Service (0x2800)
	 */
	IOV_DATA(0x10, 0x24, 0x00, 0xff, 0xff, 0x00, 0x28),
	/* ATT: Error Response (0x01) len 4
	 *   Read By Group Type Request (0x10)
	 *   Handle: 0x0024
	 *   Error: Attribute Not Found (0x0a)
	 */
	IOV_DATA(0x01, 0x10, 0x24, 0x00, 0x0a),
	/* ATT: Read By Group Type Request (0x10) len 6
	 *   Handle range: 0x0001-0xffff
	 *   Attribute group type: Secondary Service (0x2801)
	 */
	IOV_DATA(0x10, 0x01, 0x00, 0xff, 0xff, 0x01, 0x28),
	/* ATT: Error Response (0x01) len 4
	 *   Read By Group Type Request (0x10)
	 *   Handle: 0x0001
	 *   Error: Attribute Not Found (0x0a)
	 */
	IOV_DATA(0x01, 0x10, 0x01, 0x00, 0x0a),
	/* ATT: Read By Type Request (0x08) len 6
	 *   Handle range: 0x0001-0x0023
	 *   Attribute group type: Include (0x2802)
	 */
	IOV_DATA(0x08, 0x01, 0x00, 0x23, 0x00, 0x02, 0x28),
	/* ATT: Error Response (0x01) len 4
	 *   Read By Group Type Request (0x10)
	 *   Handle: 0x0001
	 *   Error: Attribute Not Found (0x0a)
	 */
	IOV_DATA(0x01, 0x08, 0x01, 0x00, 0x0a),
	/* ATT: Read By Type Request (0x08) len 6
	 *   Handle range: 0x0001-0x0023
	 *   Attribute type: Characteristic (0x2803)
	 */
	IOV_DATA(0x08, 0x01, 0x00, 0x23, 0x00, 0x03, 0x28),
	/* ACL Data RX: Handle 42 flags 0x02 dlen 80
	 *   ATT: Read By Type Response (0x09) len 75
	 *   Attribute data length: 7
	 *   Attribute data list: 10 entries
	 *   Handle: 0x0002
	 *   Value: 120300c92b
	 *   Properties: 0x12
	 *     Read (0x02)
	 *     Notify (0x10)
	 *   Value Handle: 0x0003
	 *   Value UUID: Sink PAC (0x2bc9)
	 *   Handle: 0x0005
	 *   Value: 120600ca2b
	 *   Properties: 0x12
	 *     Read (0x02)
	 *     Notify (0x10)
	 *   Value Handle: 0x0006
	 *   Value UUID: Sink Audio Locations (0x2bca)
	 *   Handle: 0x0008
	 *   Value: 120900cb2b
	 *   Properties: 0x12
	 *     Read (0x02)
	 *     Notify (0x10)
	 *   Value Handle: 0x0009
	 *   Value UUID: Source PAC (0x2bcb)
	 *   Handle: 0x000b
	 *   Value: 120c00cc2b
	 *   Properties: 0x12
	 *     Read (0x02)
	 *     Notify (0x10)
	 *  Value Handle: 0x000c
	 *  Value UUID: Source Audio Locations (0x2bcc)
	 *  Handle: 0x000e
	 *  Value: 120f00cd2b
	 *  Properties: 0x12
	 *    Read (0x02)
	 *    Notify (0x10)
	 *  Value Handle: 0x000f
	 *  Value UUID: Available Audio Contexts (0x2bcd)
	 *  Handle: 0x0011
	 *  Value: 121200ce2b
	 *  Properties: 0x12
	 *    Read (0x02)
	 *    Notify (0x10)
	 *  Value Handle: 0x0012
	 *  Value UUID: Supported Audio Contexts (0x2bce)
	 *  Handle: 0x0015
	 *  Value: 121600c42b
	 *  Properties: 0x12
	 *    Read (0x02)
	 *    Notify (0x10)
	 *  Value Handle: 0x0016
	 *  Value UUID: Sink ASE (0x2bc4)
	 *  Handle: 0x0018
	 *  Value: 121900c42b
	 *  Properties: 0x12
	 *    Read (0x02)
	 *    Notify (0x10)
	 *  Value Handle: 0x0019
	 *  Value UUID: Sink ASE (0x2bc4)
	 */
	IOV_DATA(0x09, 0x07,
		0x02, 0x00, 0x12, 0x03, 0x00, 0xc9, 0x2b,
		0x05, 0x00, 0x12, 0x06, 0x00, 0xca, 0x2b,
		0x08, 0x00, 0x12, 0x09, 0x00, 0xcb, 0x2b,
		0x0b, 0x00, 0x12, 0x0c, 0x00, 0xcc, 0x2b,
		0x0e, 0x00, 0x12, 0x0f, 0x00, 0xcd, 0x2b,
		0x11, 0x00, 0x12, 0x12, 0x00, 0xce, 0x2b,
		0x15, 0x00, 0x12, 0x16, 0x00, 0xc4, 0x2b,
		0x18, 0x00, 0x12, 0x19, 0x00, 0xc4, 0x2b),
	/* ATT: Read By Type Request (0x08) len 6
	 *   Handle range: 0x0001-0x0023
	 *   Attribute type: Characteristic (0x2803)
	 */
	IOV_DATA(0x08, 0x19, 0x00, 0x23, 0x00, 0x03, 0x28),
	/* ATT: Read By Type Response (0x09) len 75
	 *   Attribute data length: 7
	 *   Attribute data list: 3 entries
	 *   Handle: 0x001b
	 *   Value: 121c00c52b
	 *   Properties: 0x12
	 *     Read (0x02)
	 *     Notify (0x10)
	 *   Value Handle: 0x001c
	 *   Value UUID: Source ASE (0x2bc5)
	 *   Handle: 0x001e
	 *   Value: 121f00c52b
	 *   Properties: 0x12
	 *     Read (0x02)
	 *     Notify (0x10)
	 *   Value Handle: 0x001f
	 *   Value UUID: Source ASE (0x2bc5)
	 *   Handle: 0x0021
	 *   Value: 182200c62b
	 *   Properties: 0x18
	 *     Write (0x08)
	 *     Notify (0x10)
	 *   Value Handle: 0x0022
	 *   Value UUID: ASE Control Point (0x2bc6)
	 */
	IOV_DATA(0x09, 0x07,
		0x1b, 0x00, 0x12, 0x1c, 0x00, 0xc5, 0x2b,
		0x1e, 0x00, 0x12, 0x1f, 0x00, 0xc5, 0x2b,
		0x21, 0x00, 0x18, 0x22, 0x00, 0xc6, 0x2b),
	/* ATT: Read By Type Request (0x08) len 6
	 *   Handle range: 0x0022-0x0023
	 *   Attribute type: Characteristic (0x2803)
	 */
	IOV_DATA(0x08, 0x22, 0x00, 0x23, 0x00, 0x03, 0x28),
	/* ATT: Error Response (0x01) len 4
	 *   Read By Type Request (0x08)
	 *   Handle: 0x0022
	 *   Error: Attribute Not Found (0x0a)
	 */
	IOV_DATA(0x01, 0x08, 0x23, 0x00, 0x0a),
	/* ACL Data TX: Handle 42 flags 0x00 dlen 11
	 *   ATT: Read By Type Request (0x08) len 6
	 *   Handle range: 0x0001-0xffff
	 *   Attribute type: Database Hash (0x2b2a)
	 */
	IOV_DATA(0x08, 0x01, 0x00, 0xff, 0xff, 0x2a, 0x2b),
	/* ATT: Error Response (0x01) len 4
	 *   Read By Type Request (0x08)
	 *   Handle: 0x0001
	 *   Error: Attribute Not Found (0x0a)
	 */
	IOV_DATA(0x01, 0x08, 0x01, 0x00, 0x0a),
};

static void print_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	tester_debug("%s%s", prefix, str);
}

static void test_setup(const void *data)
{
	struct bt_att *att;
	struct gatt_db *db;
	struct bt_gatt_client *client;
	struct io *io;

	io = tester_setup_io(setup_data, ARRAY_SIZE(setup_data));
	g_assert(io);

	att = bt_att_new(io_get_fd(io), false);
	g_assert(att);

	bt_att_set_debug(att, BT_ATT_DEBUG, print_debug, "bt_att:", NULL);

	db = gatt_db_new();
	g_assert(db);

	client = bt_gatt_client_new(db, att, 64, 0);
	g_assert(client);

	bt_gatt_client_set_debug(client, print_debug, "bt_gatt_client:", NULL);

	bt_gatt_client_ready_register(client, client_ready_cb, client, NULL);

	bt_att_unref(att);
	gatt_db_unref(db);
}

static void test_client(const void *data)
{
	tester_test_passed();
}

int main(int argc, char *argv[])
{
	tester_init(&argc, &argv);

	tester_add("/bap/basic", NULL, test_setup, test_client, NULL);

	return tester_run();
}
