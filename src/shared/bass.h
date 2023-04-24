/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright 2023 NXP
 *
 */

#define NUM_BCAST_RECV_STATES				2
#define BT_BASS_BCAST_CODE_SIZE				16
#define BT_BASS_BIG_SYNC_FAILED_BITMASK			0xFFFFFFFF
#define BT_BASS_BCAST_SRC_LEN				15
#define BT_BASS_BCAST_SRC_SUBGROUP_LEN			5

/* Application error codes */
#define BT_BASS_ERROR_OPCODE_NOT_SUPPORTED		0x80
#define BT_BASS_ERROR_INVALID_SOURCE_ID			0x81

/* PA_Sync_State values */
#define BT_BASS_NOT_SYNCHRONIZED_TO_PA			0x00
#define BT_BASS_SYNC_INFO_RE				0x01
#define BT_BASS_SYNCHRONIZED_TO_PA			0x02
#define BT_BASS_FAILED_TO_SYNCHRONIZE_TO_PA		0x03
#define BT_BASS_NO_PAST					0x04

/* BIG_Encryption values */
#define BT_BASS_BIG_ENC_STATE_NO_ENC			0x00
#define BT_BASS_BIG_ENC_STATE_BCODE_REQ			0x01
#define BT_BASS_BIG_ENC_STATE_DEC			0x02
#define BT_BASS_BIG_ENC_STATE_BAD_CODE			0x03

/* BASS subgroup field of the Broadcast
 * Receive State characteristic
 */
struct bt_bass_subgroup_data {
	uint32_t bis_sync;
	uint32_t pending_bis_sync;
	uint8_t meta_len;
	uint8_t *meta;
};

/* BASS Broadcast Source structure */
struct bt_bcast_src {
	struct bt_bap *bap;
	struct gatt_db_attribute *attr;
	uint8_t id;
	uint8_t addr_type;
	bdaddr_t addr;
	uint8_t sid;
	uint32_t bid;
	uint8_t sync_state;
	uint8_t enc;
	uint8_t bad_code[BT_BASS_BCAST_CODE_SIZE];
	uint8_t num_subgroups;
	struct bt_bass_subgroup_data *subgroup_data;
};

/* Broadcast Receive State characteristic structure */
struct bt_bcast_recv_state {
	struct bt_bass *bass;
	struct gatt_db_attribute *attr;
	struct gatt_db_attribute *ccc;
};

/* BASS instance structure */
struct bt_bass {
	struct bt_bap_db *bdb;
	struct gatt_db_attribute *service;
	struct gatt_db_attribute *bcast_audio_scan_cp;
	struct bt_bcast_recv_state *bcast_recv_states[NUM_BCAST_RECV_STATES];
};

/* Broadcast Audio Scan Control Point
 * header structure
 */
struct bt_bass_bcast_audio_scan_cp_hdr {
	uint8_t op;
} __packed;

#define BT_BASS_REMOTE_SCAN_STOPPED			0x00

#define BT_BASS_REMOTE_SCAN_STARTED			0x01

#define BT_BASS_ADD_SRC					0x02

struct bt_bass_add_src_params {
	uint8_t addr_type;
	bdaddr_t addr;
	uint8_t sid;
	uint8_t bid[3];
	uint8_t pa_sync;
	uint16_t pa_interval;
	uint8_t num_subgroups;
	uint8_t subgroup_data[];
} __packed;

#define BT_BASS_MOD_SRC					0x03

struct bt_bass_mod_src_params {
	uint8_t id;
	uint8_t pa_sync;
	uint16_t pa_interval;
	uint8_t num_subgroups;
	uint8_t subgroup_data[];
} __packed;

#define BT_BASS_SET_BCAST_CODE				0x04

struct bt_bass_set_bcast_code_params {
	uint8_t id;
	uint8_t bcast_code[BT_BASS_BCAST_CODE_SIZE];
} __packed;

#define BT_BASS_REMOVE_SRC				0x05

struct bt_bass_remove_src_params {
	uint8_t id;
} __packed;

struct bt_bass *bass_new(struct gatt_db *db);
void bass_bcast_src_free(void *data);
void foreach_bass_service(struct gatt_db_attribute *attr,
						void *user_data);
