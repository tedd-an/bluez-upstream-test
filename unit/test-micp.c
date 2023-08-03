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
#include "src/shared/util.h"
#include "src/shared/tester.h"
#include "src/shared/queue.h"
#include "src/shared/att.h"
#include "src/shared/gatt-db.h"
#include "src/shared/gatt-server.h"
#include "src/shared/micp.h"

struct test_data {
	struct gatt_db *db;
	struct bt_micp *micp;
	struct bt_gatt_server *server;
	struct bt_gatt_client *client;
	struct queue *ccc_states;
	size_t iovcnt;
	struct iovec *iov;
	struct test_config *cfg;
};

struct ccc_state {
	uint16_t handle;
	uint16_t value;
};

struct notify {
	uint16_t handle, ccc_handle;
	uint8_t *value;
	uint16_t len;
	bt_gatt_server_conf_func_t conf;
	void *user_data;
};

#define iov_data(args...) ((const struct iovec[]) { args })

#define define_test(name, function, _cfg, args...)		\
	do {							\
		const struct iovec iov[] = { args };		\
		static struct test_data data;			\
		data.cfg = _cfg;				\
		data.iovcnt = ARRAY_SIZE(iov_data(args));	\
		data.iov = util_iov_dup(iov, ARRAY_SIZE(iov_data(args))); \
		tester_add(name, &data, NULL, function,	\
				test_teardown);			\
	} while (0)

static void print_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	if (tester_use_debug())
		tester_debug("%s%s", prefix, str);
}

static void test_teardown(const void *user_data)
{
	struct test_data *data = (void *)user_data;

	bt_micp_unref(data->micp);
	bt_gatt_server_unref(data->server);
	util_iov_free(data->iov, data->iovcnt);
	gatt_db_unref(data->db);

	queue_destroy(data->ccc_states, free);

	tester_teardown_complete();
}

static void test_complete_cb(const void *user_data)
{
	tester_test_passed();
}

static bool ccc_state_match(const void *a, const void *b)
{
	const struct ccc_state *ccc = a;
	uint16_t handle = PTR_TO_UINT(b);

	return ccc->handle == handle;
}

static struct ccc_state *find_ccc_state(struct test_data *data,
				uint16_t handle)
{
	return queue_find(data->ccc_states, ccc_state_match,
				UINT_TO_PTR(handle));
}

static struct ccc_state *get_ccc_state(struct test_data *data, uint16_t handle)
{
	struct ccc_state *ccc;

	ccc = find_ccc_state(data, handle);
	if (ccc)
		return ccc;

	ccc = new0(struct ccc_state, 1);
	ccc->handle = handle;
	queue_push_tail(data->ccc_states, ccc);

	return ccc;
}

static void gatt_notify_cb(struct gatt_db_attribute *attrib,
					struct gatt_db_attribute *ccc,
					const uint8_t *value, size_t len,
					struct bt_att *att, void *user_data)
{
	struct test_data *data = user_data;
	struct notify notify;

	memset(&notify, 0, sizeof(notify));

	notify.handle = gatt_db_attribute_get_handle(attrib);
	notify.ccc_handle = gatt_db_attribute_get_handle(ccc);
	notify.value = (void *) value;
	notify.len = len;

	printf("%s: notify.value:%d notify->len:%d\n", __func__,
		(int)*(notify.value), notify.len);
	if (!bt_gatt_server_send_notification(data->server,
			notify.handle, notify.value,
			notify.len, false))
		printf("%s: Failed to send notification\n", __func__);
}

static void gatt_ccc_read_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	struct test_data *data = user_data;
	struct ccc_state *ccc;
	uint16_t handle;
	uint8_t ecode = 0;
	const uint8_t *value = NULL;
	size_t len = 0;

	handle = gatt_db_attribute_get_handle(attrib);

	ccc = get_ccc_state(data, handle);
	if (!ccc) {
		ecode = BT_ATT_ERROR_UNLIKELY;
		goto done;
	}

	len = sizeof(ccc->value);
	value = (void *) &ccc->value;

done:
	gatt_db_attribute_read_result(attrib, id, ecode, value, len);
}

static void test_server(const void *user_data)
{
	struct test_data *data = (void *)user_data;
	struct bt_att *att;
	struct io *io;

	io = tester_setup_io(data->iov, data->iovcnt);
	g_assert(io);

	tester_io_set_complete_func(test_complete_cb);

	att = bt_att_new(io_get_fd(io), false);
	g_assert(att);

	bt_att_set_debug(att, BT_ATT_DEBUG, print_debug, "bt_att:", NULL);

	data->db = gatt_db_new();
	g_assert(data->db);

	gatt_db_ccc_register(data->db, gatt_ccc_read_cb, NULL,
					gatt_notify_cb, data);

	data->micp = bt_micp_new(data->db, NULL);
	g_assert(data->micp);

	data->server = bt_gatt_server_new(data->db, att, 64, 0);
	g_assert(data->server);

	bt_gatt_server_set_debug(data->server, print_debug, "bt_gatt_server:",
					NULL);

	data->ccc_states = queue_new();

	tester_io_send();

	bt_att_unref(att);
}

#define EXCHANGE_MTU	IOV_DATA(0x02, 0x40, 0x00), \
						IOV_DATA(0x03, 0x40, 0x00)

#define	MICS_MUTE_WRITE_VAL_00 \
			IOV_DATA(0x12, 0x03, 0x00, 0x00), \
			IOV_DATA(0x13)

#define	MICS_MUTE_WRITE_VAL_01 \
			IOV_DATA(0x12, 0x03, 0x00, 0x01), \
			IOV_DATA(0x13)

#define	MICS_MUTE_READ \
			IOV_DATA(0x0a, 0x03, 0x00), \
			IOV_DATA(0x0b, 0x01)

#define DISCOVER_PRIM_SERV_NOTIF \
		IOV_DATA(0x10, 0x01, 0x00, 0xff, 0xff, 0x00, 0x28), \
		IOV_DATA(0x11, 0x06, 0x01, 0x00, 0x04, 0x00, 0x4d, 0x18), \
		IOV_DATA(0x10, 0x05, 0x00, 0xff, 0xff, 0x00, 0x28), \
		IOV_DATA(0x01, 0x10, 0x05, 0x00, 0x0a)

/* ATT: Read By Type Request (0x08) len 6
 *   Handle range: 0x0001-0x0009
 *   Attribute type: Characteristic (0x2803)
 * ATT: Read By Type Response (0x09) len 22
 * Attribute data length: 7
 *   Handle: 0x0002
 *   Value: 1a0300c82b
 *   Properties: 0x1a
 *   Value Handle: 0x0003
 *   Value UUID: Mute (0x2bc3)
 */
#define DISC_MICS_CHAR_1 \
	IOV_DATA(0x08, 0x01, 0x00, 0x05, 0x00, 0x03, 0x28), \
	IOV_DATA(0x09, 0x07, \
		0x02, 0x00, 0x1a, 0x03, 0x00, 0xc3, 0x2b), \
	IOV_DATA(0x08, 0x05, 0x00, 0x05, 0x00, 0x03, 0x28), \
	IOV_DATA(0x01, 0x08, 0x05, 0x00, 0x0a)


#define MICS_FIND_BY_TYPE_VALUE \
	IOV_DATA(0x06, 0x01, 0x00, 0xff, 0xff, 0x00, 0x28, 0x4d, 0x18), \
	IOV_DATA(0x07, 0x01, 0x00, 0x04, 0x00), \
	IOV_DATA(0x06, 0x05, 0x00, 0xff, 0xff, 0x00, 0x28, 0x4d, 0x18), \
	IOV_DATA(0x01, 0x06, 0x05, 0x00, 0x0a)

#define DISC_MICS_CHAR_AFTER_TYPE \
	IOV_DATA(0x08, 0x01, 0x00, 0x05, 0x00, 0x03, 0x28), \
	IOV_DATA(0x09, 0x07, \
		0x02, 0x00, 0x1a, 0x03, 0x00, 0xc3, 0x2b), \
	IOV_DATA(0x08, 0x03, 0x00, 0x05, 0x00, 0x03, 0x28), \
	IOV_DATA(0x01, 0x08, 0x03, 0x00, 0x0a)

#define MICS_WRITE_CCD \
	IOV_DATA(0x12, 0x04, 0x00, 0x00, 0x00), \
	IOV_DATA(0x13), \
	IOV_DATA(0x12, 0x04, 0x00, 0x01, 0x00), \
	IOV_DATA(0x13)

#define MICS_FIND_INFO \
	IOV_DATA(0x04, 0x04, 0x00, 0x05, 0x00), \
	IOV_DATA(0x05, 0x01, 0x04, 0x00, 0x02, 0x29), \
	IOV_DATA(0x04, 0x05, 0x00, 0x05, 0x00), \
	IOV_DATA(0x01, 0x04, 0x05, 0x00, 0x0a)

#define MICS_SR_SPN_BV_01_C \
			EXCHANGE_MTU, \
			DISCOVER_PRIM_SERV_NOTIF, \
			DISC_MICS_CHAR_1, \
			MICS_FIND_BY_TYPE_VALUE, \
			DISC_MICS_CHAR_AFTER_TYPE, \
			MICS_FIND_INFO, \
			MICS_WRITE_CCD, \
			IOV_DATA(0x0a, 0x03, 0x00), \
			IOV_DATA(0x0b, 0x01), \
			MICS_MUTE_WRITE_VAL_00, \
			IOV_DATA(0x1b, 0x03, 0x00, 0x00), \
			MICS_MUTE_WRITE_VAL_01, \
			IOV_DATA(0x1b, 0x03, 0x00, 0x01), \
			IOV_DATA(0x0a, 0x03, 0x00), \
			IOV_DATA(0x0b, 0x01)

#define MICS_SR_SGGIT_SER_BV_01_C \
						EXCHANGE_MTU, \
						DISCOVER_PRIM_SERV_NOTIF, \
						MICS_FIND_BY_TYPE_VALUE

#define MICS_SR_SGGIT_CHA_BV_01_C \
						EXCHANGE_MTU, \
						DISCOVER_PRIM_SERV_NOTIF, \
						MICS_FIND_BY_TYPE_VALUE, \
						DISC_MICS_CHAR_AFTER_TYPE

#define MICS_WRITE_MUTE_CHAR_INVALID \
			IOV_DATA(0x12, 0x03, 0x00, 0x02), \
			IOV_DATA(0x01, 0x12, 0x03, 0x00, 0x13), \
			IOV_DATA(0x12, 0x03, 0x00, 0x05), \
			IOV_DATA(0x01, 0x12, 0x03, 0x00, 0x13)

#define MICS_SR_SPE_BI_1_C	\
						EXCHANGE_MTU, \
						DISCOVER_PRIM_SERV_NOTIF, \
						MICS_FIND_BY_TYPE_VALUE, \
						MICS_WRITE_MUTE_CHAR_INVALID

#define	MICS_MUTE_READ_INVALID \
			IOV_DATA(0x0a, 0x03, 0x00), \
			IOV_DATA(0x0b, 0x02)

#define	MICS_MUTE_WRITE_1 \
			IOV_DATA(0x12, 0x03, 0x00, 0x01), \
			IOV_DATA(0x01, 0x12, 0x03, 0x00, 0x80)

#define	MICS_MUTE_WRITE_0 \
			IOV_DATA(0x12, 0x03, 0x00, 0x00), \
			IOV_DATA(0x01, 0x12, 0x03, 0x00, 0x80)

#define MICS_SR_SPE_BI_02_C	\
						EXCHANGE_MTU, \
						DISCOVER_PRIM_SERV_NOTIF, \
						MICS_FIND_BY_TYPE_VALUE, \
						MICS_MUTE_READ_INVALID, \
						MICS_MUTE_WRITE_0, \
						MICS_MUTE_WRITE_1

int main(int argc, char *argv[])
{

	tester_init(&argc, &argv);

	define_test("MICS/SR/SGGIT/SER/BV-01-C", test_server, NULL,
					MICS_SR_SGGIT_SER_BV_01_C);
	define_test("MICS/SR/SGGIT/CHA/BV-01-C", test_server, NULL,
					MICS_SR_SGGIT_CHA_BV_01_C);
	define_test("MICS/SR/SPE/BI-01-C", test_server, NULL,
					MICS_SR_SPE_BI_1_C);
	define_test("MICS/SR/SPE/BI-02-C", test_server, NULL,
					MICS_SR_SPE_BI_02_C);
	define_test("MICS/SR/SPN/BV-01-C", test_server, NULL,
					MICS_SR_SPN_BV_01_C);

	return tester_run();
}
