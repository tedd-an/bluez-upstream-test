// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2011-2014  Intel Corporation
 *  Copyright (C) 2002-2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "lib/bluetooth.h"

#include "src/shared/util.h"
#include "src/shared/queue.h"
#include "src/shared/btsnoop.h"
#include "monitor/bt.h"
#include "monitor/display.h"
#include "monitor/packet.h"
#include "monitor/analyze.h"

#define TIMEVAL_MSEC(_tv) \
	(long long)((_tv)->tv_sec * 1000 + (_tv)->tv_usec / 1000)

struct hci_dev {
	uint16_t index;
	uint8_t type;
	uint8_t bdaddr[6];
	struct timeval time_added;
	struct timeval time_removed;
	unsigned long num_hci;
	unsigned long num_cmd;
	unsigned long num_evt;
	unsigned long num_acl;
	unsigned long num_sco;
	unsigned long num_iso;
	unsigned long vendor_diag;
	unsigned long system_note;
	unsigned long user_log;
	unsigned long ctrl_msg;
	unsigned long unknown;
	uint16_t manufacturer;
	struct queue *conn_list;
};

#define CONN_BR_ACL	0x01
#define CONN_BR_SCO	0x02
#define CONN_BR_ESCO	0x03
#define CONN_LE_ACL	0x04
#define CONN_LE_ISO	0x05

struct hci_conn {
	uint16_t handle;
	uint8_t type;
	uint8_t bdaddr[6];
	bool setup_seen;
	bool terminated;
	unsigned long rx_num;
	unsigned long tx_num;
	unsigned long tx_num_comp;
	size_t tx_bytes;
	struct queue *tx_queue;
	struct packet_latency tx_l;
	struct queue *plot;
	uint16_t tx_pkt_min;
	uint16_t tx_pkt_max;
	uint16_t tx_pkt_med;
	struct queue *chan_list;
};

struct hci_conn_tx {
	struct timeval tv;
	struct l2cap_chan *chan;
};

struct plot {
	long long x_msec;
	size_t y_count;
};

struct l2cap_chan {
	uint16_t cid;
	uint16_t psm;
	bool out;
	unsigned long num;
	size_t tx_bytes;
	struct packet_latency tx_l;
	struct queue *plot;
	uint16_t tx_pkt_min;
	uint16_t tx_pkt_max;
};

static struct queue *dev_list;

static void tmp_write(void *data, void *user_data)
{
	struct plot *plot = data;
	FILE *tmp = user_data;

	fprintf(tmp, "%lld %zu\n", plot->x_msec, plot->y_count);
}

static void plot_draw(struct queue *queue)
{
	const char *filename = "analyze.tmp";
	FILE *gplot = popen("gnuplot", "w");
	FILE *tmp;

	if (!gplot)
		return;

	if (queue_isempty(queue))
		goto done;

	tmp = fopen(filename, "w");
	if (!tmp)
		goto done;

	queue_foreach(queue, tmp_write, tmp);

	fprintf(gplot, "set terminal dumb enhanced ansi\n");
	fprintf(gplot, "set xlabel 'Latency (ms)'\n");
	fprintf(gplot, "set tics out nomirror\n");
	fprintf(gplot, "set log y\n");
	fprintf(gplot, "set yrange [0.5:*]\n");
	fprintf(gplot, "plot './%s' using 1:2 t 'Packets' w impulses\n",
								filename);
	fflush(gplot);

	fclose(tmp);
done:
	pclose(gplot);
	unlink(filename);
}

static void chan_destroy(void *data)
{
	struct l2cap_chan *chan = data;

	printf("  Found %s L2CAP channel with CID %u\n",
					chan->out ? "TX" : "RX", chan->cid);
	if (chan->psm)
		print_field("PSM %u", chan->psm);

	print_field("%lu packets", chan->num);

	if (!queue_isempty(chan->plot)) {
		print_field("%lld-%lld msec (~%lld msec) TX Latency",
			TV_MSEC(chan->tx_l.min), TV_MSEC(chan->tx_l.max),
			TV_MSEC(chan->tx_l.med));
		print_field("%u-%u octets TX packet size",
				chan->tx_pkt_min, chan->tx_pkt_max);
	}

	if (TV_MSEC(chan->tx_l.total))
		print_field("~%lld Kb/s TX transfer speed",
				chan->tx_bytes * 8 / TV_MSEC(chan->tx_l.total));

	if (chan->num > 1)
		plot_draw(chan->plot);

	free(chan);
}

static struct l2cap_chan *chan_alloc(struct hci_conn *conn, uint16_t cid,
								bool out)
{
	struct l2cap_chan *chan;

	chan = new0(struct l2cap_chan, 1);

	chan->cid = cid;
	chan->out = out;
	chan->plot = queue_new();

	return chan;
}

static bool chan_match_cid(const void *a, const void *b)
{
	const struct l2cap_chan *chan = a;
	uint32_t val = PTR_TO_UINT(b);
	uint16_t cid = val & 0xffff;
	bool out = val & 0x10000;

	return chan->cid == cid && chan->out == out;
}

static struct l2cap_chan *chan_lookup(struct hci_conn *conn, uint16_t cid,
								bool out)
{
	struct l2cap_chan *chan;
	uint32_t val = cid | (out ? 0x10000 : 0);

	chan = queue_find(conn->chan_list, chan_match_cid, UINT_TO_PTR(val));
	if (!chan) {
		chan = chan_alloc(conn, cid, out);
		queue_push_tail(conn->chan_list, chan);
	}

	return chan;
}

static void conn_destroy(void *data)
{
	struct hci_conn *conn = data;
	const char *str;

	switch (conn->type) {
	case CONN_BR_ACL:
		str = "BR-ACL";
		break;
	case CONN_BR_SCO:
		str = "BR-SCO";
		break;
	case CONN_BR_ESCO:
		str = "BR-ESCO";
		break;
	case CONN_LE_ACL:
		str = "LE-ACL";
		break;
	case CONN_LE_ISO:
		str = "LE-ISO";
		break;
	default:
		str = "unknown";
		break;
	}

	if (conn->tx_num > 0)
		conn->tx_pkt_med = conn->tx_bytes / conn->tx_num;

	printf("  Found %s connection with handle %u\n", str, conn->handle);
	/* TODO: Store address type */
	packet_print_addr("Address", conn->bdaddr, 0x00);
	if (!conn->setup_seen)
		print_field("Connection setup missing");
	print_field("%lu RX packets", conn->rx_num);
	print_field("%lu TX packets", conn->tx_num);
	print_field("%lu TX completed packets", conn->tx_num_comp);
	print_field("%lld-%lld msec (~%lld msec) TX Latency",
			TV_MSEC(conn->tx_l.min), TV_MSEC(conn->tx_l.max),
			TV_MSEC(conn->tx_l.med));
	print_field("%u-%u octets (~%u octets) TX packet size",
			conn->tx_pkt_min, conn->tx_pkt_max, conn->tx_pkt_med);

	if (TV_MSEC(conn->tx_l.total))
		print_field("~%lld Kb/s TX transfer speed",
			conn->tx_bytes * 8 / TV_MSEC(conn->tx_l.total));

	plot_draw(conn->plot);

	queue_destroy(conn->plot, free);
	queue_destroy(conn->chan_list, chan_destroy);

	queue_destroy(conn->tx_queue, free);
	free(conn);
}

static struct hci_conn *conn_alloc(struct hci_dev *dev, uint16_t handle,
								uint8_t type)
{
	struct hci_conn *conn;

	conn = new0(struct hci_conn, 1);

	conn->handle = handle;
	conn->type = type;
	conn->tx_queue = queue_new();
	conn->plot = queue_new();

	conn->chan_list = queue_new();

	return conn;
}

static bool conn_match_handle(const void *a, const void *b)
{
	const struct hci_conn *conn = a;
	uint16_t handle = PTR_TO_UINT(b);

	return (conn->handle == handle && !conn->terminated);
}

static struct hci_conn *conn_lookup(struct hci_dev *dev, uint16_t handle)
{
	return queue_find(dev->conn_list, conn_match_handle,
						UINT_TO_PTR(handle));
}

static struct hci_conn *conn_lookup_type(struct hci_dev *dev, uint16_t handle,
								uint8_t type)
{
	struct hci_conn *conn;

	conn = queue_find(dev->conn_list, conn_match_handle,
						UINT_TO_PTR(handle));
	if (!conn || conn->type != type) {
		conn = conn_alloc(dev, handle, type);
		queue_push_tail(dev->conn_list, conn);
	}

	return conn;
}

static void dev_destroy(void *data)
{
	struct hci_dev *dev = data;
	const char *str;

	switch (dev->type) {
	case 0x00:
		str = "BR/EDR";
		break;
	case 0x01:
		str = "AMP";
		break;
	default:
		str = "unknown";
		break;
	}

	printf("Found %s controller with index %u\n", str, dev->index);
	printf("  BD_ADDR %2.2X:%2.2X:%2.2X:%2.2X:%2.2X:%2.2X",
			dev->bdaddr[5], dev->bdaddr[4], dev->bdaddr[3],
			dev->bdaddr[2], dev->bdaddr[1], dev->bdaddr[0]);
	if (dev->manufacturer != 0xffff)
		printf(" (%s)", bt_compidtostr(dev->manufacturer));
	printf("\n");


	printf("  %lu commands\n", dev->num_cmd);
	printf("  %lu events\n", dev->num_evt);
	printf("  %lu ACL packets\n", dev->num_acl);
	printf("  %lu SCO packets\n", dev->num_sco);
	printf("  %lu ISO packets\n", dev->num_iso);
	printf("  %lu vendor diagnostics\n", dev->vendor_diag);
	printf("  %lu system notes\n", dev->system_note);
	printf("  %lu user logs\n", dev->user_log);
	printf("  %lu control messages \n", dev->ctrl_msg);
	printf("  %lu unknown opcodes\n", dev->unknown);
	queue_destroy(dev->conn_list, conn_destroy);
	printf("\n");

	free(dev);
}

static struct hci_dev *dev_alloc(uint16_t index)
{
	struct hci_dev *dev;

	dev = new0(struct hci_dev, 1);

	dev->index = index;
	dev->manufacturer = 0xffff;

	dev->conn_list = queue_new();

	return dev;
}

static bool dev_match_index(const void *a, const void *b)
{
	const struct hci_dev *dev = a;
	uint16_t index = PTR_TO_UINT(b);

	return dev->index == index;
}

static struct hci_dev *dev_lookup(uint16_t index)
{
	struct hci_dev *dev;

	dev = queue_find(dev_list, dev_match_index, UINT_TO_PTR(index));
	if (!dev) {
		dev = dev_alloc(index);
		queue_push_tail(dev_list, dev);
	}

	return dev;
}

static void l2cap_sig(struct hci_conn *conn, bool out,
					const void *data, uint16_t size)
{
	const struct bt_l2cap_hdr_sig *hdr = data;
	struct l2cap_chan *chan;
	uint16_t psm, scid, dcid;

	switch (hdr->code) {
	case BT_L2CAP_PDU_CONN_REQ:
		psm = get_le16(data + 4);
		scid = get_le16(data + 6);
		chan = chan_lookup(conn, scid, out);
		if (chan)
			chan->psm = psm;
		break;
	case BT_L2CAP_PDU_CONN_RSP:
		dcid = get_le16(data + 4);
		scid = get_le16(data + 6);
		chan = chan_lookup(conn, scid, !out);
		if (chan) {
			psm = chan->psm;
			chan = chan_lookup(conn, dcid, out);
			if (chan)
				chan->psm = psm;
		}
		break;
	}
}

static void new_index(struct timeval *tv, uint16_t index,
					const void *data, uint16_t size)
{
	const struct btsnoop_opcode_new_index *ni = data;
	struct hci_dev *dev;

	dev = dev_alloc(index);

	dev->type = ni->type;
	memcpy(dev->bdaddr, ni->bdaddr, 6);

	queue_push_tail(dev_list, dev);
}

static void del_index(struct timeval *tv, uint16_t index,
					const void *data, uint16_t size)
{
	struct hci_dev *dev;

	dev = queue_remove_if(dev_list, dev_match_index, UINT_TO_PTR(index));
	if (!dev) {
		fprintf(stderr, "Remove for an unexisting device\n");
		return;
	}

	dev_destroy(dev);
}

static void command_pkt(struct timeval *tv, uint16_t index,
					const void *data, uint16_t size)
{
	struct hci_dev *dev;

	dev = dev_lookup(index);
	if (!dev)
		return;

	dev->num_hci++;
	dev->num_cmd++;
}

static void evt_conn_complete(struct hci_dev *dev, struct timeval *tv,
					const void *data, uint16_t size)
{
	const struct bt_hci_evt_conn_complete *evt = data;
	struct hci_conn *conn;

	if (evt->status)
		return;

	conn = conn_lookup_type(dev, le16_to_cpu(evt->handle), CONN_BR_ACL);
	if (!conn)
		return;

	memcpy(conn->bdaddr, evt->bdaddr, 6);
	conn->setup_seen = true;
}

static void evt_disconnect_complete(struct hci_dev *dev, struct timeval *tv,
					const void *data, uint16_t size)
{
	const struct bt_hci_evt_disconnect_complete *evt = data;
	struct hci_conn *conn;

	if (evt->status)
		return;

	conn = conn_lookup(dev, le16_to_cpu(evt->handle));
	if (!conn)
		return;

	conn->terminated = true;
}

static void rsp_read_bd_addr(struct hci_dev *dev, struct timeval *tv,
					const void *data, uint16_t size)
{
	const struct bt_hci_rsp_read_bd_addr *rsp = data;

	if (rsp->status)
		return;

	memcpy(dev->bdaddr, rsp->bdaddr, 6);
}

static void evt_cmd_complete(struct hci_dev *dev, struct timeval *tv,
					const void *data, uint16_t size)
{
	const struct bt_hci_evt_cmd_complete *evt = data;
	uint16_t opcode;

	data += sizeof(*evt);
	size -= sizeof(*evt);

	opcode = le16_to_cpu(evt->opcode);

	switch (opcode) {
	case BT_HCI_CMD_READ_BD_ADDR:
		rsp_read_bd_addr(dev, tv, data, size);
		break;
	}
}

static bool match_plot_latency(const void *data, const void *user_data)
{
	const struct plot *plot = data;
	const struct timeval *latency = user_data;

	return TIMEVAL_MSEC(latency) == plot->x_msec;
}

static void plot_add(struct queue *queue, struct timeval *latency,
						uint16_t count)
{
	struct plot *plot;

	/* Use LRU ordering */
	plot = queue_remove_if(queue, match_plot_latency, latency);
	if (plot) {
		plot->y_count += count;
		queue_push_head(queue, plot);
		return;
	}

	plot = new0(struct plot, 1);
	plot->x_msec = TIMEVAL_MSEC(latency);
	plot->y_count = count;

	queue_push_tail(queue, plot);
}

static void evt_num_completed_packets(struct hci_dev *dev, struct timeval *tv,
					const void *data, uint16_t size)
{
	uint8_t num_handles = get_u8(data);
	int i;

	data += sizeof(num_handles);
	size -= sizeof(num_handles);

	for (i = 0; i < num_handles; i++) {
		uint16_t handle = get_le16(data);
		uint16_t count = get_le16(data + 2);
		struct hci_conn *conn;
		struct timeval res;
		struct hci_conn_tx *last_tx;
		int j;

		data += 4;
		size -= 4;

		conn = conn_lookup(dev, handle);
		if (!conn)
			continue;

		conn->tx_num_comp += count;

		for (j = 0; j < count; j++) {
			last_tx = queue_pop_head(conn->tx_queue);
			if (last_tx) {
				struct l2cap_chan *chan = last_tx->chan;

				timersub(tv, &last_tx->tv, &res);

				packet_latency_add(&conn->tx_l, &res);
				plot_add(conn->plot, &res, 1);

				if (chan) {
					packet_latency_add(&chan->tx_l, &res);
					plot_add(chan->plot, &res, 1);
				}

				free(last_tx);
			}
		}
	}
}

static void evt_sync_conn_complete(struct hci_dev *dev, struct timeval *tv,
					const void *data, uint16_t size)
{
	const struct bt_hci_evt_sync_conn_complete *evt = data;
	struct hci_conn *conn;

	if (evt->status)
		return;

	conn = conn_lookup_type(dev, le16_to_cpu(evt->handle), evt->link_type);
	if (!conn)
		return;

	memcpy(conn->bdaddr, evt->bdaddr, 6);
	conn->setup_seen = true;
}

static void evt_le_cis_established(struct hci_dev *dev, struct timeval *tv,
					struct iovec *iov)
{
	const struct bt_hci_evt_le_cis_established *evt;
	struct hci_conn *conn;

	evt = util_iov_pull_mem(iov, sizeof(*evt));
	if (!evt || evt->status)
		return;

	conn = conn_lookup_type(dev, le16_to_cpu(evt->conn_handle),
						CONN_LE_ISO);
	if (!conn)
		return;

	conn->setup_seen = true;
}

static void evt_le_big_complete(struct hci_dev *dev, struct timeval *tv,
					struct iovec *iov)
{
	const struct bt_hci_evt_le_big_complete *evt;
	int i;

	evt = util_iov_pull_mem(iov, sizeof(*evt));
	if (!evt || evt->status)
		return;

	for (i = 0; i < evt->num_bis; i++) {
		struct hci_conn *conn;
		uint16_t handle;

		if (!util_iov_pull_le16(iov, &handle))
			return;

		conn = conn_lookup_type(dev, handle, CONN_LE_ISO);
		if (conn)
			conn->setup_seen = true;
	}
}

static void evt_le_big_sync_established(struct hci_dev *dev, struct timeval *tv,
					struct iovec *iov)
{
	const struct bt_hci_evt_le_big_sync_estabilished *evt;
	int i;

	evt = util_iov_pull_mem(iov, sizeof(*evt));
	if (!evt || evt->status)
		return;

	for (i = 0; i < evt->num_bis; i++) {
		struct hci_conn *conn;
		uint16_t handle;

		if (!util_iov_pull_le16(iov, &handle))
			return;

		conn = conn_lookup_type(dev, handle, CONN_LE_ISO);
		if (conn)
			conn->setup_seen = true;
	}
}

static void evt_le_meta_event(struct hci_dev *dev, struct timeval *tv,
					const void *data, uint16_t size)
{
	struct iovec iov = {
		.iov_base = (void *)data,
		.iov_len = size,
	};
	uint8_t subevt;

	if (!util_iov_pull_u8(&iov, &subevt))
		return;

	switch (subevt) {
	case BT_HCI_EVT_LE_CIS_ESTABLISHED:
		evt_le_cis_established(dev, tv, &iov);
		break;
	case BT_HCI_EVT_LE_BIG_COMPLETE:
		evt_le_big_complete(dev, tv, &iov);
		break;
	case BT_HCI_EVT_LE_BIG_SYNC_ESTABILISHED:
		evt_le_big_sync_established(dev, tv, &iov);
		break;
	}
}

static void event_pkt(struct timeval *tv, uint16_t index,
					const void *data, uint16_t size)
{
	const struct bt_hci_evt_hdr *hdr = data;
	struct hci_dev *dev;

	data += sizeof(*hdr);
	size -= sizeof(*hdr);

	dev = dev_lookup(index);
	if (!dev)
		return;

	dev->num_hci++;
	dev->num_evt++;

	switch (hdr->evt) {
	case BT_HCI_EVT_CONN_COMPLETE:
		evt_conn_complete(dev, tv, data, size);
		break;
	case BT_HCI_EVT_DISCONNECT_COMPLETE:
		evt_disconnect_complete(dev, tv, data, size);
		break;
	case BT_HCI_EVT_CMD_COMPLETE:
		evt_cmd_complete(dev, tv, data, size);
		break;
	case BT_HCI_EVT_NUM_COMPLETED_PACKETS:
		evt_num_completed_packets(dev, tv, data, size);
		break;
	case BT_HCI_EVT_SYNC_CONN_COMPLETE:
		evt_sync_conn_complete(dev, tv, data, size);
		break;
	case BT_HCI_EVT_LE_META_EVENT:
		evt_le_meta_event(dev, tv, data, size);
		break;
	}
}

static void conn_pkt_tx(struct hci_conn *conn, struct timeval *tv,
				uint16_t size, struct l2cap_chan *chan)
{
	struct hci_conn_tx *last_tx;

	conn->tx_num++;

	last_tx = new0(struct hci_conn_tx, 1);
	memcpy(last_tx, tv, sizeof(*tv));
	last_tx->chan = chan;
	queue_push_tail(conn->tx_queue, last_tx);
	conn->tx_bytes += size;

	if (!conn->tx_pkt_min || size < conn->tx_pkt_min)
		conn->tx_pkt_min = size;
	if (!conn->tx_pkt_max || size > conn->tx_pkt_max)
		conn->tx_pkt_max = size;

	if (chan) {
		chan->tx_bytes += size;

		if (!chan->tx_pkt_min || size < chan->tx_pkt_min)
			chan->tx_pkt_min = size;
		if (!chan->tx_pkt_max || size > chan->tx_pkt_max)
			chan->tx_pkt_max = size;
	}
}

static void acl_pkt(struct timeval *tv, uint16_t index, bool out,
					const void *data, uint16_t size)
{
	const struct bt_hci_acl_hdr *hdr = data;
	struct hci_dev *dev;
	struct hci_conn *conn;
	struct l2cap_chan *chan = NULL;
	uint16_t cid;

	data += sizeof(*hdr);
	size -= sizeof(*hdr);

	dev = dev_lookup(index);
	if (!dev)
		return;

	dev->num_hci++;
	dev->num_acl++;

	conn = conn_lookup_type(dev, le16_to_cpu(hdr->handle) & 0x0fff,
								CONN_BR_ACL);
	if (!conn)
		return;

	switch (le16_to_cpu(hdr->handle) >> 12) {
	case 0x00:
	case 0x02:
		cid = get_le16(data + 2);
		chan = chan_lookup(conn, cid, out);
		if (chan)
			chan->num++;
		if (cid == 1)
			l2cap_sig(conn, out, data + 4, size - 4);
		break;
	}

	if (out) {
		conn_pkt_tx(conn, tv, size, chan);
	} else {
		conn->rx_num++;
	}
}

static void sco_pkt(struct timeval *tv, uint16_t index, bool out,
					const void *data, uint16_t size)
{
	const struct bt_hci_acl_hdr *hdr = data;
	struct hci_dev *dev;
	struct hci_conn *conn;

	dev = dev_lookup(index);
	if (!dev)
		return;

	dev->num_hci++;
	dev->num_sco++;

	conn = conn_lookup_type(dev, le16_to_cpu(hdr->handle) & 0x0fff,
								CONN_BR_SCO);
	if (!conn)
		return;

	if (out) {
		conn_pkt_tx(conn, tv, size - sizeof(*hdr), NULL);
	} else {
		conn->rx_num++;
	}
}

static void info_index(struct timeval *tv, uint16_t index,
					const void *data, uint16_t size)
{
	const struct btsnoop_opcode_index_info *hdr = data;
	struct hci_dev *dev;

	dev = dev_lookup(index);
	if (!dev)
		return;

	dev->manufacturer = hdr->manufacturer;
}

static void vendor_diag(struct timeval *tv, uint16_t index,
					const void *data, uint16_t size)
{
	struct hci_dev *dev;

	dev = dev_lookup(index);
	if (!dev)
		return;

	dev->vendor_diag++;
}

static void system_note(struct timeval *tv, uint16_t index,
					const void *data, uint16_t size)
{
	struct hci_dev *dev;

	dev = dev_lookup(index);
	if (!dev)
		return;

	dev->system_note++;
}

static void user_log(struct timeval *tv, uint16_t index,
					const void *data, uint16_t size)
{
	struct hci_dev *dev;

	dev = dev_lookup(index);
	if (!dev)
		return;

	dev->user_log++;
}

static void ctrl_msg(struct timeval *tv, uint16_t index,
					const void *data, uint16_t size)
{
	struct hci_dev *dev;

	dev = dev_lookup(index);
	if (!dev)
		return;

	dev->ctrl_msg++;
}

static void iso_pkt(struct timeval *tv, uint16_t index, bool out,
					const void *data, uint16_t size)
{
	const struct bt_hci_iso_hdr *hdr = data;
	struct hci_conn *conn;
	struct hci_dev *dev;

	dev = dev_lookup(index);
	if (!dev)
		return;

	dev->num_hci++;
	dev->num_iso++;

	conn = conn_lookup_type(dev, le16_to_cpu(hdr->handle) & 0x0fff,
								CONN_LE_ISO);
	if (!conn)
		return;

	if (out) {
		conn_pkt_tx(conn, tv, size - sizeof(*hdr), NULL);
	} else {
		conn->rx_num++;
	}
}

static void unknown_opcode(struct timeval *tv, uint16_t index,
					const void *data, uint16_t size)
{
	struct hci_dev *dev;

	dev = dev_lookup(index);
	if (!dev)
		return;

	dev->unknown++;
}

void analyze_trace(const char *path)
{
	struct btsnoop *btsnoop_file;
	unsigned long num_packets = 0;
	uint32_t format;

	btsnoop_file = btsnoop_open(path, BTSNOOP_FLAG_PKLG_SUPPORT);
	if (!btsnoop_file)
		return;

	format = btsnoop_get_format(btsnoop_file);

	switch (format) {
	case BTSNOOP_FORMAT_HCI:
	case BTSNOOP_FORMAT_UART:
	case BTSNOOP_FORMAT_MONITOR:
		break;
	default:
		fprintf(stderr, "Unsupported packet format\n");
		goto done;
	}

	dev_list = queue_new();

	while (1) {
		unsigned char buf[BTSNOOP_MAX_PACKET_SIZE];
		struct timeval tv;
		uint16_t index, opcode, pktlen;

		if (!btsnoop_read_hci(btsnoop_file, &tv, &index, &opcode,
								buf, &pktlen))
			break;

		switch (opcode) {
		case BTSNOOP_OPCODE_NEW_INDEX:
			new_index(&tv, index, buf, pktlen);
			break;
		case BTSNOOP_OPCODE_DEL_INDEX:
			del_index(&tv, index, buf, pktlen);
			break;
		case BTSNOOP_OPCODE_COMMAND_PKT:
			command_pkt(&tv, index, buf, pktlen);
			break;
		case BTSNOOP_OPCODE_EVENT_PKT:
			event_pkt(&tv, index, buf, pktlen);
			break;
		case BTSNOOP_OPCODE_ACL_TX_PKT:
			acl_pkt(&tv, index, true, buf, pktlen);
			break;
		case BTSNOOP_OPCODE_ACL_RX_PKT:
			acl_pkt(&tv, index, false, buf, pktlen);
			break;
		case BTSNOOP_OPCODE_SCO_TX_PKT:
			sco_pkt(&tv, index, true, buf, pktlen);
			break;
		case BTSNOOP_OPCODE_SCO_RX_PKT:
			sco_pkt(&tv, index, false, buf, pktlen);
			break;
		case BTSNOOP_OPCODE_OPEN_INDEX:
		case BTSNOOP_OPCODE_CLOSE_INDEX:
			break;
		case BTSNOOP_OPCODE_INDEX_INFO:
			info_index(&tv, index, buf, pktlen);
			break;
		case BTSNOOP_OPCODE_VENDOR_DIAG:
			vendor_diag(&tv, index, buf, pktlen);
			break;
		case BTSNOOP_OPCODE_SYSTEM_NOTE:
			system_note(&tv, index, buf, pktlen);
			break;
		case BTSNOOP_OPCODE_USER_LOGGING:
			user_log(&tv, index, buf, pktlen);
			break;
		case BTSNOOP_OPCODE_CTRL_OPEN:
		case BTSNOOP_OPCODE_CTRL_CLOSE:
		case BTSNOOP_OPCODE_CTRL_COMMAND:
		case BTSNOOP_OPCODE_CTRL_EVENT:
			ctrl_msg(&tv, index, buf, pktlen);
			break;
		case BTSNOOP_OPCODE_ISO_TX_PKT:
			iso_pkt(&tv, index, true, buf, pktlen);
			break;
		case BTSNOOP_OPCODE_ISO_RX_PKT:
			iso_pkt(&tv, index, false, buf, pktlen);
			break;
		default:
			unknown_opcode(&tv, index, buf, pktlen);
			break;
		}

		num_packets++;
	}

	printf("Trace contains %lu packets\n\n", num_packets);

	queue_destroy(dev_list, dev_destroy);

done:
	btsnoop_unref(btsnoop_file);
}
