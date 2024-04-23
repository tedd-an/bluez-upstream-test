/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2024  Intel Corporation
 *
 */

struct telephony_ctrl {
	char	*device;	/* Device path */
	char	*path;		/* Telephony object path */
	char    *status;
	uint8_t	call_status;   /* call status of active call*/
	uint8_t call_index;    /* call index of active call */
	struct  call_callback	*cb;
};

struct telephony_control_callback {
	int (*call_answer)(struct telephony_ctrl *tc, void *user_data);
	int (*call_reject)(struct telephony_ctrl *tc, void *user_data);
};

struct telephony_ctrl *telephony_create_device(const char *path, uint16_t id);

void telephony_set_callbacks(struct telephony_ctrl *tc,
			     const struct telephony_control_callback *cbs,
			     void *user_data);

void telephony_destroy_device(struct telephony_ctrl *tc);

void telephony_set_incom_call_settings(struct telephony_ctrl *tc,
				       const char *key, void *data, size_t len);
void telephony_set_call_termination(struct telephony_ctrl *tc,
				    const char *key, void *data, size_t len);

void telephony_update_call_Info(struct telephony_ctrl *tc);

struct ccp_call_list_evt {
	uint8_t length;
	uint8_t index;
	uint8_t state;
	uint8_t flag;
};

struct ccp_incoming_call_evt {
	uint8_t length;
	uint8_t index;
};

struct ccp_call_terminate_evt {
	uint8_t length;
	uint8_t index;
	uint8_t reason;
};

enum call_state {
	INCOMING_CALL = 0,
	DIALLING_CALL,
	ALERTING_CALL,
	ACTIVE_CALL,
	LOCAL_HOLD,
	REMOTE_HOLD,
	CALL_DISCONNECTED = 10
};
