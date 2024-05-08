// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2024  Asymptotic Inc.
 *
 *  Author: Arun Raghavan <arun@asymptotic.io>
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <dbus/dbus.h>
#include <glib.h>

#include "gdbus/gdbus.h"
#include "lib/bluetooth.h"
#include "lib/uuid.h"

#include "src/dbus-common.h"
#include "src/adapter.h"
#include "src/device.h"
#include "src/log.h"
#include "src/plugin.h"
#include "src/profile.h"
#include "src/service.h"
#include "src/shared/att.h"
#include "src/shared/gatt-client.h"
#include "src/shared/gatt-db.h"
#include "src/shared/util.h"

#include "profiles/audio/media.h"
#include "profiles/audio/transport.h"

#include "asha.h"
#include "l2cap.h"

/* We use strings instead of uint128_t to maintain readability */
#define ASHA_CHRC_READ_ONLY_PROPERTIES_UUID "6333651e-c481-4a3e-9169-7c902aad37bb"
#define ASHA_CHRC_AUDIO_CONTROL_POINT_UUID "f0d4de7e-4a88-476c-9d9f-1937b0996cc0"
#define ASHA_CHRC_AUDIO_STATUS_UUID "38663f1a-e711-4cac-b641-326b56404837"
#define ASHA_CHRC_VOLUME_UUID "00e4ca9e-ab14-41e4-8823-f9e70c7e91df"
#define ASHA_CHRC_LE_PSM_OUT_UUID "2d410339-82b6-42aa-b34e-e2e01df8cc1a"

#define MEDIA_ENDPOINT_INTERFACE "org.bluez.MediaEndpoint1"

// 1 sequence number, 4 for L2CAP header, 2 for SDU, and then 20ms of G.722
#define ASHA_MTU 167

struct asha_device {
	struct btd_device *device;
	struct bt_gatt_client *client;
	struct gatt_db *db;
	struct gatt_db_attribute *attr;
	uint16_t acp_handle;
	uint16_t volume_handle;
	unsigned int status_notify_id;
	unsigned int volume_notify_id;

	uint16_t psm;
	bool right_side;
	bool binaural;
	bool csis_supported;
	bool coc_streaming_supported;
	uint8_t hisyncid[8];
	uint16_t render_delay;
	uint16_t codec_ids;
	int8_t volume;

	struct media_transport *transport;
	int fd;
	asha_state_t state;
	asha_cb_t cb;
	void *cb_user_data;
	int resume_id;
};

static struct asha_device *asha_device_new(void)
{
	struct asha_device *asha;

	asha = new0(struct asha_device, 1);

	return asha;
}

static void asha_device_reset(struct asha_device *asha)
{
	if (asha->status_notify_id) {
		bt_gatt_client_unregister_notify(asha->client,
						asha->status_notify_id);
	}

	if (asha->volume_notify_id) {
		bt_gatt_client_unregister_notify(asha->client,
						asha->volume_notify_id);
	}

	gatt_db_unref(asha->db);
	asha->db = NULL;

	bt_gatt_client_unref(asha->client);
	asha->client = NULL;

	asha->psm = 0;
}

static void asha_state_reset(struct asha_device *asha)
{
	close(asha->fd);
	asha->fd = -1;

	asha->state = ASHA_STOPPED;
	asha->resume_id = 0;

	asha->cb = NULL;
	asha->cb_user_data = NULL;
}

static void asha_device_free(struct asha_device *asha)
{
	gatt_db_unref(asha->db);
	bt_gatt_client_unref(asha->client);
	free(asha);
}

uint16_t asha_device_get_render_delay(struct asha_device *asha)
{
	return asha->render_delay;
}

asha_state_t asha_device_get_state(struct asha_device *asha)
{
	return asha->state;
}

int asha_device_get_fd(struct asha_device *asha)
{
	return asha->fd;
}

uint16_t asha_device_get_mtu(struct asha_device *asha)
{
	return ASHA_MTU;
}

static int asha_connect_socket(struct asha_device *asha)
{
	int fd = 0, err, ret = 0;
	struct sockaddr_l2 addr = { 0, };
	struct l2cap_options opts;

	fd = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
	if (fd < 0) {
		error("Could not open L2CAP CoC socket: %s", strerror(errno));
		goto error;
	}

	addr.l2_family = AF_BLUETOOTH;
	addr.l2_bdaddr_type = BDADDR_LE_PUBLIC;

	// We need to bind before connect to work around getting the wrong addr
	// type on older(?) kernels
	err = bind(fd, (struct sockaddr *) &addr, sizeof(addr));
	if (err < 0) {
		error("Could not bind L2CAP CoC socket: %s", strerror(errno));
		goto error;
	}

	addr.l2_psm = asha->psm;
	bacpy(&addr.l2_bdaddr, device_get_address(asha->device));

	opts.mode = BT_MODE_LE_FLOWCTL;
	opts.omtu = opts.imtu = ASHA_MTU;

	err = setsockopt(fd, SOL_BLUETOOTH, BT_MODE, &opts.mode,
							sizeof(opts.mode));
	if (err < 0) {
		error("Could not set L2CAP CoC socket flow control mode: %s",
				strerror(errno));
		// Let this be non-fatal?
	}

	err = setsockopt(fd, SOL_BLUETOOTH, BT_RCVMTU, &opts.imtu, sizeof(opts.imtu));
	if (err < 0) {
		error("Could not set L2CAP CoC socket receive MTU: %s",
				strerror(errno));
		// Let this be non-fatal?
	}

	err = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
	if (err < 0) {
		error("Could not connect L2CAP CoC socket: %s", strerror(errno));
		goto error;
	}

	DBG("L2CAP CoC socket is open");
	return fd;

error:
	if (fd)
		close(fd);
	return -1;
}

static void asha_acp_sent(bool success, uint8_t err, void *user_data)
{
	struct asha_device *asha = user_data;

	if (success) {
		DBG("AudioControlPoint command successfully sent");
	} else {
		error("Failed to send AudioControlPoint command: %d", err);

		if (asha->cb)
			asha->cb(-1, asha->cb_user_data);

		asha_state_reset(asha);
	}
}

static int asha_send_acp(struct asha_device *asha, uint8_t *cmd,
		unsigned int len, asha_cb_t cb, void *user_data)
{
	if (!bt_gatt_client_write_value(asha->client, asha->acp_handle, cmd,
				len, asha_acp_sent, asha, NULL)) {
		error("Error writing ACP start");
		return -1;
	}

	asha->cb = cb;
	asha->cb_user_data = user_data;

	return 0;
}

unsigned int asha_device_start(struct asha_device *asha, asha_cb_t cb,
		void *user_data)
{
	uint8_t acp_start_cmd[] = {
		0x01, // START
		0x01, // G.722, 16 kHz
		0,   // Unknown media type
		asha->volume, // Volume
		0,   // Other disconnected
	};
	int ret;

	if (asha->state != ASHA_STOPPED) {
		error("ASHA device start failed. Bad state %d", asha->state);
		return 0;
	}

	ret = asha_connect_socket(asha);
	if (ret < 0)
		return 0;

	asha->fd = ret;

	ret = asha_send_acp(asha, acp_start_cmd, sizeof(acp_start_cmd), cb,
			user_data);
	if (ret < 0)
		return 0;

	asha->state = ASHA_STARTING;

	return (++asha->resume_id);
}

unsigned int asha_device_stop(struct asha_device *asha, asha_cb_t cb,
		void *user_data)
{
	uint8_t acp_stop_cmd[] = {
		0x02, // STOP
	};
	int ret;

	if (asha->state != ASHA_STARTED)
		return 0;

	asha->state = ASHA_STOPPING;

	ret = asha_send_acp(asha, acp_stop_cmd, sizeof(acp_stop_cmd), cb,
			user_data);
	if (ret < 0)
		return 0;

	return asha->resume_id;
}

int8_t asha_device_get_volume(struct asha_device *asha)
{
	return asha->volume;
}

bool asha_device_set_volume(struct asha_device *asha, int8_t volume)
{
	if (!bt_gatt_client_write_value(asha->client, asha->volume_handle,
				(const uint8_t *)&volume, 1, NULL, NULL,
				NULL)) {
		error("Error writing ACP start");
		return false;
	}

	asha->volume = volume;
	return true;
}

static char *make_endpoint_path(struct asha_device *asha)
{
	char *path;
	int err;

	err = asprintf(&path, "%s/asha", device_get_path(asha->device));
	if (err < 0) {
		error("Could not allocate path for remote %s",
				device_get_path(asha->device));
		return NULL;
	}

	return path;

}

static bool uuid_cmp(const char *uuid1, const bt_uuid_t *uuid2)
{
	bt_uuid_t lhs;

	bt_string_to_uuid(&lhs, uuid1);

	return bt_uuid_cmp(&lhs, uuid2) == 0;
}

static void read_psm(bool success,
			uint8_t att_ecode,
			const uint8_t *value,
			uint16_t length,
			void *user_data)
{
	struct asha_device *asha = user_data;

	if (!success) {
		DBG("Reading PSM failed with ATT errror: %u", att_ecode);
		return;
	}

	if (length != 2) {
		DBG("Reading PSM failed: unexpected length %u", length);
		return;
	}

	asha->psm = get_le16(value);

	DBG("Got PSM: %u", asha->psm);
}

static void read_rops(bool success,
			uint8_t att_ecode,
			const uint8_t *value,
			uint16_t length,
			void *user_data)
{
	struct asha_device *asha = user_data;

	if (!success) {
		DBG("Reading ROPs failed with ATT errror: %u", att_ecode);
		return;
	}

	if (length != 17) {
		DBG("Reading ROPs failed: unexpected length %u", length);
		return;
	}

	if (value[0] != 0x01) {
		DBG("Unexpected ASHA version: %u", value[0]);
		return;
	}

	/* Device Capabilities */
	asha->right_side = (value[1] & 0x1) != 0;
	asha->binaural = (value[1] & 0x2) != 0;
	asha->csis_supported = (value[1] & 0x4) != 0;
	/* HiSyncId: 2 byte company id, 6 byte ID shared by left and right */
	memcpy(asha->hisyncid, &value[2], 8);
	/* FeatureMap */
	asha->coc_streaming_supported = (value[10] & 0x1) != 0;
	/* RenderDelay */
	asha->render_delay = get_le16(&value[11]);
	/* byte 13 & 14 are reserved */
	/* Codec IDs */
	asha->codec_ids = get_le16(&value[15]);

	DBG("Got ROPS: side %u, binaural %u, csis: %u, delay %u, codecs: %u",
			asha->right_side, asha->binaural, asha->csis_supported,
			asha->render_delay, asha->codec_ids);
}

void audio_status_notify(uint16_t value_handle, const uint8_t *value,
					uint16_t length, void *user_data)
{
	struct asha_device *asha = user_data;
	uint8_t status = *value;
	// Back these up to survive the reset paths
	asha_cb_t cb = asha->cb;
	asha_cb_t cb_user_data = asha->cb_user_data;

	if (asha->state == ASHA_STARTING) {
		if (status == 0) {
			asha->state = ASHA_STARTED;
			DBG("ASHA start complete");
		} else {
			asha_state_reset(asha);
			DBG("ASHA start failed");
		}
	} else if (asha->state == ASHA_STOPPING) {
		// We reset our state, regardless
		asha_state_reset(asha);
		DBG("ASHA stop %s", status == 0 ? "complete" : "failed");
	}

	if (cb) {
		cb(status, cb_user_data);
		asha->cb = NULL;
		asha->cb_user_data = NULL;
	}
}

static void read_volume(bool success,
			uint8_t att_ecode,
			const uint8_t *value,
			uint16_t length,
			void *user_data)
{
	struct asha_device *asha = user_data;

	if (!success) {
		DBG("Reading volume failed with ATT errror: %u", att_ecode);
		return;
	}

	if (length != 2) {
		DBG("Reading volume failed: unexpected length %u", length);
		return;
	}

	asha->volume = get_s8(value);

	DBG("Got volume: %d", asha->volume);
}

void volume_notify(uint16_t value_handle, const uint8_t *value,
					uint16_t length, void *user_data)
{
	struct asha_device *asha = user_data;

	asha->volume = get_s8(value);

	DBG("Volume changed: %d", asha->volume);
}

static void handle_characteristic(struct gatt_db_attribute *attr,
								void *user_data)
{
	struct asha_device *asha = user_data;
	uint16_t value_handle;
	bt_uuid_t uuid;

	if (!gatt_db_attribute_get_char_data(attr, NULL, &value_handle, NULL,
								NULL, &uuid)) {
		error("Failed to obtain characteristic data");
		return;
	}

	if (uuid_cmp(ASHA_CHRC_LE_PSM_OUT_UUID, &uuid)) {
		if (!bt_gatt_client_read_value(asha->client, value_handle,
					read_psm, asha, NULL))
			DBG("Failed to send request to read battery level");
	} if (uuid_cmp(ASHA_CHRC_READ_ONLY_PROPERTIES_UUID, &uuid)) {
		if (!bt_gatt_client_read_value(asha->client, value_handle,
					read_rops, asha, NULL))
			DBG("Failed to send request for readonly properties");
	} if (uuid_cmp(ASHA_CHRC_AUDIO_CONTROL_POINT_UUID, &uuid)) {
		// Store this for later writes
		asha->acp_handle = value_handle;
	} if (uuid_cmp(ASHA_CHRC_VOLUME_UUID, &uuid)) {
		// Store this for later reads and writes
		asha->volume_handle = value_handle;
		asha->volume_notify_id =
			bt_gatt_client_register_notify(asha->client,
				value_handle, NULL, volume_notify, asha,
				NULL);
		if (!asha->status_notify_id)
			DBG("Failed to send request to notify volume");
		if (!bt_gatt_client_read_value(asha->client, value_handle,
					read_volume, asha, NULL))
			DBG("Failed to send request to volume");
	} if (uuid_cmp(ASHA_CHRC_AUDIO_STATUS_UUID, &uuid)) {
		asha->status_notify_id =
			bt_gatt_client_register_notify(asha->client,
				value_handle, NULL, audio_status_notify, asha,
				NULL);
		if (!asha->status_notify_id)
			DBG("Failed to send request to notify AudioStatus");
	} else {
		char uuid_str[MAX_LEN_UUID_STR];

		bt_uuid_to_string(&uuid, uuid_str, sizeof(uuid_str));
		DBG("Unsupported characteristic: %s", uuid_str);
	}
}

static void foreach_asha_service(struct gatt_db_attribute *attr, void *user_data)
{
	struct asha_device *asha = user_data;

	DBG("Found ASHA GATT service");

	asha->attr = attr;
	gatt_db_service_foreach_char(asha->attr, handle_characteristic, asha);
}

static DBusMessage *asha_set_configuration(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	return NULL;
}

static gboolean get_uuid(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	const char *uuid;

	uuid = ASHA_PROFILE_UUID;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &uuid);

	return TRUE;
}

static gboolean get_side(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct asha_device *asha = data;
	const char *side = asha->right_side ? "right" : "left";

	// Use a string in case we want to support anything else in the future
	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &side);

	return TRUE;
}


static gboolean get_binaural(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct asha_device *asha = data;
	dbus_bool_t binaural = asha->binaural;

	// Use a string in case we want to support anything else in the future
	dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &binaural);

	return TRUE;
}

static gboolean get_hisyncid(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct asha_device *asha = data;
	DBusMessageIter array;
	uint8_t *hisyncid = asha->hisyncid;

	dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
					DBUS_TYPE_BYTE_AS_STRING, &array);

	dbus_message_iter_append_fixed_array(&array, DBUS_TYPE_BYTE,
			&hisyncid, sizeof(asha->hisyncid));

	dbus_message_iter_close_container(iter, &array);

	return TRUE;
}

static gboolean get_codecs(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct asha_device *asha = data;
	dbus_uint16_t codecs = asha->codec_ids;

	// Use a string in case we want to support anything else in the future
	dbus_message_iter_append_basic(iter, DBUS_TYPE_UINT16, &codecs);

	return TRUE;
}

static gboolean get_device(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct asha_device *asha = data;
	const char *path;

	path = device_get_path(asha->device);

	dbus_message_iter_append_basic(iter, DBUS_TYPE_OBJECT_PATH, &path);

	return TRUE;
}

static gboolean get_transport(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct asha_device *asha = data;
	const char *path;

	path = media_transport_get_path(asha->transport);

	dbus_message_iter_append_basic(iter, DBUS_TYPE_OBJECT_PATH, &path);

	return TRUE;
}

static int asha_source_device_probe(struct btd_service *service)
{
	struct asha_device *asha;
	struct btd_device *device = btd_service_get_device(service);
	char addr[18];

	ba2str(device_get_address(device), addr);
	DBG("Probing ASHA device %s", addr);

	asha = asha_device_new();
	asha->device = device;

	btd_service_set_user_data(service, asha);

	return 0;
}

static void asha_source_device_remove(struct btd_service *service)
{
	struct asha_device *asha;
	struct btd_device *device = btd_service_get_device(service);
	char addr[18];

	ba2str(device_get_address(device), addr);
	DBG("Removing ASHA device %s", addr);

	asha = btd_service_get_user_data(service);
	if (!asha) {
		// Can this actually happen?
		DBG("Not handlihng ASHA profile");
		return;
	}

	asha_device_free(asha);
}

static const GDBusMethodTable asha_ep_methods[] = {
	{ },
};

static const GDBusPropertyTable asha_ep_properties[] = {
	{ "UUID", "s", get_uuid, NULL, NULL,
					G_DBUS_PROPERTY_FLAG_EXPERIMENTAL },
	{ "Side", "s", get_side, NULL, NULL,
					G_DBUS_PROPERTY_FLAG_EXPERIMENTAL },
	{ "Binaural", "b", get_binaural, NULL, NULL,
					G_DBUS_PROPERTY_FLAG_EXPERIMENTAL },
	{ "HiSyncId", "ay", get_hisyncid, NULL, NULL,
					G_DBUS_PROPERTY_FLAG_EXPERIMENTAL },
	{ "Codecs", "q", get_codecs, NULL, NULL,
					G_DBUS_PROPERTY_FLAG_EXPERIMENTAL },
	{ "Device", "o", get_device, NULL, NULL,
					G_DBUS_PROPERTY_FLAG_EXPERIMENTAL },
	{ "Transport", "o", get_transport, NULL, NULL,
					G_DBUS_PROPERTY_FLAG_EXPERIMENTAL },
	{ }
};

static void asha_source_endpoint_register(struct asha_device *asha)
{
	char *path;
	const struct media_endpoint *asha_ep;

	path = make_endpoint_path(asha);
	if (!path)
		goto error;

	if (g_dbus_register_interface(btd_get_dbus_connection(),
				path, MEDIA_ENDPOINT_INTERFACE,
				asha_ep_methods, NULL,
				asha_ep_properties,
				asha, NULL) == FALSE) {
		error("Could not register remote ep %s", path);
		goto error;
	}

	asha_ep = media_endpoint_get_asha();
	asha->transport = media_transport_create(asha->device, path, NULL, 0,
			(void *) asha_ep, asha);

error:
	if (path)
		free(path);
	return;
}

static void asha_source_endpoint_unregister(struct asha_device *asha)
{
	char *path;

	path = make_endpoint_path(asha);
	if (!path)
		goto error;

	g_dbus_unregister_interface(btd_get_dbus_connection(),
				path, MEDIA_ENDPOINT_INTERFACE);

	if (asha->transport) {
		media_transport_destroy(asha->transport);
		asha->transport = NULL;
	}

error:
	if (path)
		free(path);
}

static int asha_source_accept(struct btd_service *service)
{
	struct btd_device *device = btd_service_get_device(service);
	struct gatt_db *db = btd_device_get_gatt_db(device);
	struct bt_gatt_client *client = btd_device_get_gatt_client(device);
	struct asha_device *asha = btd_service_get_user_data(service);
	bt_uuid_t asha_uuid;
	char addr[18];

	ba2str(device_get_address(device), addr);
	DBG("Accepting ASHA connection on %s", addr);

	if (!asha) {
		// Can this actually happen?
		DBG("Not handling ASHA profile");
		return -1;
	}

	asha->db = gatt_db_ref(db);
	asha->client = bt_gatt_client_clone(client);

	bt_uuid16_create(&asha_uuid, ASHA_SERVICE);
	gatt_db_foreach_service(db, &asha_uuid, foreach_asha_service, asha);

	if (!asha->attr) {
		error("ASHA attribute not found");
		asha_device_reset(asha);
		return -1;
	}

	asha_source_endpoint_register(asha);

	btd_service_connecting_complete(service, 0);

	return 0;
}

static int asha_source_disconnect(struct btd_service *service)
{
	struct btd_device *device = btd_service_get_device(service);
	struct gatt_db *db = btd_device_get_gatt_db(device);
	struct bt_gatt_client *client = btd_device_get_gatt_client(device);
	struct asha_device *asha = btd_service_get_user_data(service);
	bt_uuid_t asha_uuid;
	char addr[18];

	ba2str(device_get_address(device), addr);
	DBG("Disconnecting ASHA on %s", addr);

	if (!asha) {
		// Can this actually happen?
		DBG("Not handlihng ASHA profile");
		return -1;
	}

	asha_source_endpoint_unregister(asha);
	asha_device_reset(asha);

	btd_service_disconnecting_complete(service, 0);

	return 0;
}

static struct btd_profile asha_source_profile = {
	.name		= "asha-source",
	.priority	= BTD_PROFILE_PRIORITY_MEDIUM,
	.remote_uuid	= ASHA_PROFILE_UUID,
	.experimental	= true,

	.device_probe	= asha_source_device_probe,
	.device_remove	= asha_source_device_remove,

	.auto_connect	= true,
	.accept		= asha_source_accept,
	.disconnect	= asha_source_disconnect,
};

static int asha_init(void)
{
	int err;

	err = btd_profile_register(&asha_source_profile);
	if (err)
		return err;

	return 0;
}

static void asha_exit(void)
{
	btd_profile_unregister(&asha_source_profile);
}

BLUETOOTH_PLUGIN_DEFINE(asha, VERSION, BLUETOOTH_PLUGIN_PRIORITY_DEFAULT,
							asha_init, asha_exit)
