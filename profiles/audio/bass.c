// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright 2023 NXP
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>

#include <glib.h>

#include "gdbus/gdbus.h"

#include "lib/bluetooth.h"
#include "lib/uuid.h"
#include "lib/iso.h"

#include "btio/btio.h"

#include "src/dbus-common.h"
#include "src/shared/util.h"
#include "src/shared/att.h"
#include "src/shared/queue.h"
#include "src/shared/gatt-db.h"
#include "src/shared/gatt-client.h"
#include "src/shared/gatt-server.h"
#include "src/adapter.h"
#include "src/shared/bass.h"

#include "src/plugin.h"
#include "src/gatt-database.h"
#include "src/device.h"
#include "src/profile.h"
#include "src/service.h"
#include "src/log.h"
#include "src/error.h"

#define BASS_UUID_STR "0000184f-0000-1000-8000-00805f9b34fb"

struct bass_data {
	struct btd_device *device;
	struct btd_service *service;
	struct bt_bass *bass;

	unsigned int io_cb_id;
};

static struct queue *sessions;

struct bt_bass_io {
	GIOChannel *listen;
	guint listen_io_id;
	GIOChannel *pa;
	guint pa_io_id;
	struct queue *bises;
};

#define MAX_BIS_BITMASK_IDX		31

#define DEFAULT_IO_QOS \
{ \
	.interval	= 10000, \
	.latency	= 10, \
	.sdu		= 40, \
	.phy		= 0x02, \
	.rtn		= 2, \
}

static struct bt_iso_qos default_qos = {
	.bcast = {
		.big			= BT_ISO_QOS_BIG_UNSET,
		.bis			= BT_ISO_QOS_BIS_UNSET,
		.sync_factor		= 0x07,
		.packing		= 0x00,
		.framing		= 0x00,
		.in			= DEFAULT_IO_QOS,
		.out			= DEFAULT_IO_QOS,
		.encryption		= 0x00,
		.bcode			= {0x00},
		.options		= 0x00,
		.skip			= 0x0000,
		.sync_timeout		= 0x4000,
		.sync_cte_type		= 0x00,
		.mse			= 0x00,
		.timeout		= 0x4000,
	}
};

static void bass_debug(const char *str, void *user_data)
{
	DBG_IDX(0xffff, "%s", str);
}

static struct bass_data *bass_data_new(struct btd_device *device)
{
	struct bass_data *data;

	data = new0(struct bass_data, 1);
	data->device = device;

	return data;
}

static void bass_data_add(struct bass_data *data)
{
	DBG("data %p", data);

	if (queue_find(sessions, NULL, data)) {
		error("data %p already added", data);
		return;
	}

	bt_bass_set_debug(data->bass, bass_debug, NULL, NULL);

	if (!sessions)
		sessions = queue_new();

	queue_push_tail(sessions, data);

	if (data->service)
		btd_service_set_user_data(data->service, data);
}

static bool match_data(const void *data, const void *match_data)
{
	const struct bass_data *bdata = data;
	const struct bt_bass *bass = match_data;

	return bdata->bass == bass;
}

static void bass_data_free(struct bass_data *data)
{
	if (data->service) {
		btd_service_set_user_data(data->service, NULL);
		bt_bass_set_user_data(data->bass, NULL);
	}

	bt_bass_unref(data->bass);
	free(data);
}

static void bass_data_remove(struct bass_data *data)
{
	DBG("data %p", data);

	if (!queue_remove(sessions, data))
		return;

	bass_data_free(data);

	if (queue_isempty(sessions)) {
		queue_destroy(sessions, NULL);
		sessions = NULL;
	}
}

static void bass_detached(struct bt_bass *bass, void *user_data)
{
	struct bass_data *data;

	DBG("%p", bass);

	data = queue_find(sessions, match_data, bass);
	if (!data) {
		error("Unable to find bass session");
		return;
	}

	/* If there is a service it means there is BASS thus we can keep
	 * instance allocated.
	 */
	if (data->service)
		return;

	bass_data_remove(data);
}

static gboolean check_io_err(GIOChannel *io)
{
	struct pollfd fds;

	memset(&fds, 0, sizeof(fds));
	fds.fd = g_io_channel_unix_get_fd(io);
	fds.events = POLLERR;

	if (poll(&fds, 1, 0) > 0 && (fds.revents & POLLERR))
		return TRUE;

	return FALSE;
}

static gboolean pa_io_disconnect_cb(GIOChannel *io, GIOCondition cond,
			gpointer data)
{
	struct bt_bcast_src *bcast_src = data;

	DBG("PA sync io has been disconnected");

	bcast_src->io->pa_io_id = 0;
	g_io_channel_unref(bcast_src->io->pa);
	bcast_src->io->pa = NULL;

	return FALSE;
}

static void confirm_cb(GIOChannel *io, gpointer user_data)
{
	struct bt_bcast_src *bcast_src = user_data;
	int sk, err;
	socklen_t len;
	struct bt_iso_qos qos;

	if (!bcast_src || !bcast_src->bass)
		return;

	if (check_io_err(io)) {
		DBG("PA sync failed");

		/* Mark PA sync as failed and notify client */
		bcast_src->sync_state = BT_BASS_FAILED_TO_SYNCHRONIZE_TO_PA;
		goto notify;
	}

	bcast_src->sync_state = BT_BASS_SYNCHRONIZED_TO_PA;
	bcast_src->io->pa = io;
	g_io_channel_ref(bcast_src->io->pa);

	bcast_src->io->pa_io_id = g_io_add_watch(io, G_IO_ERR |
					G_IO_HUP | G_IO_NVAL,
					(GIOFunc) pa_io_disconnect_cb,
					bcast_src);

	len = sizeof(qos);
	memset(&qos, 0, len);

	sk = g_io_channel_unix_get_fd(io);

	err = getsockopt(sk, SOL_BLUETOOTH, BT_ISO_QOS, &qos, &len);
	if (err < 0) {
		DBG("Failed to get iso qos");
		return;
	}

	if (!qos.bcast.encryption)
		/* BIG is not encrypted. Try to synchronize */
		bcast_src->enc = BT_BASS_BIG_ENC_STATE_NO_ENC;
	else
		/* BIG is encrypted. Wait for Client to provide the
		 * Broadcast_Code
		 */
		bcast_src->enc = BT_BASS_BIG_ENC_STATE_BCODE_REQ;

notify:
	if (bcast_src->confirm_cb)
		bcast_src->confirm_cb(bcast_src);
}

static gboolean listen_io_disconnect_cb(GIOChannel *io, GIOCondition cond,
			gpointer data)
{
	struct bt_bcast_src *bcast_src = data;

	DBG("Listen io has been disconnected");

	bcast_src->io->listen_io_id = 0;
	g_io_channel_unref(bcast_src->io->listen);
	bcast_src->io->listen = NULL;

	return FALSE;
}

static int bass_io_listen(struct bt_bcast_src *bcast_src,
				const bdaddr_t *src)
{
	uint8_t addr_type;
	GError *err = NULL;
	struct bt_iso_qos iso_qos = default_qos;
	uint8_t num_bis = 0;
	uint8_t bis[ISO_MAX_NUM_BIS];

	if (!bcast_src)
		return -1;

	if (!bcast_src->io) {
		bcast_src->io = malloc(sizeof(*bcast_src->io));
		if (!bcast_src->io)
			return -1;

		memset(bcast_src->io, 0, sizeof(*bcast_src->io));
	}

	memset(bis, 0, ISO_MAX_NUM_BIS);

	for (int i = 0; i < bcast_src->num_subgroups; i++) {
		struct bt_bass_subgroup_data *data =
				&bcast_src->subgroup_data[i];

		if (data->pending_bis_sync != BIS_SYNC_NO_PREF)
			/* Iterate through the bis sync bitmask written
			 * by the client and store the bis indexes that
			 * the BASS server will try to synchronize to
			 */
			for (int bis_idx = 0; bis_idx < 31; bis_idx++) {
				if (data->pending_bis_sync & (1 << bis_idx)) {
					bis[num_bis] = bis_idx + 1;
					num_bis++;
				}
			}
	}

	/* Convert to three-value type */
	if (bcast_src->addr_type)
		addr_type = BDADDR_LE_RANDOM;
	else
		addr_type = BDADDR_LE_PUBLIC;

	/* Try to synchronize to the source */
	bcast_src->io->listen = bt_io_listen(NULL, confirm_cb,
				bcast_src, NULL, &err,
				BT_IO_OPT_SOURCE_BDADDR,
				src,
				BT_IO_OPT_DEST_BDADDR,
				&bcast_src->addr,
				BT_IO_OPT_DEST_TYPE,
				addr_type,
				BT_IO_OPT_MODE, BT_IO_MODE_ISO,
				BT_IO_OPT_QOS, &iso_qos,
				BT_IO_OPT_ISO_BC_SID, bcast_src->sid,
				BT_IO_OPT_ISO_BC_NUM_BIS, num_bis,
				BT_IO_OPT_ISO_BC_BIS, bis,
				BT_IO_OPT_INVALID);

	if (!bcast_src->io->listen) {
		DBG("%s", err->message);
		g_error_free(err);
		return -1;
	}

	g_io_channel_ref(bcast_src->io->listen);

	bcast_src->io->listen_io_id = g_io_add_watch(bcast_src->io->listen,
					G_IO_ERR | G_IO_HUP | G_IO_NVAL,
					(GIOFunc)listen_io_disconnect_cb,
					bcast_src);

	if (num_bis > 0 && !bcast_src->io->bises)
		bcast_src->io->bises = queue_new();

	return 0;
}

static void bass_bis_unref(void *data)
{
	GIOChannel *io = data;

	g_io_channel_unref(io);
}

static void connect_cb(GIOChannel *io, GError *gerr,
				gpointer user_data)
{
	struct bt_bcast_src *bcast_src = user_data;
	int bis_idx;
	int i;

	if (!bcast_src || !bcast_src->bass)
		return;

	/* Keep io reference */
	g_io_channel_ref(io);
	queue_push_tail(bcast_src->io->bises, io);

	for (i = 0; i < bcast_src->num_subgroups; i++) {
		struct bt_bass_subgroup_data *data =
				&bcast_src->subgroup_data[i];

		for (bis_idx = 0; bis_idx < MAX_BIS_BITMASK_IDX; bis_idx++) {
			if (data->pending_bis_sync & (1 << bis_idx)) {
				data->bis_sync |= (1 << bis_idx);
				data->pending_bis_sync &= ~(1 << bis_idx);
				break;
			}
		}

		if (bis_idx < MAX_BIS_BITMASK_IDX)
			break;
	}

	for (i = 0; i < bcast_src->num_subgroups; i++) {
		if (bcast_src->subgroup_data[i].pending_bis_sync)
			break;
	}

	/* Wait until all BISes have been connected */
	if (i != bcast_src->num_subgroups)
		return;

	if (check_io_err(io)) {
		DBG("BIG sync failed");

		queue_destroy(bcast_src->io->bises, bass_bis_unref);

		bcast_src->io->bises = NULL;

		/* Close listen io */
		g_io_channel_shutdown(bcast_src->io->listen, TRUE, NULL);
		g_io_channel_unref(bcast_src->io->listen);

		bcast_src->io->listen = NULL;

		if (bcast_src->io->listen_io_id > 0) {
			g_source_remove(bcast_src->io->listen_io_id);
			bcast_src->io->listen_io_id  = 0;
		}

		/* Close PA io */
		g_io_channel_shutdown(bcast_src->io->pa, TRUE, NULL);
		g_io_channel_unref(bcast_src->io->pa);

		bcast_src->io->pa = NULL;

		if (bcast_src->io->pa_io_id > 0) {
			g_source_remove(bcast_src->io->pa_io_id);
			bcast_src->io->pa_io_id  = 0;
		}

		for (i = 0; i < bcast_src->num_subgroups; i++)
			bcast_src->subgroup_data[i].bis_sync =
				BT_BASS_BIG_SYNC_FAILED_BITMASK;

		/* If BIG sync failed because of an incorrect broadcast code,
		 * inform client
		 */
		if (bcast_src->enc == BT_BASS_BIG_ENC_STATE_BCODE_REQ)
			bcast_src->enc = BT_BASS_BIG_ENC_STATE_BAD_CODE;
	} else {
		if (bcast_src->enc == BT_BASS_BIG_ENC_STATE_BCODE_REQ)
			bcast_src->enc = BT_BASS_BIG_ENC_STATE_DEC;
	}

	if (bcast_src->connect_cb)
		bcast_src->connect_cb(bcast_src);
}

static int bass_io_accept(struct bt_bcast_src *bcast_src)
{
	int sk, err;
	socklen_t len;
	struct bt_iso_qos qos;
	GError *gerr = NULL;

	if (!bcast_src || !bcast_src->io || !bcast_src->io->pa)
		return -1;

	if (bcast_src->enc == BT_BASS_BIG_ENC_STATE_BCODE_REQ) {
		/* Update socket QoS with Broadcast Code */
		len = sizeof(qos);
		memset(&qos, 0, len);

		sk = g_io_channel_unix_get_fd(bcast_src->io->pa);

		err = getsockopt(sk, SOL_BLUETOOTH, BT_ISO_QOS, &qos, &len);
		if (err < 0) {
			DBG("Failed to get iso qos");
			return -1;
		}

		memcpy(qos.bcast.bcode, bcast_src->bcode,
			BT_BASS_BCAST_CODE_SIZE);

		if (setsockopt(sk, SOL_BLUETOOTH, BT_ISO_QOS, &qos,
					sizeof(qos)) < 0) {
			DBG("Failed to set iso qos");
			return -1;
		}
	}

	if (!bt_io_bcast_accept(bcast_src->io->pa,
		connect_cb, bcast_src, NULL, &gerr,
		BT_IO_OPT_INVALID)) {
		DBG("bt_io_accept: %s", gerr->message);
		g_error_free(gerr);
		return -1;
	}

	return 0;
}

static void bass_io_destroy(struct bt_bcast_src *bcast_src)
{
	if (!bcast_src || !bcast_src->io)
		return;

	queue_destroy(bcast_src->io->bises, bass_bis_unref);
	bcast_src->io->bises = NULL;

	if (bcast_src->io->listen) {
		g_io_channel_shutdown(bcast_src->io->listen, TRUE, NULL);
		g_io_channel_unref(bcast_src->io->listen);
		bcast_src->io->listen = NULL;
	}

	if (bcast_src->io->listen_io_id > 0) {
		g_source_remove(bcast_src->io->listen_io_id);
		bcast_src->io->listen_io_id  = 0;
	}

	if (bcast_src->io->pa) {
		g_io_channel_shutdown(bcast_src->io->pa, TRUE, NULL);
		g_io_channel_unref(bcast_src->io->pa);
		bcast_src->io->pa = NULL;
	}

	if (bcast_src->io->pa_io_id > 0) {
		g_source_remove(bcast_src->io->pa_io_id);
		bcast_src->io->pa_io_id  = 0;
	}

	free(bcast_src->io);
}

static void bass_attached(struct bt_bass *bass, void *user_data)
{
	struct bass_data *data;
	struct bt_att *att;
	struct btd_device *device;

	DBG("%p", bass);

	data = queue_find(sessions, match_data, bass);
	if (data)
		return;

	att = bt_bass_get_att(bass);
	if (!att)
		return;

	device = btd_adapter_find_device_by_fd(bt_att_get_fd(att));
	if (!device) {
		error("Unable to find device");
		return;
	}

	data = bass_data_new(device);
	data->bass = bass;

	/* Register io callbacks */
	data->io_cb_id = bt_bass_io_cb_register(bass, bass_io_listen,
			bass_io_accept, bass_io_destroy);

	bass_data_add(data);
}

static int bass_probe(struct btd_service *service)
{
	struct btd_device *device = btd_service_get_device(service);
	struct btd_adapter *adapter = device_get_adapter(device);
	struct btd_gatt_database *database = btd_adapter_get_database(adapter);
	struct bass_data *data = btd_service_get_user_data(service);
	char addr[18];

	ba2str(device_get_address(device), addr);
	DBG("%s", addr);

	/* Ignore, if we were probed for this device already */
	if (data) {
		error("Profile probed twice for the same device!");
		return -EINVAL;
	}

	data = bass_data_new(device);
	data->service = service;

	data->bass = bt_bass_new(btd_gatt_database_get_db(database),
					btd_device_get_gatt_db(device),
					btd_adapter_get_address(adapter));
	if (!data->bass) {
		error("Unable to create BASS instance");
		free(data);
		return -EINVAL;
	}

	/* Register io callbacks */
	data->io_cb_id = bt_bass_io_cb_register(data->bass, bass_io_listen,
			bass_io_accept, bass_io_destroy);

	bass_data_add(data);
	bt_bass_set_user_data(data->bass, service);

	return 0;
}

static void bass_remove(struct btd_service *service)
{
	struct btd_device *device = btd_service_get_device(service);
	struct bass_data *data;
	char addr[18];

	ba2str(device_get_address(device), addr);
	DBG("%s", addr);

	data = btd_service_get_user_data(service);
	if (!data) {
		error("BASS service not handled by profile");
		return;
	}

	bass_data_remove(data);
}
static int bass_accept(struct btd_service *service)
{
	struct btd_device *device = btd_service_get_device(service);
	struct bt_gatt_client *client = btd_device_get_gatt_client(device);
	struct bass_data *data = btd_service_get_user_data(service);
	char addr[18];

	ba2str(device_get_address(device), addr);
	DBG("%s", addr);

	if (!data) {
		error("BASS service not handled by profile");
		return -EINVAL;
	}

	if (!bt_bass_attach(data->bass, client)) {
		error("BASS unable to attach");
		return -EINVAL;
	}

	btd_service_connecting_complete(service, 0);

	return 0;
}

static int bass_disconnect(struct btd_service *service)
{
	struct bass_data *data = btd_service_get_user_data(service);
	struct btd_device *device = btd_service_get_device(service);
	char addr[18];

	ba2str(device_get_address(device), addr);
	DBG("%s", addr);

	bt_bass_detach(data->bass);

	btd_service_disconnecting_complete(service, 0);

	return 0;
}

static int bass_server_probe(struct btd_profile *p,
				struct btd_adapter *adapter)
{
	struct btd_gatt_database *database = btd_adapter_get_database(adapter);

	DBG("BASS path %s", adapter_get_path(adapter));

	bt_bass_add_db(btd_gatt_database_get_db(database),
				btd_adapter_get_address(adapter));

	return 0;
}

static void bass_server_remove(struct btd_profile *p,
					struct btd_adapter *adapter)
{
	DBG("BASS remove Adapter");
}

static struct btd_profile bass_service = {
	.name		= "bass",
	.priority	= BTD_PROFILE_PRIORITY_MEDIUM,
	.remote_uuid	= BASS_UUID_STR,
	.device_probe	= bass_probe,
	.device_remove	= bass_remove,
	.accept		= bass_accept,
	.disconnect	= bass_disconnect,
	.adapter_probe	= bass_server_probe,
	.adapter_remove	= bass_server_remove,
	.experimental	= true,
};

static unsigned int bass_id;

static int bass_init(void)
{
	int err;

	err = btd_profile_register(&bass_service);
	if (err)
		return err;

	bass_id = bt_bass_register(bass_attached, bass_detached, NULL);

	return 0;
}

static void bass_exit(void)
{
	btd_profile_unregister(&bass_service);
	bt_bass_unregister(bass_id);
}

BLUETOOTH_PLUGIN_DEFINE(bass, VERSION, BLUETOOTH_PLUGIN_PRIORITY_DEFAULT,
							bass_init, bass_exit)
