// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2023  NXP Semiconductors. All rights reserved.
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
#include "btio/btio.h"
#include "src/shared/util.h"
#include "src/shared/tester.h"
#include "src/shared/queue.h"
#include "src/shared/att.h"
#include "src/shared/gatt-db.h"
#include "src/shared/gatt-helpers.h"
#include "src/shared/micp.h"

struct test_data {
	struct gatt_db *db;
	struct bt_mics *mics;
	struct bt_micp *micp;
	struct bt_gatt_client *client;
	size_t iovcnt;
	struct iovec *iov;
	struct test_config *cfg;
};

struct db_attribute_micp_test_data {
	struct gatt_db_attribute *match;
	bool found;
};

#define MICP_GATT_CLIENT_MTU	64
#define iov_data(args...) ((const struct iovec[]) { args })

#define define_test(name, function, _cfg, args...)		\
	do {							\
		const struct iovec iov[] = { args };		\
		static struct test_data data;			\
		data.cfg = _cfg;				\
		data.iovcnt = ARRAY_SIZE(iov_data(args));	\
		data.iov = util_iov_dup(iov, ARRAY_SIZE(iov_data(args))); \
		tester_add(name, &data, test_setup, function,	\
				test_teardown);			\
	} while (0)

static void print_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	if (tester_use_debug())
		tester_debug("%s %s", prefix, str);
}

static void test_teardown(const void *user_data)
{
	struct test_data *data = (void *)user_data;

	bt_gatt_client_unref(data->client);
	util_iov_free(data->iov, data->iovcnt);
	gatt_db_unref(data->db);

	tester_teardown_complete();
}

static void test_complete_cb(const void *user_data)
{
	tester_test_passed();
}

static void client_ready_cb(bool success, uint8_t att_ecode, void *user_data)
{

	if (!success)
		tester_setup_failed();
	else
		tester_setup_complete();
}

static void micp_write_cb(bool success, uint8_t att_ecode, void *user_data)
{
	if (success)
		printf("MICP Write successful\n");
	else
		printf("\nWrite failed: 0x%02x\n", att_ecode);
}

static void micp_write_value(struct bt_micp *micp, void *user_data)
{
	struct bt_mics *mics = micp_get_mics(micp);
	uint16_t	value_handle;
	int ret;
	const uint16_t value = 0x0001;

	gatt_db_attribute_get_char_data(mics->ms, NULL, &value_handle,
							NULL, NULL, NULL);

	printf("%s handle: %x\n", __func__, value_handle);
	ret = bt_gatt_client_write_value(micp->client, value_handle,
		(void *)&value, sizeof(value), micp_write_cb, NULL, NULL);

	if (!ret)
		printf("bt_gatt_client_write_value() : Write FAILED");
}

static void micp_ready(struct bt_micp *micp, void *user_data)
{
	micp_write_value(micp, user_data);
}

static void test_client(const void *user_data)
{
	struct test_data *data = (void *)user_data;
	struct io *io;

	io = tester_setup_io(data->iov, data->iovcnt);
	g_assert(io);

	tester_io_set_complete_func(test_complete_cb);

	data->db = gatt_db_new();
	g_assert(data->db);

	data->micp = bt_micp_new(data->db, bt_gatt_client_get_db(data->client));
	g_assert(data->micp);

	bt_micp_set_debug(data->micp, print_debug, "bt_mip: ", NULL);

	bt_micp_ready_register(data->micp, micp_ready, data, NULL);

	bt_micp_attach(data->micp, data->client);
}

	/* ATT: Exchange MTU Response (0x03) len 2
	 *   Server RX MTU: 64
	 */
	/* ATT: Exchange MTU Request (0x02) len 2
	 *    Client RX MTU: 64
	 */
#define ATT_EXCHANGE_MTU	IOV_DATA(0x02, 0x40, 0x00), \
			IOV_DATA(0x03, 0x40, 0x00)

/*
 *      ATT: Read By Type Request (0x08) len 6
 *        Handle range: 0x0001-0xffff
 *        Attribute type: Server Supported Features (0x2b3a)
 */
#define MICP_READ_SR_FEATURE	IOV_DATA(0x08, 0x01, 0x00, 0Xff, 0xff, \
			0x3a, 0x2b), \
			IOV_DATA(0x01, 0x08, 0x01, 0x00, 0x0a)

	/*
	 * ATT: Read By Group Type Request (0x10) len 6
	 *   Handle range: 0x0001-0xffff
	 *   Attribute group type: Primary Service (0x2800)
	 */

/*
 *     ATT: Read By Group Type Response (0x11) len 7
 *        Attribute data length: 6
 *        Attribute group list: 1 entry
 *        Handle range: 0x00a0-0x00a4
 *        UUID: Microphone Control (0x184d)
 */
#define MICP_READ_GROUP_TYPE	\
			IOV_DATA(0x10, 0x01, 0x00, 0xff, 0xff, 0x00, 0x28), \
			IOV_DATA(0x11, 0x06, \
				0x01, 0x00, 0x04, 0x00, 0x4d, 0x18), \
			IOV_DATA(0x10, 0x05, 0x00, 0xff, 0xff, 0x00, 0x28), \
			IOV_DATA(0x01, 0x10, 0x06, 0x00, 0x0a)

	/* ATT: Read By Group Type Request (0x10) len 6
	 *   Handle range: 0x0001-0xffff
	 *   Attribute group type: Secondary Service (0x2801)
	 */
	/* ATT: Error Response (0x01) len 4
	 *   Read By Group Type Request (0x10)
	 *   Handle: 0x0001
	 *   Error: Attribute Not Found (0x0a)08 01 00 05 00 02 28
	 */
#define MICP_READ_REQ_SECOND_SERVICE	\
			IOV_DATA(0x10, 0x01, 0x00, 0xff, 0xff, 0x01, 0x28), \
			IOV_DATA(0x01, 0x10, 0x01, 0x00, 0x0a)

#define MICP_READ_REQ_INCLUDE_SERVICE	\
			IOV_DATA(0x08, 0x01, 0x00, 0x04, 0x00, 0x02, 0x28), \
			IOV_DATA(0x01, 0x08, 0x01, 0x00, 0x0a)

	/* ATT: Read By Type Request (0x08) len 6
	 *   Handle range: 0x0001-0x0004
	 *   Attribute type: Characteristic (0x2803)
	 */

/*      ATT: Find Information Request (0x04) len 4
 *        Handle range: 0x0004-0x0004
 */
#define	MICP_FIND_INFO_REQ	\
			IOV_DATA(0x04, 0x04, 0x00, 0x04, 0x00), \
			IOV_DATA(0x05, 0x01, 0x04, 0x00, 0x02, 0x29)

/*
 *      ATT: Read By Type Request (0x08) len 6
 *        Handle range: 0x0001-0x0004
 *        Attribute type: Characteristic (0x2803)
 */
#define	MICP_READ_REQ_CHAR	\
			IOV_DATA(0x08, 0x01, 0x00, 0x04, 0x00, 0x03, 0x28),\
			IOV_DATA(0x09, 0x07, \
			0x02, 0x00, 0x1a, 0x03, 0x00, 0xc3, 0x2b), \
			IOV_DATA(0x08, 0x03, 0x00, 0x04, 0x00, 0x03, 0x28), \
			IOV_DATA(0x01, 0x08, 0x04, 0x00, 0x0a)

#define	MICS_MUTE_READ \
			IOV_DATA(0x0a, 0x03, 0x00), \
			IOV_DATA(0x0b, 0x01)

#define	MICS_EN_MUTE_DISCPTR	\
			IOV_DATA(0x12, 0x04, 0x00, 0x01, 0x00), \
			IOV_DATA(0x13)

#define	MICS_MUTE_WRITE	\
			IOV_DATA(0x12, 0x03, 0x00, 0x01),\
			IOV_DATA(0x13)

#define MICP_CL_CGGIT_SER_BV_01_C \
			MICS_MUTE_READ, \
			MICS_EN_MUTE_DISCPTR, \
			IOV_DATA(0x12, 0x03, 0x00, 0x01, 0x00), \
			IOV_DATA(0x01, 0x12, 0x03, 0x00, 0x013)

#define	MICP_CL_CGGIT_CHA_BV_01_C	\
			MICS_MUTE_READ, \
			MICS_EN_MUTE_DISCPTR, \
			IOV_DATA(0x12, 0x03, 0x00, 0x06, 0x00), \
			IOV_DATA(0x01, 0x12, 0x03, 0x00, 0x013), \
			MICS_MUTE_READ

#define MICP_CL_SPE_BI_01_C	\
			MICS_MUTE_READ, \
			MICS_EN_MUTE_DISCPTR, \
			IOV_DATA(0x12, 0x03, 0x00, 0x01, 0x00), \
			IOV_DATA(0x01, 0x12, 0x03, 0x00, 0x80)

/* GATT Discover All procedure */
static const struct iovec setup_data[] = {
				ATT_EXCHANGE_MTU,
				MICP_READ_SR_FEATURE,
				MICP_READ_GROUP_TYPE,
				MICP_READ_REQ_SECOND_SERVICE,
				MICP_READ_REQ_INCLUDE_SERVICE,
				MICP_READ_REQ_CHAR,
				MICP_FIND_INFO_REQ
};

static void test_setup(const void *user_data)
{
	struct test_data *data = (void *)user_data;
	struct bt_att *att;
	struct gatt_db *db;
	struct io *io;

	io = tester_setup_io(setup_data, ARRAY_SIZE(setup_data));
	g_assert(io);

	att = bt_att_new(io_get_fd(io), false);
	g_assert(att);

	bt_att_set_debug(att, BT_ATT_DEBUG, print_debug, "bt_att:", NULL);

	db = gatt_db_new();
	g_assert(db);


	data->client = bt_gatt_client_new(db, att, MICP_GATT_CLIENT_MTU, 0);
	g_assert(data->client);

	bt_gatt_client_set_debug(data->client, print_debug, "bt_gatt_client:",
						NULL);

	bt_gatt_client_ready_register(data->client, client_ready_cb, data,
						NULL);

	bt_att_unref(att);
	gatt_db_unref(db);
}


int main(int argc, char *argv[])
{

	tester_init(&argc, &argv);

	define_test("MICP/CL/CGGIT/SER/BV-01-C", test_client, NULL,
					MICS_MUTE_READ);
	define_test("MICP/CL/CGGIT/CHA/BV-01-C", test_client, NULL,
					MICP_CL_CGGIT_SER_BV_01_C);
	define_test("MICP/CL/SPE/BI-01-C", test_client, NULL,
					MICP_CL_SPE_BI_01_C);

	return tester_run();
}
