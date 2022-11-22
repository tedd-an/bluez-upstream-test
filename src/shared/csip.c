// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2022  Intel Corporation. All rights reserved.
 *
 */

#define _GNU_SOURCE
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>

#include <glib.h>

#include "lib/bluetooth.h"
#include "lib/uuid.h"

#include "src/shared/queue.h"
#include "src/shared/util.h"
#include "src/shared/timeout.h"
#include "src/shared/att.h"
#include "src/shared/gatt-db.h"
#include "src/shared/gatt-server.h"
#include "src/shared/gatt-client.h"
#include "src/shared/csip.h"
#include "src/btd.h"

#define DBG(_csip, fmt, arg...) \
	csip_debug(_csip, "%s:%s() " fmt, __FILE__, __func__, ## arg)

/* SIRK is now hardcoded in the code. This can be moved
 * to a configuration file. Since the code is to validate
 * the CSIP use case of set member
 */
#define SIRK "761FAE703ED681F0C50B34155B6434FB"
#define CSIS_SIZE	0x02
#define CSIS_LOCK	0x01
#define CSIS_RANK	0x01
#define CSIS_PLAINTEXT	0x01
#define CSIS_ENC	0x02

struct bt_csip_db {
	struct gatt_db *db;
	struct bt_csis *csis;
};

struct csis_sirk {
	uint8_t type;
	uint8_t val[16];
} __packed;

struct bt_csis {
	struct bt_csip_db *cdb;
	struct csis_sirk *sirk;
	uint8_t cs_size;
	uint8_t cs_lock;
	uint8_t cs_rank;
	struct gatt_db_attribute *service;
	struct gatt_db_attribute *csirk;
	struct gatt_db_attribute *csize;
	struct gatt_db_attribute *cslock;
	struct gatt_db_attribute *cslock_ccc;
	struct gatt_db_attribute *crank;
};

struct bt_csip_cb {
	unsigned int id;
	bt_csip_func_t attached;
	bt_csip_func_t detached;
	void *user_data;
};

struct bt_csip {
	int ref_count;
	struct bt_csip_db *ldb;
	struct bt_csip_db *rdb;
	struct bt_gatt_client *client;
	struct bt_att *att;

	struct queue *pending;

	bt_csip_debug_func_t debug_func;
	bt_csip_destroy_func_t debug_destroy;
	void *debug_data;
	void *user_data;
};

static struct queue *csip_db;
static struct queue *csip_cbs;
static struct queue *sessions;

static void csip_detached(void *data, void *user_data)
{
	struct bt_csip_cb *cb = data;
	struct bt_csip *csip = user_data;

	cb->detached(csip, cb->user_data);
}

void bt_csip_detach(struct bt_csip *csip)
{
	if (!queue_remove(sessions, csip))
		return;

	bt_gatt_client_unref(csip->client);
	csip->client = NULL;

	queue_foreach(csip_cbs, csip_detached, csip);
}

static void csip_db_free(void *data)
{
	struct bt_csip_db *cdb = data;

	if (!cdb)
		return;

	gatt_db_unref(cdb->db);

	free(cdb->csis);
	free(cdb);
}
static void csip_free(void *data)
{
	struct bt_csip *csip = data;

	bt_csip_detach(csip);

	csip_db_free(csip->rdb);

	queue_destroy(csip->pending, NULL);

	free(csip);
}

struct bt_att *bt_csip_get_att(struct bt_csip *csip)
{
	if (!csip)
		return NULL;

	if (csip->att)
		return csip->att;

	return bt_gatt_client_get_att(csip->client);
}

struct bt_csip *bt_csip_ref(struct bt_csip *csip)
{
	if (!csip)
		return NULL;

	__sync_fetch_and_add(&csip->ref_count, 1);

	return csip;
}

void bt_csip_unref(struct bt_csip *csip)
{
	if (!csip)
		return;

	if (__sync_sub_and_fetch(&csip->ref_count, 1))
		return;

	csip_free(csip);
}

static void csip_debug(struct bt_csip *csip, const char *format, ...)
{
	va_list ap;

	if (!csip || !format || !csip->debug_func)
		return;

	va_start(ap, format);
	util_debug_va(csip->debug_func, csip->debug_data, format, ap);
	va_end(ap);
}

static void csis_sirk_read(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	struct bt_csis *csis = user_data;
	struct iovec iov;

	iov.iov_base = csis->sirk;
	iov.iov_len = sizeof(struct csis_sirk);

	gatt_db_attribute_read_result(attrib, id, 0, iov.iov_base,
							iov.iov_len);
}

static void csis_size_read(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	struct bt_csis *csis = user_data;
	struct iovec iov;

	iov.iov_base = &csis->cs_size;
	iov.iov_len = sizeof(csis->cs_size);

	gatt_db_attribute_read_result(attrib, id, 0, iov.iov_base,
							iov.iov_len);
}

static void csis_lock_read_cb(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	uint8_t value = CSIS_LOCK;

	gatt_db_attribute_read_result(attrib, id, 0, &value, sizeof(value));
}

static void csis_lock_write_cb(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				const uint8_t *value, size_t len,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	gatt_db_attribute_write_result(attrib, id, 0);
}

static void csis_rank_read_cb(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	uint8_t value = CSIS_RANK;

	gatt_db_attribute_read_result(attrib, id, 0, &value, sizeof(value));
}

static struct bt_csis *csis_new(struct gatt_db *db)
{
	struct bt_csis *csis;
	struct csis_sirk *sirk;
	bt_uuid_t uuid;

	/* For Common Audio Service*/
	struct gatt_db_attribute *service;

	if (!db)
		return NULL;

	csis = new0(struct bt_csis, 1);
	sirk = new0(struct csis_sirk, 1);

	sirk->type = btd_opts.csis_defaults.sirk_type;
	memcpy(sirk->val, btd_opts.csis_defaults.sirk_val,
			sizeof(sirk->val));
	csis->sirk = sirk;
	csis->cs_size = btd_opts.csis_defaults.cs_size;
	csis->cs_lock = 1;
	csis->cs_rank = btd_opts.csis_defaults.cs_rank;

	/* Populate DB with CSIS attributes */
	bt_uuid16_create(&uuid, CSIS_UUID);
	csis->service = gatt_db_add_service(db, &uuid, true, 10);

	bt_uuid16_create(&uuid, CS_SIRK);
	csis->csirk = gatt_db_service_add_characteristic(csis->service,
					&uuid,
					BT_ATT_PERM_READ,
					BT_GATT_CHRC_PROP_READ,
					csis_sirk_read, NULL,
					csis);

	bt_uuid16_create(&uuid, CS_SIZE);
	csis->csize = gatt_db_service_add_characteristic(csis->service,
					&uuid,
					BT_ATT_PERM_READ,
					BT_GATT_CHRC_PROP_READ,
					csis_size_read, NULL,
					csis);

	/* Lock */
	bt_uuid16_create(&uuid, CS_LOCK);
	csis->cslock = gatt_db_service_add_characteristic(csis->service, &uuid,
					BT_ATT_PERM_READ,
					BT_GATT_CHRC_PROP_READ |
					BT_GATT_CHRC_PROP_WRITE |
					BT_GATT_CHRC_PROP_NOTIFY,
					csis_lock_read_cb,
					csis_lock_write_cb,
					csis);

	csis->cslock_ccc = gatt_db_service_add_ccc(csis->service,
					BT_ATT_PERM_READ | BT_ATT_PERM_WRITE);

	/* Rank */
	bt_uuid16_create(&uuid, CS_RANK);
	csis->crank = gatt_db_service_add_characteristic(csis->service, &uuid,
					BT_ATT_PERM_READ,
					BT_GATT_CHRC_PROP_READ,
					csis_rank_read_cb,
					NULL, csis);

	/* Add the CAS service */
	bt_uuid16_create(&uuid, 0x1853);
	service = gatt_db_add_service(db, &uuid, true, 2);
	gatt_db_service_add_included(service, csis->service);
	gatt_db_service_set_active(service, true);
	gatt_db_service_add_included(service, csis->service);

	gatt_db_service_set_active(csis->service, true);

	return csis;
}

static struct bt_csip_db *csip_db_new(struct gatt_db *db)
{
	struct bt_csip_db *cdb;

	if (!db)
		return NULL;

	cdb = new0(struct bt_csip_db, 1);
	cdb->db = gatt_db_ref(db);

	if (!csip_db)
		csip_db = queue_new();

	cdb->csis = csis_new(db);
	cdb->csis->cdb = cdb;

	queue_push_tail(csip_db, cdb);

	return cdb;
}

bool bt_csip_set_user_data(struct bt_csip *csip, void *user_data)
{
	if (!csip)
		return false;

	csip->user_data = user_data;

	return true;
}

static bool csip_db_match(const void *data, const void *match_data)
{
	const struct bt_csip_db *cdb = data;
	const struct gatt_db *db = match_data;

	return (cdb->db == db);
}

static struct bt_csip_db *csip_get_db(struct gatt_db *db)
{
	struct bt_csip_db *cdb;

	cdb = queue_find(csip_db, csip_db_match, db);
	if (cdb)
		return cdb;

	return csip_db_new(db);
}

void bt_csip_add_db(struct gatt_db *db)
{
	csip_db_new(db);
}

bool bt_csip_set_debug(struct bt_csip *csip, bt_csip_debug_func_t func,
			void *user_data, bt_csip_destroy_func_t destroy)
{
	if (!csip)
		return false;

	if (csip->debug_destroy)
		csip->debug_destroy(csip->debug_data);

	csip->debug_func = func;
	csip->debug_destroy = destroy;
	csip->debug_data = user_data;

	return true;
}

unsigned int bt_csip_register(bt_csip_func_t attached, bt_csip_func_t detached,
							void *user_data)
{
	struct bt_csip_cb *cb;
	static unsigned int id;

	if (!attached && !detached)
		return 0;

	if (!csip_cbs)
		csip_cbs = queue_new();

	cb = new0(struct bt_csip_cb, 1);
	cb->id = ++id ? id : ++id;
	cb->attached = attached;
	cb->detached = detached;
	cb->user_data = user_data;

	queue_push_tail(csip_cbs, cb);

	return cb->id;
}

static bool match_id(const void *data, const void *match_data)
{
	const struct bt_csip_cb *cb = data;
	unsigned int id = PTR_TO_UINT(match_data);

	return (cb->id == id);
}

bool bt_csip_unregister(unsigned int id)
{
	struct bt_csip_cb *cb;

	cb = queue_remove_if(csip_cbs, match_id, UINT_TO_PTR(id));
	if (!cb)
		return false;

	free(cb);

	return true;
}

struct bt_csip *bt_csip_new(struct gatt_db *ldb, struct gatt_db *rdb)
{
	struct bt_csip *csip;
	struct bt_csip_db *db;

	if (!ldb)
		return NULL;

	db = csip_get_db(ldb);
	if (!db)
		return NULL;

	csip = new0(struct bt_csip, 1);
	csip->ldb = db;
	csip->pending = queue_new();

	if (!rdb)
		goto done;

	db = new0(struct bt_csip_db, 1);
	db->db = gatt_db_ref(rdb);

	csip->rdb = db;

done:
	bt_csip_ref(csip);

	return csip;
}

static struct bt_csis *csip_get_csis(struct bt_csip *csip)
{
	if (!csip)
		return NULL;

	if (csip->rdb->csis)
		return csip->rdb->csis;

	csip->rdb->csis = new0(struct bt_csis, 1);
	csip->rdb->csis->cdb = csip->rdb;

	return csip->rdb->csis;
}

static void foreach_csis_char(struct gatt_db_attribute *attr, void *user_data)
{
	struct bt_csip *csip = user_data;
	uint16_t value_handle;
	bt_uuid_t uuid, uuid_csirk, uuid_csize;
	struct bt_csis *csis;

	if (!gatt_db_attribute_get_char_data(attr, NULL, &value_handle,
						NULL, NULL, &uuid))
		return;

	bt_uuid16_create(&uuid_csirk, CS_SIRK);
	bt_uuid16_create(&uuid_csize, CS_SIZE);

	if (!bt_uuid_cmp(&uuid, &uuid_csirk)) {
		DBG(csip, "CSIS IRK found: handle 0x%04x", value_handle);

		csis = csip_get_csis(csip);
		if (!csis || csis->sirk)
			return;

		csis->csirk = attr;
		return;
	}

	if (!bt_uuid_cmp(&uuid, &uuid_csize)) {
		DBG(csip, "CSIS SIZE found: handle 0x%04x", value_handle);

		csis = csip_get_csis(csip);
		if (!csis)
			return;

		csis->csize = attr;
	}

}
static void foreach_csis_service(struct gatt_db_attribute *attr,
						void *user_data)
{
	struct bt_csip *csip = user_data;
	struct bt_csis *csis = csip_get_csis(csip);

	csis->service = attr;

	gatt_db_service_set_claimed(attr, true);

	gatt_db_service_foreach_char(attr, foreach_csis_char, csip);
}

bool bt_csip_attach(struct bt_csip *csip, struct bt_gatt_client *client)
{
	bt_uuid_t uuid;

	if (!sessions)
		sessions = queue_new();

	queue_push_tail(sessions, csip);

	if (!client)
		return true;

	if (csip->client)
		return false;

	csip->client = bt_gatt_client_clone(client);
	if (!csip->client)
		return false;

	bt_uuid16_create(&uuid, CSIS_UUID);
	gatt_db_foreach_service(csip->ldb->db, &uuid, foreach_csis_service,
				csip);

	return true;
}

