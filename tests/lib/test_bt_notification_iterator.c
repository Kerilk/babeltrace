/*
 * Copyright 2017 - Philippe Proulx <pproulx@efficios.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; under version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>
#include <babeltrace/ctf-ir/event-class.h>
#include <babeltrace/ctf-ir/event.h>
#include <babeltrace/ctf-ir/field-types.h>
#include <babeltrace/ctf-ir/fields.h>
#include <babeltrace/ctf-ir/packet.h>
#include <babeltrace/ctf-ir/stream-class.h>
#include <babeltrace/ctf-ir/stream.h>
#include <babeltrace/ctf-ir/trace.h>
#include <babeltrace/graph/clock-class-priority-map.h>
#include <babeltrace/graph/component-class-filter.h>
#include <babeltrace/graph/component-class-sink.h>
#include <babeltrace/graph/component-class-source.h>
#include <babeltrace/graph/component-class.h>
#include <babeltrace/graph/component-sink.h>
#include <babeltrace/graph/component-source.h>
#include <babeltrace/graph/component.h>
#include <babeltrace/graph/connection.h>
#include <babeltrace/graph/graph.h>
#include <babeltrace/graph/notification-event.h>
#include <babeltrace/graph/notification-inactivity.h>
#include <babeltrace/graph/notification-iterator.h>
#include <babeltrace/graph/notification-packet.h>
#include <babeltrace/graph/notification-stream.h>
#include <babeltrace/graph/output-port-notification-iterator.h>
#include <babeltrace/graph/port.h>
#include <babeltrace/graph/private-component-source.h>
#include <babeltrace/graph/private-component-sink.h>
#include <babeltrace/graph/private-component.h>
#include <babeltrace/graph/private-connection.h>
#include <babeltrace/graph/private-connection-private-notification-iterator.h>
#include <babeltrace/graph/private-port.h>
#include <babeltrace/plugin/plugin.h>
#include <babeltrace/ref.h>
#include <glib.h>

#include "tap/tap.h"

#define NR_TESTS	6

enum test {
	TEST_NO_AUTO_NOTIFS,
	TEST_OUTPUT_PORT_NOTIFICATION_ITERATOR,
};

enum test_event_type {
	TEST_EV_TYPE_NOTIF_UNEXPECTED,
	TEST_EV_TYPE_NOTIF_EVENT,
	TEST_EV_TYPE_NOTIF_INACTIVITY,
	TEST_EV_TYPE_NOTIF_STREAM_BEGIN,
	TEST_EV_TYPE_NOTIF_PACKET_BEGIN,
	TEST_EV_TYPE_NOTIF_PACKET_END,
	TEST_EV_TYPE_NOTIF_STREAM_END,
	TEST_EV_TYPE_END,
	TEST_EV_TYPE_SENTINEL,
};

struct test_event {
	enum test_event_type type;
	struct bt_stream *stream;
	struct bt_packet *packet;
};

static bool debug = false;
static enum test current_test;
static GArray *test_events;
static struct bt_clock_class_priority_map *src_empty_cc_prio_map;
static struct bt_stream_class *src_stream_class;
static struct bt_event_class *src_event_class;
static struct bt_stream *src_stream1;
static struct bt_stream *src_stream2;
static struct bt_packet *src_stream1_packet1;
static struct bt_packet *src_stream1_packet2;
static struct bt_packet *src_stream2_packet1;
static struct bt_packet *src_stream2_packet2;

enum {
	SEQ_END = -1,
	SEQ_STREAM1_BEGIN = -2,
	SEQ_STREAM2_BEGIN = -3,
	SEQ_STREAM1_END = -4,
	SEQ_STREAM2_END = -5,
	SEQ_STREAM1_PACKET1_BEGIN = -6,
	SEQ_STREAM1_PACKET2_BEGIN = -7,
	SEQ_STREAM2_PACKET1_BEGIN = -8,
	SEQ_STREAM2_PACKET2_BEGIN = -9,
	SEQ_STREAM1_PACKET1_END = -10,
	SEQ_STREAM1_PACKET2_END = -11,
	SEQ_STREAM2_PACKET1_END = -12,
	SEQ_STREAM2_PACKET2_END = -13,
	SEQ_EVENT_STREAM1_PACKET1 = -14,
	SEQ_EVENT_STREAM1_PACKET2 = -15,
	SEQ_EVENT_STREAM2_PACKET1 = -16,
	SEQ_EVENT_STREAM2_PACKET2 = -17,
	SEQ_INACTIVITY = -18,
};

struct src_iter_user_data {
	int64_t *seq;
	size_t at;
};

struct sink_user_data {
	struct bt_notification_iterator *notif_iter;
};

/*
 * No automatic notifications generated in this block.
 * Stream 2 notifications are more indented.
 */
static int64_t seq_no_auto_notifs[] = {
	SEQ_STREAM1_BEGIN,
	SEQ_STREAM1_PACKET1_BEGIN,
	SEQ_EVENT_STREAM1_PACKET1,
	SEQ_EVENT_STREAM1_PACKET1,
		SEQ_STREAM2_BEGIN,
	SEQ_EVENT_STREAM1_PACKET1,
		SEQ_STREAM2_PACKET2_BEGIN,
		SEQ_EVENT_STREAM2_PACKET2,
	SEQ_EVENT_STREAM1_PACKET1,
	SEQ_STREAM1_PACKET1_END,
		SEQ_STREAM2_PACKET2_END,
	SEQ_STREAM1_PACKET2_BEGIN,
	SEQ_EVENT_STREAM1_PACKET2,
		SEQ_STREAM2_END,
	SEQ_STREAM1_PACKET2_END,
	SEQ_STREAM1_END,
	SEQ_END,
};

static
void clear_test_events(void)
{
	g_array_set_size(test_events, 0);
}

static
void print_test_event(FILE *fp, const struct test_event *event)
{
	fprintf(fp, "{ type = ");

	switch (event->type) {
	case TEST_EV_TYPE_NOTIF_UNEXPECTED:
		fprintf(fp, "TEST_EV_TYPE_NOTIF_UNEXPECTED");
		break;
	case TEST_EV_TYPE_NOTIF_EVENT:
		fprintf(fp, "TEST_EV_TYPE_NOTIF_EVENT");
		break;
	case TEST_EV_TYPE_NOTIF_INACTIVITY:
		fprintf(fp, "TEST_EV_TYPE_NOTIF_INACTIVITY");
		break;
	case TEST_EV_TYPE_NOTIF_STREAM_BEGIN:
		fprintf(fp, "TEST_EV_TYPE_NOTIF_STREAM_BEGIN");
		break;
	case TEST_EV_TYPE_NOTIF_STREAM_END:
		fprintf(fp, "TEST_EV_TYPE_NOTIF_STREAM_END");
		break;
	case TEST_EV_TYPE_NOTIF_PACKET_BEGIN:
		fprintf(fp, "TEST_EV_TYPE_NOTIF_PACKET_BEGIN");
		break;
	case TEST_EV_TYPE_NOTIF_PACKET_END:
		fprintf(fp, "TEST_EV_TYPE_NOTIF_PACKET_END");
		break;
	case TEST_EV_TYPE_END:
		fprintf(fp, "TEST_EV_TYPE_END");
		break;
	case TEST_EV_TYPE_SENTINEL:
		fprintf(fp, "TEST_EV_TYPE_SENTINEL");
		break;
	default:
		fprintf(fp, "(UNKNOWN)");
		break;
	}

	fprintf(fp, ", stream = %p, packet = %p }", event->stream,
		event->packet);
}

static
void append_test_event(struct test_event *event)
{
	g_array_append_val(test_events, *event);
}

static
bool compare_single_test_events(const struct test_event *ev_a,
		const struct test_event *ev_b)
{
	if (debug) {
		fprintf(stderr, ":: Comparing test events: ");
		print_test_event(stderr, ev_a);
		fprintf(stderr, " vs. ");
		print_test_event(stderr, ev_b);
		fprintf(stderr, "\n");
	}

	if (ev_a->type != ev_b->type) {
		return false;
	}

	switch (ev_a->type) {
	case TEST_EV_TYPE_END:
	case TEST_EV_TYPE_SENTINEL:
		break;
	default:
		if (ev_a->stream != ev_b->stream) {
			return false;
		}

		if (ev_a->packet != ev_b->packet) {
			return false;
		}
		break;
	}

	return true;
}

static
bool compare_test_events(const struct test_event *expected_events)
{
	const struct test_event *expected_event = expected_events;
	size_t i = 0;

	assert(expected_events);

	while (true) {
		const struct test_event *event;

		if (expected_event->type == TEST_EV_TYPE_SENTINEL) {
			break;
		}

		if (i >= test_events->len) {
			return false;
		}

		event = &g_array_index(test_events, struct test_event, i);

		if (!compare_single_test_events(event, expected_event)) {
			return false;
		}

		i++;
		expected_event++;
	}

	if (i != test_events->len) {
		return false;
	}

	return true;
}

static
void init_static_data(void)
{
	int ret;
	struct bt_trace *trace;
	struct bt_field_type *empty_struct_ft;

	/* Test events */
	test_events = g_array_new(FALSE, TRUE, sizeof(struct test_event));
	assert(test_events);

	/* Metadata */
	empty_struct_ft = bt_field_type_structure_create();
	assert(empty_struct_ft);
	trace = bt_trace_create();
	assert(trace);
	ret = bt_trace_set_packet_header_field_type(trace, empty_struct_ft);
	assert(ret == 0);
	src_empty_cc_prio_map = bt_clock_class_priority_map_create();
	assert(src_empty_cc_prio_map);
	src_stream_class = bt_stream_class_create("my-stream-class");
	assert(src_stream_class);
	ret = bt_stream_class_set_packet_context_field_type(src_stream_class,
		empty_struct_ft);
	assert(ret == 0);
	ret = bt_stream_class_set_event_header_field_type(src_stream_class,
		empty_struct_ft);
	assert(ret == 0);
	ret = bt_stream_class_set_event_context_field_type(src_stream_class,
		empty_struct_ft);
	assert(ret == 0);
	src_event_class = bt_event_class_create("my-event-class");
	ret = bt_event_class_set_context_field_type(src_event_class,
		empty_struct_ft);
	assert(ret == 0);
	ret = bt_event_class_set_payload_field_type(src_event_class,
		empty_struct_ft);
	assert(ret == 0);
	ret = bt_stream_class_add_event_class(src_stream_class,
		src_event_class);
	assert(ret == 0);
	ret = bt_trace_add_stream_class(trace, src_stream_class);
	assert(ret == 0);
	src_stream1 = bt_stream_create(src_stream_class, "stream-1", 0);
	assert(src_stream1);
	src_stream2 = bt_stream_create(src_stream_class, "stream-2", 1);
	assert(src_stream2);
	src_stream1_packet1 = bt_packet_create(src_stream1);
	assert(src_stream1_packet1);
	src_stream1_packet2 = bt_packet_create(src_stream1);
	assert(src_stream1_packet2);
	src_stream2_packet1 = bt_packet_create(src_stream2);
	assert(src_stream2_packet1);
	src_stream2_packet2 = bt_packet_create(src_stream2);
	assert(src_stream2_packet2);

	if (debug) {
		fprintf(stderr, ":: stream 1: %p\n", src_stream1);
		fprintf(stderr, ":: stream 2: %p\n", src_stream2);
		fprintf(stderr, ":: stream 1, packet 1: %p\n", src_stream1_packet1);
		fprintf(stderr, ":: stream 1, packet 2: %p\n", src_stream1_packet2);
		fprintf(stderr, ":: stream 2, packet 1: %p\n", src_stream2_packet1);
		fprintf(stderr, ":: stream 2, packet 2: %p\n", src_stream2_packet2);
	}

	bt_put(trace);
	bt_put(empty_struct_ft);
}

static
void fini_static_data(void)
{
	/* Test events */
	g_array_free(test_events, TRUE);

	/* Metadata */
	bt_put(src_empty_cc_prio_map);
	bt_put(src_stream_class);
	bt_put(src_event_class);
	bt_put(src_stream1);
	bt_put(src_stream2);
	bt_put(src_stream1_packet1);
	bt_put(src_stream1_packet2);
	bt_put(src_stream2_packet1);
	bt_put(src_stream2_packet2);
}

static
void src_iter_finalize(
		struct bt_private_connection_private_notification_iterator *private_notification_iterator)
{
	struct src_iter_user_data *user_data =
		bt_private_connection_private_notification_iterator_get_user_data(
			private_notification_iterator);

	if (user_data) {
		g_free(user_data);
	}
}

static
enum bt_notification_iterator_status src_iter_init(
		struct bt_private_connection_private_notification_iterator *priv_notif_iter,
		struct bt_private_port *private_port)
{
	struct src_iter_user_data *user_data =
		g_new0(struct src_iter_user_data, 1);
	int ret;

	assert(user_data);
	ret = bt_private_connection_private_notification_iterator_set_user_data(
		priv_notif_iter, user_data);
	assert(ret == 0);

	switch (current_test) {
	case TEST_NO_AUTO_NOTIFS:
	case TEST_OUTPUT_PORT_NOTIFICATION_ITERATOR:
		user_data->seq = seq_no_auto_notifs;
		break;
	default:
		abort();
	}

	return BT_NOTIFICATION_ITERATOR_STATUS_OK;
}

static
struct bt_event *src_create_event(struct bt_packet *packet)
{
	struct bt_event *event = bt_event_create(src_event_class);
	int ret;

	assert(event);
	ret = bt_event_set_packet(event, packet);
	assert(ret == 0);
	return event;
}

static
struct bt_notification_iterator_next_method_return src_iter_next_seq(
		struct src_iter_user_data *user_data)
{
	struct bt_notification_iterator_next_method_return next_return = {
		.status = BT_NOTIFICATION_ITERATOR_STATUS_OK,
	};
	int64_t cur_ts_ns;
	struct bt_packet *event_packet = NULL;

	assert(user_data->seq);
	cur_ts_ns = user_data->seq[user_data->at];

	switch (cur_ts_ns) {
	case SEQ_END:
		next_return.status =
			BT_NOTIFICATION_ITERATOR_STATUS_END;
		break;
	case SEQ_INACTIVITY:
		next_return.notification =
			bt_notification_inactivity_create(src_empty_cc_prio_map);
		assert(next_return.notification);
		break;
	case SEQ_STREAM1_BEGIN:
		next_return.notification =
			bt_notification_stream_begin_create(src_stream1);
		assert(next_return.notification);
		break;
	case SEQ_STREAM2_BEGIN:
		next_return.notification =
			bt_notification_stream_begin_create(src_stream2);
		assert(next_return.notification);
		break;
	case SEQ_STREAM1_END:
		next_return.notification =
			bt_notification_stream_end_create(src_stream1);
		assert(next_return.notification);
		break;
	case SEQ_STREAM2_END:
		next_return.notification =
			bt_notification_stream_end_create(src_stream2);
		assert(next_return.notification);
		break;
	case SEQ_STREAM1_PACKET1_BEGIN:
		next_return.notification =
			bt_notification_packet_begin_create(src_stream1_packet1);
		assert(next_return.notification);
		break;
	case SEQ_STREAM1_PACKET2_BEGIN:
		next_return.notification =
			bt_notification_packet_begin_create(src_stream1_packet2);
		assert(next_return.notification);
		break;
	case SEQ_STREAM2_PACKET1_BEGIN:
		next_return.notification =
			bt_notification_packet_begin_create(src_stream2_packet1);
		assert(next_return.notification);
		break;
	case SEQ_STREAM2_PACKET2_BEGIN:
		next_return.notification =
			bt_notification_packet_begin_create(src_stream2_packet2);
		assert(next_return.notification);
		break;
	case SEQ_STREAM1_PACKET1_END:
		next_return.notification =
			bt_notification_packet_end_create(src_stream1_packet1);
		assert(next_return.notification);
		break;
	case SEQ_STREAM1_PACKET2_END:
		next_return.notification =
			bt_notification_packet_end_create(src_stream1_packet2);
		assert(next_return.notification);
		break;
	case SEQ_STREAM2_PACKET1_END:
		next_return.notification =
			bt_notification_packet_end_create(src_stream2_packet1);
		assert(next_return.notification);
		break;
	case SEQ_STREAM2_PACKET2_END:
		next_return.notification =
			bt_notification_packet_end_create(src_stream2_packet2);
		assert(next_return.notification);
		break;
	case SEQ_EVENT_STREAM1_PACKET1:
		event_packet = src_stream1_packet1;
		break;
	case SEQ_EVENT_STREAM1_PACKET2:
		event_packet = src_stream1_packet2;
		break;
	case SEQ_EVENT_STREAM2_PACKET1:
		event_packet = src_stream2_packet1;
		break;
	case SEQ_EVENT_STREAM2_PACKET2:
		event_packet = src_stream2_packet2;
		break;
	default:
		abort();
	}

	if (event_packet) {
		struct bt_event *event = src_create_event(event_packet);

		assert(event);
		next_return.notification = bt_notification_event_create(event,
			src_empty_cc_prio_map);
		bt_put(event);
		assert(next_return.notification);
	}

	if (next_return.status != BT_NOTIFICATION_ITERATOR_STATUS_END) {
		user_data->at++;
	}

	return next_return;
}

static
struct bt_notification_iterator_next_method_return src_iter_next(
		struct bt_private_connection_private_notification_iterator *priv_iterator)
{
	struct bt_notification_iterator_next_method_return next_return = {
		.status = BT_NOTIFICATION_ITERATOR_STATUS_OK,
		.notification = NULL,
	};
	struct src_iter_user_data *user_data =
		bt_private_connection_private_notification_iterator_get_user_data(priv_iterator);

	assert(user_data);
	next_return = src_iter_next_seq(user_data);
	return next_return;
}

static
enum bt_component_status src_init(
		struct bt_private_component *private_component,
		struct bt_value *params, void *init_method_data)
{
	int ret;

	ret = bt_private_component_source_add_output_private_port(
		private_component, "out", NULL, NULL);
	assert(ret == 0);
	return BT_COMPONENT_STATUS_OK;
}

static
void src_finalize(struct bt_private_component *private_component)
{
}

static
enum bt_notification_iterator_status common_consume(
		struct bt_notification_iterator *notif_iter)
{
	enum bt_notification_iterator_status ret;
	struct bt_notification *notification = NULL;
	struct test_event test_event = { 0 };
	bool do_append_test_event = true;
	assert(notif_iter);

	ret = bt_notification_iterator_next(notif_iter);
	if (ret < 0) {
		do_append_test_event = false;
		goto end;
	}

	switch (ret) {
	case BT_NOTIFICATION_ITERATOR_STATUS_END:
		test_event.type = TEST_EV_TYPE_END;
		goto end;
	case BT_NOTIFICATION_ITERATOR_STATUS_AGAIN:
		abort();
	default:
		break;
	}

	notification = bt_notification_iterator_get_notification(
		notif_iter);
	assert(notification);

	switch (bt_notification_get_type(notification)) {
	case BT_NOTIFICATION_TYPE_EVENT:
	{
		struct bt_event *event;

		test_event.type = TEST_EV_TYPE_NOTIF_EVENT;
		event = bt_notification_event_get_event(notification);
		assert(event);
		test_event.packet = bt_event_get_packet(event);
		bt_put(event);
		assert(test_event.packet);
		bt_put(test_event.packet);
		break;
	}
	case BT_NOTIFICATION_TYPE_INACTIVITY:
		test_event.type = TEST_EV_TYPE_NOTIF_INACTIVITY;
		break;
	case BT_NOTIFICATION_TYPE_STREAM_BEGIN:
		test_event.type = TEST_EV_TYPE_NOTIF_STREAM_BEGIN;
		test_event.stream =
			bt_notification_stream_begin_get_stream(notification);
		assert(test_event.stream);
		bt_put(test_event.stream);
		break;
	case BT_NOTIFICATION_TYPE_STREAM_END:
		test_event.type = TEST_EV_TYPE_NOTIF_STREAM_END;
		test_event.stream =
			bt_notification_stream_end_get_stream(notification);
		assert(test_event.stream);
		bt_put(test_event.stream);
		break;
	case BT_NOTIFICATION_TYPE_PACKET_BEGIN:
		test_event.type = TEST_EV_TYPE_NOTIF_PACKET_BEGIN;
		test_event.packet =
			bt_notification_packet_begin_get_packet(notification);
		assert(test_event.packet);
		bt_put(test_event.packet);
		break;
	case BT_NOTIFICATION_TYPE_PACKET_END:
		test_event.type = TEST_EV_TYPE_NOTIF_PACKET_END;
		test_event.packet =
			bt_notification_packet_end_get_packet(notification);
		assert(test_event.packet);
		bt_put(test_event.packet);
		break;
	default:
		test_event.type = TEST_EV_TYPE_NOTIF_UNEXPECTED;
		break;
	}

	if (test_event.packet) {
		test_event.stream = bt_packet_get_stream(test_event.packet);
		assert(test_event.stream);
		bt_put(test_event.stream);
	}

end:
	if (do_append_test_event) {
		append_test_event(&test_event);
	}

	bt_put(notification);
	return ret;
}

static
enum bt_component_status sink_consume(
		struct bt_private_component *priv_component)
{
	enum bt_component_status ret = BT_COMPONENT_STATUS_OK;
	struct sink_user_data *user_data =
		bt_private_component_get_user_data(priv_component);
	enum bt_notification_iterator_status it_ret;

	assert(user_data && user_data->notif_iter);
	it_ret = common_consume(user_data->notif_iter);

	if (it_ret < 0) {
		ret = BT_COMPONENT_STATUS_ERROR;
		goto end;
	}

	switch (it_ret) {
	case BT_NOTIFICATION_ITERATOR_STATUS_END:
		ret = BT_COMPONENT_STATUS_END;
		BT_PUT(user_data->notif_iter);
		goto end;
	case BT_NOTIFICATION_ITERATOR_STATUS_AGAIN:
		abort();
	default:
		break;
	}

end:
	return ret;
}

static
void sink_port_connected(struct bt_private_component *private_component,
		struct bt_private_port *self_private_port,
		struct bt_port *other_port)
{
	struct bt_private_connection *priv_conn =
		bt_private_port_get_private_connection(self_private_port);
	struct sink_user_data *user_data = bt_private_component_get_user_data(
		private_component);
	enum bt_connection_status conn_status;

	assert(user_data);
	assert(priv_conn);
	conn_status = bt_private_connection_create_notification_iterator(
		priv_conn, &user_data->notif_iter);
	assert(conn_status == 0);
	bt_put(priv_conn);
}

static
enum bt_component_status sink_init(
		struct bt_private_component *private_component,
		struct bt_value *params, void *init_method_data)
{
	struct sink_user_data *user_data = g_new0(struct sink_user_data, 1);
	int ret;

	assert(user_data);
	ret = bt_private_component_set_user_data(private_component,
		user_data);
	assert(ret == 0);
	ret = bt_private_component_sink_add_input_private_port(
		private_component, "in", NULL, NULL);
	assert(ret == 0);
	return BT_COMPONENT_STATUS_OK;
}

static
void sink_finalize(struct bt_private_component *private_component)
{
	struct sink_user_data *user_data = bt_private_component_get_user_data(
		private_component);

	if (user_data) {
		bt_put(user_data->notif_iter);
		g_free(user_data);
	}
}

static
void create_source_sink(struct bt_graph *graph, struct bt_component **source,
		struct bt_component **sink)
{
	struct bt_component_class *src_comp_class;
	struct bt_component_class *sink_comp_class;
	int ret;

	/* Create source component */
	if (source) {
		src_comp_class = bt_component_class_source_create("src",
			src_iter_next);
		assert(src_comp_class);
		ret = bt_component_class_set_init_method(src_comp_class,
			src_init);
		assert(ret == 0);
		ret = bt_component_class_set_finalize_method(src_comp_class,
			src_finalize);
		assert(ret == 0);
		ret = bt_component_class_source_set_notification_iterator_init_method(
			src_comp_class, src_iter_init);
		assert(ret == 0);
		ret = bt_component_class_source_set_notification_iterator_finalize_method(
			src_comp_class, src_iter_finalize);
		assert(ret == 0);
		ret = bt_graph_add_component(graph, src_comp_class, "source",
			NULL, source);
		assert(ret == 0);
		bt_put(src_comp_class);
	}

	/* Create sink component */
	if (sink) {
		sink_comp_class = bt_component_class_sink_create("sink",
			sink_consume);
		assert(sink_comp_class);
		ret = bt_component_class_set_init_method(sink_comp_class,
			sink_init);
		assert(ret == 0);
		ret = bt_component_class_set_finalize_method(sink_comp_class,
			sink_finalize);
		ret = bt_component_class_set_port_connected_method(
			sink_comp_class, sink_port_connected);
		assert(ret == 0);
		ret = bt_graph_add_component(graph, sink_comp_class, "sink",
			NULL, sink);
		assert(ret == 0);
		bt_put(sink_comp_class);
	}
}

static
void do_std_test(enum test test, const char *name,
		const struct test_event *expected_test_events)
{
	struct bt_component *src_comp;
	struct bt_component *sink_comp;
	struct bt_port *upstream_port;
	struct bt_port *downstream_port;
	struct bt_graph *graph;
	enum bt_graph_status graph_status = BT_GRAPH_STATUS_OK;

	clear_test_events();
	current_test = test;
	diag("test: %s", name);
	graph = bt_graph_create();
	assert(graph);
	create_source_sink(graph, &src_comp, &sink_comp);

	/* Connect source to sink */
	upstream_port = bt_component_source_get_output_port_by_name(src_comp, "out");
	assert(upstream_port);
	downstream_port = bt_component_sink_get_input_port_by_name(sink_comp, "in");
	assert(downstream_port);
	graph_status = bt_graph_connect_ports(graph, upstream_port,
		downstream_port, NULL);
	bt_put(upstream_port);
	bt_put(downstream_port);

	/* Run the graph until the end */
	while (graph_status == BT_GRAPH_STATUS_OK ||
			graph_status == BT_GRAPH_STATUS_AGAIN) {
		graph_status = bt_graph_run(graph);
	}

	ok(graph_status == BT_GRAPH_STATUS_END, "graph finishes without any error");

	/* Compare the resulting test events */
	if (expected_test_events) {
		ok(compare_test_events(expected_test_events),
			"the produced sequence of test events is the expected one");
	}

	bt_put(src_comp);
	bt_put(sink_comp);
	bt_put(graph);
}

static
void test_no_auto_notifs(void)
{
	const struct test_event expected_test_events[] = {
		{ .type = TEST_EV_TYPE_NOTIF_STREAM_BEGIN, .stream = src_stream1, .packet = NULL, },
		{ .type = TEST_EV_TYPE_NOTIF_PACKET_BEGIN, .stream = src_stream1, .packet = src_stream1_packet1, },
		{ .type = TEST_EV_TYPE_NOTIF_EVENT, .stream = src_stream1, .packet = src_stream1_packet1, },
		{ .type = TEST_EV_TYPE_NOTIF_EVENT, .stream = src_stream1, .packet = src_stream1_packet1, },
		{ .type = TEST_EV_TYPE_NOTIF_STREAM_BEGIN, .stream = src_stream2, .packet = NULL, },
		{ .type = TEST_EV_TYPE_NOTIF_EVENT, .stream = src_stream1, .packet = src_stream1_packet1, },
		{ .type = TEST_EV_TYPE_NOTIF_PACKET_BEGIN, .stream = src_stream2, .packet = src_stream2_packet2, },
		{ .type = TEST_EV_TYPE_NOTIF_EVENT, .stream = src_stream2, .packet = src_stream2_packet2, },
		{ .type = TEST_EV_TYPE_NOTIF_EVENT, .stream = src_stream1, .packet = src_stream1_packet1, },
		{ .type = TEST_EV_TYPE_NOTIF_PACKET_END, .stream = src_stream1, .packet = src_stream1_packet1, },
		{ .type = TEST_EV_TYPE_NOTIF_PACKET_END, .stream = src_stream2, .packet = src_stream2_packet2, },
		{ .type = TEST_EV_TYPE_NOTIF_PACKET_BEGIN, .stream = src_stream1, .packet = src_stream1_packet2, },
		{ .type = TEST_EV_TYPE_NOTIF_EVENT, .stream = src_stream1, .packet = src_stream1_packet2, },
		{ .type = TEST_EV_TYPE_NOTIF_STREAM_END, .stream = src_stream2, .packet = NULL, },
		{ .type = TEST_EV_TYPE_NOTIF_PACKET_END, .stream = src_stream1, .packet = src_stream1_packet2, },
		{ .type = TEST_EV_TYPE_NOTIF_STREAM_END, .stream = src_stream1, .packet = NULL, },
		{ .type = TEST_EV_TYPE_END, },
		{ .type = TEST_EV_TYPE_SENTINEL, },
	};

	do_std_test(TEST_NO_AUTO_NOTIFS, "no automatic notifications",
		expected_test_events);
}

static
void test_output_port_notification_iterator(void)
{
	const struct test_event expected_test_events[] = {
		{ .type = TEST_EV_TYPE_NOTIF_STREAM_BEGIN, .stream = src_stream1, .packet = NULL, },
		{ .type = TEST_EV_TYPE_NOTIF_PACKET_BEGIN, .stream = src_stream1, .packet = src_stream1_packet1, },
		{ .type = TEST_EV_TYPE_NOTIF_EVENT, .stream = src_stream1, .packet = src_stream1_packet1, },
		{ .type = TEST_EV_TYPE_NOTIF_EVENT, .stream = src_stream1, .packet = src_stream1_packet1, },
		{ .type = TEST_EV_TYPE_NOTIF_STREAM_BEGIN, .stream = src_stream2, .packet = NULL, },
		{ .type = TEST_EV_TYPE_NOTIF_EVENT, .stream = src_stream1, .packet = src_stream1_packet1, },
		{ .type = TEST_EV_TYPE_NOTIF_PACKET_BEGIN, .stream = src_stream2, .packet = src_stream2_packet2, },
		{ .type = TEST_EV_TYPE_NOTIF_EVENT, .stream = src_stream2, .packet = src_stream2_packet2, },
		{ .type = TEST_EV_TYPE_NOTIF_EVENT, .stream = src_stream1, .packet = src_stream1_packet1, },
		{ .type = TEST_EV_TYPE_NOTIF_PACKET_END, .stream = src_stream1, .packet = src_stream1_packet1, },
		{ .type = TEST_EV_TYPE_NOTIF_PACKET_END, .stream = src_stream2, .packet = src_stream2_packet2, },
		{ .type = TEST_EV_TYPE_NOTIF_PACKET_BEGIN, .stream = src_stream1, .packet = src_stream1_packet2, },
		{ .type = TEST_EV_TYPE_NOTIF_EVENT, .stream = src_stream1, .packet = src_stream1_packet2, },
		{ .type = TEST_EV_TYPE_NOTIF_STREAM_END, .stream = src_stream2, .packet = NULL, },
		{ .type = TEST_EV_TYPE_NOTIF_PACKET_END, .stream = src_stream1, .packet = src_stream1_packet2, },
		{ .type = TEST_EV_TYPE_NOTIF_STREAM_END, .stream = src_stream1, .packet = NULL, },
		{ .type = TEST_EV_TYPE_END, },
		{ .type = TEST_EV_TYPE_SENTINEL, },
	};
	struct bt_component *src_comp;
	struct bt_notification_iterator *notif_iter;
	enum bt_notification_iterator_status iter_status =
		BT_NOTIFICATION_ITERATOR_STATUS_OK;
	struct bt_port *upstream_port;
	struct bt_graph *graph;

	clear_test_events();
	current_test = TEST_OUTPUT_PORT_NOTIFICATION_ITERATOR;
	diag("test: output port notification iterator");
	graph = bt_graph_create();
	assert(graph);
	create_source_sink(graph, &src_comp, NULL);

	/* Create notification iterator on source's output port */
	upstream_port = bt_component_source_get_output_port_by_name(src_comp, "out");
	notif_iter = bt_output_port_notification_iterator_create(upstream_port,
		NULL);
	ok(notif_iter, "bt_output_port_notification_iterator_create() succeeds");
	bt_put(upstream_port);

	/* Consume the notification iterator */
	while (iter_status == BT_NOTIFICATION_ITERATOR_STATUS_OK) {
		iter_status = common_consume(notif_iter);
	}

	ok(iter_status == BT_NOTIFICATION_ITERATOR_STATUS_END,
		"output port notification iterator finishes without any error");

	/* Compare the resulting test events */
	ok(compare_test_events(expected_test_events),
		"the produced sequence of test events is the expected one");

	bt_put(src_comp);
	bt_put(graph);
	bt_put(notif_iter);
}

static
void test_output_port_notification_iterator_cannot_consume(void)
{
	struct bt_component *src_comp;
	struct bt_notification_iterator *notif_iter;
	struct bt_port *upstream_port;
	struct bt_graph *graph;

	clear_test_events();
	current_test = TEST_OUTPUT_PORT_NOTIFICATION_ITERATOR;
	diag("test: cannot consume graph with existing output port notification iterator");
	graph = bt_graph_create();
	assert(graph);
	create_source_sink(graph, &src_comp, NULL);

	/* Create notification iterator on source's output port */
	upstream_port = bt_component_source_get_output_port_by_name(src_comp, "out");
	notif_iter = bt_output_port_notification_iterator_create(upstream_port,
		NULL);
	assert(notif_iter);
	bt_put(upstream_port);

	/*
	 * This should fail because the graph is now managed by the
	 * notification iterator.
	 */
	ok(bt_graph_run(graph) == BT_GRAPH_STATUS_CANNOT_CONSUME,
		"bt_graph_run() returns BT_GRAPH_STATUS_CANNOT_CONSUME when there's an output port notification iterator");

	bt_put(src_comp);
	bt_put(graph);
	bt_put(notif_iter);
}

#define DEBUG_ENV_VAR	"TEST_BT_NOTIFICATION_ITERATOR_DEBUG"

int main(int argc, char **argv)
{
	if (getenv(DEBUG_ENV_VAR) && strcmp(getenv(DEBUG_ENV_VAR), "1") == 0) {
		debug = true;
	}

	plan_tests(NR_TESTS);
	init_static_data();
	test_no_auto_notifs();
	test_output_port_notification_iterator();
	test_output_port_notification_iterator_cannot_consume();
	fini_static_data();
	return exit_status();
}