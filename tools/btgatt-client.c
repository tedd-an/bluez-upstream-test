// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2014  Google Inc.
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <limits.h>
#include <errno.h>

#include "lib/bluetooth.h"
#include "lib/hci.h"
#include "lib/hci_lib.h"
#include "lib/l2cap.h"
#include "lib/uuid.h"

#include "src/shared/mainloop.h"
#include "src/shared/shell.h"
#include "src/shared/util.h"
#include "src/shared/att.h"
#include "src/shared/queue.h"
#include "src/shared/gatt-db.h"
#include "src/shared/gatt-client.h"
#include "src/shared/gatt-helpers.h"

#define ATT_CID 4
#define ATT_PSM 31

#define MAX_LEN_LINE 512

struct client *cli = NULL;
static bool verbose = false;
static bool shell_running = false;
static uint8_t dst_type = BDADDR_LE_PUBLIC;
static bdaddr_t src_addr, dst_addr;
static int security_level = BT_SECURITY_LOW;
static uint16_t mtu = 0;

#define print(fmt, arg...) do { \
	if (shell_running) \
		bt_shell_printf(fmt "\n", ## arg); \
	else \
		printf(fmt "\n", ## arg); \
} while (0)

#define error(fmt, arg...) do { \
	if (shell_running) \
		bt_shell_printf(COLOR_RED fmt "\n" COLOR_OFF, ## arg); \
	else \
		fprintf(stderr, COLOR_RED fmt "\n" COLOR_OFF, ## arg); \
} while (0)

#define append(str, fmt, arg...) do { \
	sprintf(strchr(str, '\0'), fmt, ## arg); \
} while (0)

struct client {
	int fd;
	struct bt_att *att;
	struct gatt_db *db;
	struct bt_gatt_client *gatt;

	unsigned int reliable_session_id;
};

static int l2cap_att_connect(bdaddr_t *src, bdaddr_t *dst, uint8_t dst_type,
									int sec);

static void update_prompt(void)
{
	char str[64], addr[18], type[3];
	if (!bacmp(&dst_addr, BDADDR_ANY))
		sprintf(str, "[GATT client]# ");
	else {
		ba2str(&dst_addr, addr);
		sprintf(type, dst_type == BDADDR_BREDR ? "BR" : "LE");
		if (cli)
			sprintf(str, COLOR_BLUE "[%s][%s]" COLOR_OFF "# ", addr, type);
		else
			sprintf(str, "[%s][%s]# ", addr, type);
	}
	bt_shell_set_prompt(str);
}

static const char *ecode_to_string(uint8_t ecode)
{
	switch (ecode) {
	case BT_ATT_ERROR_INVALID_HANDLE:
		return "Invalid Handle";
	case BT_ATT_ERROR_READ_NOT_PERMITTED:
		return "Read Not Permitted";
	case BT_ATT_ERROR_WRITE_NOT_PERMITTED:
		return "Write Not Permitted";
	case BT_ATT_ERROR_INVALID_PDU:
		return "Invalid PDU";
	case BT_ATT_ERROR_AUTHENTICATION:
		return "Authentication Required";
	case BT_ATT_ERROR_REQUEST_NOT_SUPPORTED:
		return "Request Not Supported";
	case BT_ATT_ERROR_INVALID_OFFSET:
		return "Invalid Offset";
	case BT_ATT_ERROR_AUTHORIZATION:
		return "Authorization Required";
	case BT_ATT_ERROR_PREPARE_QUEUE_FULL:
		return "Prepare Write Queue Full";
	case BT_ATT_ERROR_ATTRIBUTE_NOT_FOUND:
		return "Attribute Not Found";
	case BT_ATT_ERROR_ATTRIBUTE_NOT_LONG:
		return "Attribute Not Long";
	case BT_ATT_ERROR_INSUFFICIENT_ENCRYPTION_KEY_SIZE:
		return "Insuficient Encryption Key Size";
	case BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN:
		return "Invalid Attribute value len";
	case BT_ATT_ERROR_UNLIKELY:
		return "Unlikely Error";
	case BT_ATT_ERROR_INSUFFICIENT_ENCRYPTION:
		return "Insufficient Encryption";
	case BT_ATT_ERROR_UNSUPPORTED_GROUP_TYPE:
		return "Group type Not Supported";
	case BT_ATT_ERROR_INSUFFICIENT_RESOURCES:
		return "Insufficient Resources";
	case BT_ERROR_CCC_IMPROPERLY_CONFIGURED:
		return "CCC Improperly Configured";
	case BT_ERROR_ALREADY_IN_PROGRESS:
		return "Procedure Already in Progress";
	case BT_ERROR_OUT_OF_RANGE:
		return "Out of Range";
	default:
		return "Unknown error type";
	}
}

static void client_destroy()
{
	if (cli) {
		bt_gatt_client_unref(cli->gatt);
		bt_att_unref(cli->att);
		free(cli);
		cli = NULL;
	}
}

static void att_disconnect_cb(int err, void *user_data)
{
	print("Device disconnected: %s", strerror(err));

	client_destroy();
	update_prompt();
}

static void att_debug_cb(const char *str, void *user_data)
{
	const char *prefix = user_data;

	print(COLOR_BOLDGRAY "%s" COLOR_BOLDWHITE "%s" COLOR_OFF, prefix, str);
}

static void gatt_debug_cb(const char *str, void *user_data)
{
	const char *prefix = user_data;

	print(COLOR_GREEN "%s%s" COLOR_OFF, prefix, str);
}

static void ready_cb(bool success, uint8_t att_ecode, void *user_data);
static void service_changed_cb(uint16_t start_handle, uint16_t end_handle,
							void *user_data);

static void log_service_event(struct gatt_db_attribute *attr, const char *str)
{
	char uuid_str[MAX_LEN_UUID_STR];
	bt_uuid_t uuid;
	uint16_t start, end;

	gatt_db_attribute_get_service_uuid(attr, &uuid);
	bt_uuid_to_string(&uuid, uuid_str, sizeof(uuid_str));

	gatt_db_attribute_get_service_handles(attr, &start, &end);

	print("%s - UUID: %s start: 0x%04x end: 0x%04x", str, uuid_str,
								start, end);
}

static void service_added_cb(struct gatt_db_attribute *attr, void *user_data)
{
	log_service_event(attr, "Service Added");
}

static void service_removed_cb(struct gatt_db_attribute *attr, void *user_data)
{
	log_service_event(attr, "Service Removed");
}

static struct client *client_create(int fd, uint16_t mtu)
{
	struct client *cli;

	cli = new0(struct client, 1);
	if (!cli) {
		error("Failed to allocate memory for client");
		return NULL;
	}

	cli->att = bt_att_new(fd, false);
	if (!cli->att) {
		error("Failed to initialze ATT transport layer");
		bt_att_unref(cli->att);
		free(cli);
		return NULL;
	}

	if (!bt_att_set_close_on_unref(cli->att, true)) {
		error("Failed to set up ATT transport layer");
		bt_att_unref(cli->att);
		free(cli);
		return NULL;
	}

	if (!bt_att_register_disconnect(cli->att, att_disconnect_cb, NULL,
								NULL)) {
		error("Failed to set ATT disconnect handler");
		bt_att_unref(cli->att);
		free(cli);
		return NULL;
	}

	cli->fd = fd;
	cli->db = gatt_db_new();
	if (!cli->db) {
		error("Failed to create GATT database");
		bt_att_unref(cli->att);
		free(cli);
		return NULL;
	}

	cli->gatt = bt_gatt_client_new(cli->db, cli->att, mtu, 0);
	if (!cli->gatt) {
		error("Failed to create GATT client");
		gatt_db_unref(cli->db);
		bt_att_unref(cli->att);
		free(cli);
		return NULL;
	}

	gatt_db_register(cli->db, service_added_cb, service_removed_cb,
								NULL, NULL);

	if (verbose) {
		bt_att_set_debug(cli->att, BT_ATT_DEBUG_VERBOSE, att_debug_cb,
								"att: ", NULL);
		bt_gatt_client_set_debug(cli->gatt, gatt_debug_cb, "gatt: ",
									NULL);
	}

	bt_gatt_client_ready_register(cli->gatt, ready_cb, NULL, NULL);
	bt_gatt_client_set_service_changed(cli->gatt, service_changed_cb, NULL,
									NULL);

	/* bt_gatt_client already holds a reference */
	gatt_db_unref(cli->db);

	return cli;
}

static void append_uuid(char *str, const bt_uuid_t *uuid)
{
	char uuid_str[MAX_LEN_UUID_STR];
	bt_uuid_t uuid128;

	bt_uuid_to_uuid128(uuid, &uuid128);
	bt_uuid_to_string(&uuid128, uuid_str, sizeof(uuid_str));

	append(str, "%s", uuid_str);
}

static void print_incl(struct gatt_db_attribute *attr, void *user_data)
{
	uint16_t handle, start, end;
	struct gatt_db_attribute *service;
	bt_uuid_t uuid;
	char line[MAX_LEN_LINE] = {0};

	if (!gatt_db_attribute_get_incl_data(attr, &handle, &start, &end))
		return;

	service = gatt_db_get_attribute(cli->db, start);
	if (!service)
		return;

	gatt_db_attribute_get_service_uuid(service, &uuid);

	append(line, "\t  " COLOR_GREEN "include" COLOR_OFF " - handle: "
					"0x%04x, - start: 0x%04x, end: 0x%04x,"
					"uuid: ", handle, start, end);
	append_uuid(line, &uuid);
	print("%s", line);
}

static void print_desc(struct gatt_db_attribute *attr, void *user_data)
{
	char line[MAX_LEN_LINE] = {0};
	append(line, "\t\t  " COLOR_MAGENTA "descr" COLOR_OFF
					" - handle: 0x%04x, uuid: ",
					gatt_db_attribute_get_handle(attr));
	append_uuid(line, gatt_db_attribute_get_type(attr));
	print("%s", line);
}

static void print_chrc(struct gatt_db_attribute *attr, void *user_data)
{
	uint16_t handle, value_handle;
	uint8_t properties;
	uint16_t ext_prop;
	bt_uuid_t uuid;
	char line[MAX_LEN_LINE] = {0};

	if (!gatt_db_attribute_get_char_data(attr, &handle,
								&value_handle,
								&properties,
								&ext_prop,
								&uuid))
		return;

	append(line, "\t  " COLOR_YELLOW "charac" COLOR_OFF
				" - start: 0x%04x, value: 0x%04x, "
				"props: 0x%02x, ext_props: 0x%04x, uuid: ",
				handle, value_handle, properties, ext_prop);
	append_uuid(line, &uuid);
	print("%s", line);

	gatt_db_service_foreach_desc(attr, print_desc, NULL);
}

static void print_service(struct gatt_db_attribute *attr, void *user_data)
{
	uint16_t start, end;
	bool primary;
	bt_uuid_t uuid;
	char line[MAX_LEN_LINE] = {0};

	if (!gatt_db_attribute_get_service_data(attr, &start, &end, &primary,
									&uuid))
		return;

	append(line, COLOR_RED "service" COLOR_OFF " - start: 0x%04x, "
				"end: 0x%04x, type: %s, uuid: ",
				start, end, primary ? "primary" : "secondary");
	append_uuid(line, &uuid);
	print("%s", line);

	gatt_db_service_foreach_incl(attr, print_incl, NULL);
	gatt_db_service_foreach_char(attr, print_chrc, NULL);
}

static void print_services(struct client *cli)
{
	gatt_db_foreach_service(cli->db, NULL, print_service, NULL);
}

static void print_services_by_uuid(const bt_uuid_t *uuid)
{
	gatt_db_foreach_service(cli->db, uuid, print_service, NULL);
}

static void print_services_by_handle(uint16_t handle)
{
	uint16_t start = 0x0001, end = 0xFFFF;
	if (handle) {
		start = handle;
		end = handle;
	}
	gatt_db_foreach_service_in_range(cli->db, NULL, print_service, NULL,
			start, end);
}

static void ready_cb(bool success, uint8_t att_ecode, void *user_data)
{
	if (!success) {
		error("GATT discovery procedures failed - error code: 0x%02x",
								att_ecode);
		return;
	}

	print("GATT discovery procedures complete");

	print_services(cli);
}

static void service_changed_cb(uint16_t start_handle, uint16_t end_handle,
								void *user_data)
{
	print("Service Changed handled - start: 0x%04x end: 0x%04x",
						start_handle, end_handle);

	gatt_db_foreach_service_in_range(cli->db, NULL, print_service, NULL,
						start_handle, end_handle);
}

static struct option services_options[] = {
	{ "uuid",	1, 0, 'u' },
	{ "handle",	1, 0, 'a' },
	{ "help",	0, 0, 'h' },
	{ 0, 0, 0, 0 }
};

static void cmd_services(int argc, char **argv)
{
	int opt;
	bool use_uuid = false;
	bt_uuid_t tmp, uuid;
	uint16_t handle = 0;
	char *endptr = NULL;

	if (!bt_gatt_client_is_ready(cli->gatt)) {
		print("GATT client not initialized");
		return;
	}

	while ((opt = getopt_long(argc, argv, "u:a:", services_options,
								NULL)) != -1) {
		switch (opt) {
		case 'u':
			if (bt_string_to_uuid(&tmp, optarg) < 0) {
				error("Invalid UUID: %s", optarg);
				optind = 0;
				return bt_shell_noninteractive_quit(EXIT_FAILURE);
			}
			bt_uuid_to_uuid128(&tmp, &uuid);
			use_uuid = true;
			break;
		case 'a':
			handle = strtol(optarg, &endptr, 0);
			if (!endptr || *endptr != '\0') {
				error("Invalid start handle: %s", optarg);
				optind = 0;
				return bt_shell_noninteractive_quit(EXIT_FAILURE);
			}
			break;
		case 'h':
			bt_shell_usage();
			optind = 0;
			return bt_shell_noninteractive_quit(EXIT_SUCCESS);
		default:
			bt_shell_usage();
			optind = 0;
			return bt_shell_noninteractive_quit(EXIT_FAILURE);
		}
	}

	optind = 0;

	if (use_uuid)
		print_services_by_uuid(&uuid);
	else
		print_services_by_handle(handle);
}

static void read_multiple_cb(bool success, uint8_t att_ecode,
					const uint8_t *value, uint16_t length,
					void *user_data)
{
	int i;
	char line[MAX_LEN_LINE] = {0};

	if (!success) {
		error("Read multiple request failed: 0x%02x", att_ecode);
		return;
	}

	append(line, "Read multiple value (%u bytes):", length);

	for (i = 0; i < length; i++)
		append(line, "%02x ", value[i]);

	print("%s", line);
}

static void cmd_read_multiple(int argc, char **argv)
{
	uint16_t *value;
	int i;
	char *endptr = NULL;

	value = malloc(sizeof(uint16_t) * argc);
	if (!value) {
		error("Failed to construct value");
		return;
	}

	for (i = 1; i < argc; i++) {
		value[i] = strtol(argv[i], &endptr, 0);
		if (endptr == argv[i] || *endptr != '\0' || !value[i]) {
			error("Invalid value byte: %s", argv[i]);
			free(value);
			return;
		}
	}

	if (!bt_gatt_client_read_multiple(cli->gatt, value, argc,
						read_multiple_cb, NULL, NULL))
		error("Failed to initiate read multiple procedure");

	free(value);
}

void read_by_type_cb(bool success, uint8_t att_ecode,
						struct bt_gatt_result *result,
						void *user_data)
{
	const uint8_t *value;
	uint16_t length, handle;
	struct bt_gatt_iter iter;
	char line[MAX_LEN_LINE];
	int i;

	if (!success) {
		error("Read by type request failed: %s (0x%02x)",
				ecode_to_string(att_ecode), att_ecode);
		return;
	}

	bt_gatt_iter_init(&iter, result);
	while (bt_gatt_iter_next_read_by_type(&iter, &handle, &length, &value)) {
		line[0] = '\0';
		append(line, "\tValue handle 0x%04x", handle);

		if (length == 0) {
			print("%s: 0 bytes", line);
			return;
		}

		append(line, " (%u bytes): ", length);

		for (i = 0; i < length; i++)
			append(line, "%02x ", value[i]);

		print("%s", line);
	}
}

static void cmd_read_by_type(int argc, char **argv)
{
	bt_uuid_t uuid;
	uint16_t start_handle = 0x0001, end_handle = 0xFFFF;
	char *endptr = NULL;

	if (bt_string_to_uuid(&uuid, argv[1]) < 0) {
		error("Invalid UUID: %s", optarg);
		return;
	}
	if (argc > 2) {
		start_handle = strtol(argv[2], &endptr, 0);
		if (!endptr || *endptr != '\0' || !start_handle) {
			error("Invalid start_handle : %s", argv[1]);
			return;
		}
	}
	if (argc > 3) {
		end_handle = strtol(argv[3], &endptr, 0);
		if (!endptr || *endptr != '\0' || !end_handle) {
			error("Invalid end_handle : %s", argv[1]);
			return;
		}
	}
	if (start_handle > end_handle) {
		error("start_handle cannot by larger than end_handle");
		return;
	}

	if (!bt_gatt_read_by_type(cli->att, start_handle, end_handle,
				&uuid, read_by_type_cb, NULL, NULL))
		error("Failed to initiate read value procedure");
}

static void read_cb(bool success, uint8_t att_ecode, const uint8_t *value,
					uint16_t length, void *user_data)
{
	int i;
	char line[MAX_LEN_LINE] = {0};

	if (!success) {
		error("Read request failed: %s (0x%02x)",
				ecode_to_string(att_ecode), att_ecode);
		return;
	}

	append(line, "Read value");

	if (length == 0) {
		print("%s: 0 bytes", line);
		return;
	}

	append(line, " (%u bytes): ", length);

	for (i = 0; i < length; i++)
		append(line, "%02x ", value[i]);

	print("%s", line);
}

static void cmd_read_value(int argc, char **argv)
{
	uint16_t handle;
	char *endptr = NULL;

	handle = strtol(argv[1], &endptr, 0);
	if (!endptr || *endptr != '\0' || !handle) {
		error("Invalid value handle: %s", argv[1]);
		return;
	}

	if (!bt_gatt_client_read_value(cli->gatt, handle, read_cb,
								NULL, NULL))
		error("Failed to initiate read value procedure");
}

static void cmd_read_long_value(int argc, char **argv)
{
	uint16_t handle;
	uint16_t offset;
	char *endptr = NULL;

	handle = strtol(argv[1], &endptr, 0);
	if (!endptr || *endptr != '\0' || !handle) {
		error("Invalid value handle: %s", argv[1]);
		return;
	}

	endptr = NULL;
	offset = strtol(argv[2], &endptr, 0);
	if (!endptr || *endptr != '\0') {
		error("Invalid offset: %s", argv[2]);
		return;
	}

	if (!bt_gatt_client_read_long_value(cli->gatt, handle, offset, read_cb,
								NULL, NULL))
		error("Failed to initiate read long value procedure");
}

static struct option write_value_options[] = {
	{ "without-response",	0, 0, 'w' },
	{ "signed-write",	0, 0, 's' },
	{ "help",	0, 0, 'h' },
	{ }
};

static void write_cb(bool success, uint8_t att_ecode, void *user_data)
{
	if (success) {
		print("Write successful");
	} else {
		error("Write failed: %s (0x%02x)",
				ecode_to_string(att_ecode), att_ecode);
	}
}

static uint8_t *read_bytes(char **argv, int *length)
{
	int i, byte;
	uint8_t *value;
	char *endptr = NULL;
	bool use_bytes = false;

	if (*length == 3 && !strcmp(argv[i], "bytes")) {
		byte = strtol(argv[i+1], &endptr, 0);
		if (endptr == argv[i+1] || *endptr != '\0'
			|| errno == ERANGE || byte < 0 || byte > 255) {
			error("Invalid bytes value: %s", argv[i+1]);
			return NULL;
		}
		*length = strtol(argv[i+2], &endptr, 0);
		if (endptr == argv[i+2] || *endptr != '\0'
			|| errno == ERANGE) {
			error("Invalid bytes count: %s", argv[i+2]);
			return NULL;
		}
		use_bytes = true;
	}

	if (*length <= 0) {
		error("Nothing to write");
		return NULL;
	}
	if (*length > BT_ATT_MAX_VALUE_LEN) {
		error("Write value too long");
		return NULL;
	}

	value = malloc(*length);
	if (!value) {
		error("Failed to construct write value");
		return NULL;
	}

	if (use_bytes) {
		memset(value, byte, *length);
		return value;
	}

	for (i = 0; i < *length; i++) {
		byte = strtol(argv[i], &endptr, 0);
		if (endptr == argv[i] || *endptr != '\0'
			|| errno == ERANGE || byte < 0 || byte > 255) {
			error("Invalid value byte: %s", argv[i]);
			free(value);
			return NULL;
		}
		value[i] = byte;
	}
	return value;
}

static void cmd_write_value(int argc, char **argv)
{
	int opt, i, val;
	uint16_t handle;
	char *endptr = NULL;
	int length;
	uint8_t *value = NULL;
	bool without_response = false;
	bool signed_write = false;

	while ((opt = getopt_long(argc, argv, "+ws", write_value_options,
								NULL)) != -1) {
		switch (opt) {
		case 'w':
			without_response = true;
			break;
		case 's':
			signed_write = true;
			break;
		case 'h':
			bt_shell_usage();
			optind = 0;
			return bt_shell_noninteractive_quit(EXIT_SUCCESS);
		default:
			bt_shell_usage();
			optind = 0;
			return bt_shell_noninteractive_quit(EXIT_FAILURE);
		}
	}

	argc -= optind;
	argv += optind;
	optind = 0;

	handle = strtol(argv[0], &endptr, 0);
	if (!endptr || *endptr != '\0' || !handle) {
		error("Invalid handle: %s", argv[1]);
		return;
	}

	length = argc - 1;
	value = read_bytes(argv + 1, &length);
	if (!value)
		return;

	if (without_response) {
		if (!bt_gatt_client_write_without_response(cli->gatt, handle,
						signed_write, value, length)) {
			error("Failed to initiate write without response "
								"procedure");
			goto done;
		}

		print("Write command sent");
		goto done;
	}

	if (!bt_gatt_client_write_value(cli->gatt, handle, value, length,
								write_cb,
								NULL, NULL))
		error("Failed to initiate write procedure");

done:
	free(value);
}

static struct option write_long_value_options[] = {
	{ "reliable-write",	0, 0, 'r' },
	{ "help", 0, 0, 'h' },
	{ }
};

static void write_long_cb(bool success, bool reliable_error, uint8_t att_ecode,
								void *user_data)
{
	if (success) {
		print("Write successful");
	} else if (reliable_error) {
		error("Reliable write not verified");
	} else {
		error("Write failed: %s (0x%02x)",
				ecode_to_string(att_ecode), att_ecode);
	}
}

static void cmd_write_long_value(int argc, char **argv)
{
	int opt, i, val;
	uint16_t handle;
	uint16_t offset;
	char *endptr = NULL;
	int length;
	uint8_t *value = NULL;
	bool reliable_writes = false;

	while ((opt = getopt_long(argc, argv, "+r", write_long_value_options,
								NULL)) != -1) {
		switch (opt) {
		case 'r':
			reliable_writes = true;
			break;
		case 'h':
			bt_shell_usage();
			optind = 0;
			return bt_shell_noninteractive_quit(EXIT_SUCCESS);
		default:
			bt_shell_usage();
			optind = 0;
			return bt_shell_noninteractive_quit(EXIT_FAILURE);
		}
	}

	argc -= optind;
	argv += optind;
	optind = 0;

	handle = strtol(argv[0], &endptr, 0);
	if (!endptr || *endptr != '\0' || !handle) {
		error("Invalid handle: %s", argv[1]);
		return;
	}

	endptr = NULL;
	offset = strtol(argv[1], &endptr, 0);
	if (!endptr || *endptr != '\0' || errno == ERANGE) {
		error("Invalid offset: %s", argv[2]);
		return;
	}

	length = argc - 2;
	value = read_bytes(argv + 2, &length);
	if (!value)
		return;

	if (!bt_gatt_client_write_long_value(cli->gatt, reliable_writes, handle,
							offset, value, length,
							write_long_cb,
							NULL, NULL))
		error("Failed to initiate long write procedure");

	free(value);
}

static struct option write_prepare_options[] = {
	{ "session-id",		1, 0, 's' },
	{ "help", 0, 0, 'h' },
	{ }
};

static void cmd_write_prepare(int argc, char **argv)
{
	int opt, i, val;
	unsigned int id = 0;
	uint16_t handle;
	uint16_t offset;
	char *endptr = NULL;
	unsigned int length;
	uint8_t *value = NULL;

	while ((opt = getopt_long(argc, argv , "s:", write_prepare_options,
								NULL)) != -1) {
		switch (opt) {
		case 's':
			id = atoi(optarg);
			break;
		case 'h':
			bt_shell_usage();
			optind = 0;
			return bt_shell_noninteractive_quit(EXIT_SUCCESS);
		default:
			bt_shell_usage();
			optind = 0;
			return bt_shell_noninteractive_quit(EXIT_FAILURE);
		}
	}

	argc -= optind;
	argv += optind;
	optind = 0;

	if (cli->reliable_session_id != id) {
		error("Session id != Ongoing session id (%u!=%u)", id,
						cli->reliable_session_id);
		return;
	}

	handle = strtol(argv[0], &endptr, 0);
	if (!endptr || *endptr != '\0' || !handle) {
		error("Invalid handle: %s", argv[1]);
		return;
	}

	endptr = NULL;
	offset = strtol(argv[1], &endptr, 0);
	if (!endptr || *endptr != '\0' || errno == ERANGE) {
		error("Invalid offset: %s", argv[2]);
		return;
	}

	/*
	 * First two arguments are handle and offset. What remains is the value
	 * length
	 */
	length = argc - 2;
	value = read_bytes(argv + 2, &length);
	if (!value)
		return;

	cli->reliable_session_id =
			bt_gatt_client_prepare_write(cli->gatt, id,
							handle, offset,
							value, length,
							write_long_cb, NULL,
							NULL);
	if (!cli->reliable_session_id)
		error("Failed to proceed prepare write");
	else
		print("Prepare write success."
				"Session id: %d to be used on next write",
						cli->reliable_session_id);

	free(value);
}

static void cmd_write_execute(int argc, char **argv)
{
	char *endptr = NULL;
	unsigned int session_id;
	bool execute;

	session_id = strtol(argv[1], &endptr, 0);
	if (!endptr || *endptr != '\0') {
		error("Invalid session id: %s", argv[1]);
		return;
	}

	if (session_id != cli->reliable_session_id) {
		error("Invalid session id: %u != %u", session_id,
						cli->reliable_session_id);
		return;
	}

	execute = !!strtol(argv[2], &endptr, 0);
	if (!endptr || *endptr != '\0') {
		error("Invalid execute: %s", argv[2]);
		return;
	}

	if (execute) {
		if (!bt_gatt_client_write_execute(cli->gatt, session_id,
							write_cb, NULL, NULL))
			error("Failed to proceed write execute");
	} else {
		bt_gatt_client_cancel(cli->gatt, session_id);
	}

	cli->reliable_session_id = 0;
}

static void notify_cb(uint16_t value_handle, const uint8_t *value,
					uint16_t length, void *user_data)
{
	int i;
	char line[MAX_LEN_LINE] = {0};

	append(line, "\tHandle Value Not/Ind: 0x%04x - ", value_handle);

	if (length == 0) {
		print("%s(0 bytes)", line);
		return;
	}

	append(line, "(%u bytes): ", length);

	for (i = 0; i < length; i++)
		append(line, "%02x ", value[i]);

	print("%s", line);
}

static void register_notify_cb(uint16_t att_ecode, void *user_data)
{
	if (att_ecode) {
		error("Failed to register notify handler "
					"- error code: 0x%02x", att_ecode);
		return;
	}

	print("Registered notify handler!");
}

static void cmd_register_notify(int argc, char **argv)
{
	uint16_t value_handle;
	unsigned int id;
	char *endptr = NULL;

	if (!bt_gatt_client_is_ready(cli->gatt)) {
		printf("GATT client not initialized\n");
		return;
	}

	value_handle = strtol(argv[1], &endptr, 0);
	if (!endptr || *endptr != '\0' || !value_handle) {
		error("Invalid value handle: %s", argv[1]);
		return;
	}

	id = bt_gatt_client_register_notify(cli->gatt, value_handle,
							register_notify_cb,
							notify_cb, NULL, NULL);
	if (!id) {
		error("Failed to register notify handler");
		return;
	}

	print("Registering notify handler with id: %u", id);
}

static void cmd_unregister_notify(int argc, char **argv)
{
	unsigned int id;
	char *endptr = NULL;

	if (!bt_gatt_client_is_ready(cli->gatt)) {
		printf("GATT client not initialized\n");
		return;
	}

	id = strtol(argv[1], &endptr, 0);
	if (!endptr || *endptr != '\0' || !id) {
		error("Invalid notify id: %s", argv[1]);
		return;
	}

	if (!bt_gatt_client_unregister_notify(cli->gatt, id)) {
		error("Failed to unregister notify handler with id: %u", id);
		return;
	}

	print("Unregistered notify handler with id: %u", id);
}

static void cmd_set_security(int argc, char **argv)
{
	char *endptr = NULL;
	int level;

	level = strtol(argv[1], &endptr, 0);
	if (!endptr || *endptr != '\0' || level < 1 || level > 3) {
		error("Invalid level: %s", argv[1]);
		return;
	}

	if (!bt_gatt_client_set_security(cli->gatt, level))
		error("Could not set sec level");
	else
		print("Setting security level %d success", level);
}

static void cmd_get_security(int argc, char **argv)
{
	int level;

	level = bt_gatt_client_get_security(cli->gatt);
	if (level < 0)
		error("Could not get sec level");
	else
		print("Security level: %u", level);
}

static bool convert_sign_key(char *optarg, uint8_t key[16])
{
	int i;

	if (strlen(optarg) != 32) {
		error("sign-key length is invalid");
		return false;
	}

	for (i = 0; i < 16; i++) {
		if (sscanf(optarg + (i * 2), "%2hhx", &key[i]) != 1)
			return false;
	}

	return true;
}

static bool local_counter(uint32_t *sign_cnt, void *user_data)
{
	static uint32_t cnt = 0;

	*sign_cnt = cnt++;

	return true;
}

static void cmd_set_sign_key(int argc, char **argv)
{
	uint8_t key[16];

	memset(key, 0, 16);

	if (!strcmp(argv[1], "-c") || !strcmp(argv[1], "--sign-key")) {
		if (convert_sign_key(argv[2], key))
			bt_att_set_local_key(cli->att, key, local_counter, cli);
	} else {
		bt_shell_usage();
		optind = 0;
	}
}

static void connect_device()
{
	int fd;
	fd = l2cap_att_connect(&src_addr, &dst_addr, dst_type, security_level);
	if (fd < 0) {
		return bt_shell_noninteractive_quit(EXIT_FAILURE);
	}
	cli = client_create(fd, mtu);
	if (!cli) {
		close(fd);
		return bt_shell_noninteractive_quit(EXIT_FAILURE);
	}
}

static void cmd_connect(int argc, char **argv)
{
	char addr[18];

	if (cli) {
		error("Already connected");
		return bt_shell_noninteractive_quit(EXIT_FAILURE);
	}
	if (argc > 1) {
		if (str2ba(argv[1], &addr) < 0) {
			error("Invalid remote address: %s", argv[1]);
			return bt_shell_noninteractive_quit(EXIT_FAILURE);
		}
		bacpy(&dst_addr, &addr);
	}
	if (argc > 2) {
		if (strcmp(argv[2], "random") == 0)
			dst_type = BDADDR_LE_RANDOM;
		else if (strcmp(argv[2], "public") == 0)
			dst_type = BDADDR_LE_PUBLIC;
		else if (strcmp(argv[2], "bredr") == 0)
			dst_type = BDADDR_BREDR;
		else {
			error("Allowed types: random, public, bredr");
			return bt_shell_noninteractive_quit(EXIT_FAILURE);
		}
	}
	if (!bacmp(&dst_addr, BDADDR_ANY)) {
		error("Destination address required!");
		return bt_shell_noninteractive_quit(EXIT_FAILURE);
	}
	connect_device();
	update_prompt();
}

static void cmd_disconnect(int argc, char **argv)
{
	if (!cli) {
		error("Already disconnected");
		return bt_shell_noninteractive_quit(EXIT_FAILURE);
	}
	close(cli->fd);
	client_destroy();
	update_prompt();
}

static const struct bt_shell_menu main_menu = {
	.name = "main",
	.entries = {
	{ "services", "[options...]", cmd_services,
		"Show discovered services\n"
		"Options:\n"
			"\t -u, --uuid <uuid>\tService UUID\n"
			"\t -a, --handle <handle>\tService start handle\n"
		"e.g.:\n"
			"\tservices\n\tservices -u 0x180d\n\tservices -a 0x0009"
	},
	{ "read-value", "<value_handle>",
		cmd_read_value, "Read a characteristic or descriptor value" },
	{ "read-long-value", "<value_handle> <offset>",
		cmd_read_long_value, "Read a long characteristic or desctriptor value" },
	{ "read-multiple", "<handles...>",
		cmd_read_multiple, "Read Multiple" },
	{ "read-by-type", "<uuid> [start_handle] [end_handle]",
		cmd_read_by_type, "Read a value by UUID" },
	{ "write-value", " [-w|-s] <value_handle> <value...>",
		cmd_write_value, "Write a characteristic or descriptor value\n"
		"Options:\n"
			"\t-w, --without-response\tWrite without response\n"
			"\t-s, --signed-write\tSigned write command\n"
			"\tbytes <value> <count>\tWrite specified number of bytes with value\n"
		"e.g.:\n"
			"\twrite-value 0x0001 00 01 00"
			"\twrite-value 0x0001 bytes 0 100"
	},
	{ "write-long-value", "[-r] <value_handle> <offset>",
		cmd_write_long_value, "Write long characteristic or descriptor value\n"
		"Options:\n"
			"\t-r, --reliable-write\tReliable write\n"
			"\tbytes <value> <count>\tWrite specified number of bytes with value\n"
		"e.g.:\n"
			"\twrite-long-value 0x0001 0 00 01 00"
			"\twrite-long-value 0x0001 0 bytes 0 100"
	},
	{ "write-prepare", " [options...] <value_handle> <value>",
		cmd_write_prepare, "Write prepare characteristic or descriptor value\n"
		"Options:\n"
			"\t-s, --session-id\tSession id\n"
			"\tbytes <value> <count>\tWrite specified number of bytes with value\n"
		"e.g.:\n"
			"\twrite-prepare -s 1 0x0001 00 01 00"
			"\twrite-prepare -s 1 0x0001 bytes 0 100"
	},
	{ "write-execute", " <session_id> <execute>",
		cmd_write_execute, "Execute already prepared write" },
	{ "register-notify", "<chrc_value_handle>",
		cmd_register_notify, "Subscribe to not/ind from a characteristic" },
	{ "unregister-notify", "<notify_id>",
		cmd_unregister_notify, "Unregister a not/ind session"},
	{ "set-security", "<level 1-3>",
		cmd_set_security, "Set security level on connection"},
	{ "get-security", NULL,
		cmd_get_security, "Get security level on connection"},
	{ "set-sign-key", "<csrk>",
		cmd_set_sign_key, "Set signing key for signed write command"},
	{ "connect", "[address] [public|random|bredr]",
		cmd_connect, "Connect to device" },
	{ "disconnect", NULL,
		cmd_disconnect, "Disconnect from connected device" },
	{} },
};

static int l2cap_att_connect(bdaddr_t *src, bdaddr_t *dst, uint8_t dst_type,
									int sec)
{
	int sock;
	struct sockaddr_l2 srcaddr, dstaddr;
	struct bt_security btsec;

	if (verbose) {
		char srcaddr_str[18], dstaddr_str[18];

		ba2str(src, srcaddr_str);
		ba2str(dst, dstaddr_str);

		print("btgatt-client: Opening L2CAP %s connection on ATT "
					"channel:\n\t src: %s\n\tdest: %s",
					(dst_type == BDADDR_BREDR ? "BR/EDR" : "LE"),
					srcaddr_str, dstaddr_str);
	}

	sock = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
	if (sock < 0) {
		error("Failed to create L2CAP socket");
		return -1;
	}

	/* Set up source address */
	memset(&srcaddr, 0, sizeof(srcaddr));
	srcaddr.l2_family = AF_BLUETOOTH;
	if (dst_type == BDADDR_BREDR)
		srcaddr.l2_psm = htobs(ATT_PSM);
	else
		srcaddr.l2_cid = htobs(ATT_CID);
	srcaddr.l2_bdaddr_type = 0;
	bacpy(&srcaddr.l2_bdaddr, src);

	if (bind(sock, (struct sockaddr *)&srcaddr, sizeof(srcaddr)) < 0) {
		error("Failed to bind L2CAP socket");
		close(sock);
		return -1;
	}

	/* Set the security level */
	memset(&btsec, 0, sizeof(btsec));
	btsec.level = sec;
	if (setsockopt(sock, SOL_BLUETOOTH, BT_SECURITY, &btsec,
							sizeof(btsec)) != 0) {
		error("Failed to set L2CAP security level");
		close(sock);
		return -1;
	}

	/* Set up destination address */
	memset(&dstaddr, 0, sizeof(dstaddr));
	dstaddr.l2_family = AF_BLUETOOTH;
	if (dst_type == BDADDR_BREDR)
		dstaddr.l2_psm = htobs(ATT_PSM);
	else
		dstaddr.l2_cid = htobs(ATT_CID);
	dstaddr.l2_bdaddr_type = dst_type;
	bacpy(&dstaddr.l2_bdaddr, dst);

	print("Connecting to device...");
	fflush(stdout);

	if (connect(sock, (struct sockaddr *) &dstaddr, sizeof(dstaddr)) < 0) {
		error("Failed to connect");
		close(sock);
		return -1;
	}

	print("Done");

	return sock;
}

static struct option main_options[] = {
	{ "index",     required_argument, NULL, 'i' },
	{ "dst-addr",  required_argument, NULL, 'd' },
	{ "type",      required_argument, NULL, 'T' },
	{ "mtu",       required_argument, NULL, 'M' },
	{ "sec-level", required_argument, NULL, 's' },
	{ "verbose",   no_argument,       NULL, 'V' },
	{ }
};

static const char *index_option;
static const char *dst_addr_option;
static const char *type_option;
static const char *mtu_option;
static const char *security_level_option;
static const char *verbose_option;

static const char **optargs[] = {
	&index_option,
	&dst_addr_option,
	&type_option,
	&mtu_option,
	&security_level_option,
	&verbose_option,
};

static const char *help[] = {
	"Specify adapter index, e.g. hci0",
	"Specify the destination address",
	"Specify the address type (random|public|bredr)",
	"The ATT MTU to use",
	"Set security level (low|medium|high|fips)",
	"Enable extra logging"
};

static const struct bt_shell_opt opt = {
	.options = main_options,
	.optno = sizeof(main_options) / sizeof(struct option),
	.optstr = "i:d:T:M:s:V",
	.optarg = optargs,
	.help = help,
};

int main(int argc, char *argv[])
{
	int dev_id = -1;
	int status;

	bt_shell_init(argc, argv, &opt);
	bt_shell_set_menu(&main_menu);

	if (verbose_option)
		verbose = true;
	if (security_level_option) {
		if (strcmp(security_level_option, "low") == 0)
			security_level = BT_SECURITY_LOW;
		else if (strcmp(security_level_option, "medium") == 0)
			security_level = BT_SECURITY_MEDIUM;
		else if (strcmp(security_level_option, "high") == 0)
			security_level = BT_SECURITY_HIGH;
		else if (strcmp(security_level_option, "fips") == 0)
			security_level = BT_SECURITY_FIPS;
		else {
			error("Invalid security level");
			return EXIT_FAILURE;
		}
	}
	if (mtu_option) {
		int arg;

		arg = atoi(mtu_option);
		if (arg <= 0) {
			error("Invalid MTU: %d", arg);
			return EXIT_FAILURE;
		}

		if (arg > UINT16_MAX) {
			error("MTU too large: %d", arg);
			return EXIT_FAILURE;
		}

		mtu = (uint16_t)arg;
	}
	if (type_option) {
		if (strcmp(type_option, "random") == 0)
			dst_type = BDADDR_LE_RANDOM;
		else if (strcmp(type_option, "public") == 0)
			dst_type = BDADDR_LE_PUBLIC;
		else if (strcmp(type_option, "bredr") == 0)
			dst_type = BDADDR_BREDR;
		else {
			error("Allowed types: random, public, bredr");
			return EXIT_FAILURE;
		}
	}
	if (dst_addr_option) {
		if (str2ba(dst_addr_option, &dst_addr) < 0) {
			error("Invalid remote address: %s", dst_addr_option);
			return EXIT_FAILURE;
		}
	} else {
		bacpy(&dst_addr, BDADDR_ANY);
	}
	if (index_option) {
		dev_id = hci_devid(index_option);
		if (dev_id < 0) {
			error("Invalid adapter");
			return EXIT_FAILURE;
		}
	}

	if (dev_id == -1)
		bacpy(&src_addr, BDADDR_ANY);
	else if (hci_devba(dev_id, &src_addr) < 0) {
		error("Adapter not available");
		return EXIT_FAILURE;
	}

	if (bacmp(&dst_addr, BDADDR_ANY))
		connect_device();

	bt_shell_attach(fileno(stdin));
	update_prompt();
	shell_running = true;
	status = bt_shell_run();
	shell_running = false;

	client_destroy();

	return status;
}
