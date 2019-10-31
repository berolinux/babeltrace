/*
 * Copyright 2019 - Francis Deslauriers <francis.deslauriers@efficios.com>
 * Copyright 2016 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#define BT_COMP_LOG_SELF_COMP (viewer_connection->self_comp)
#define BT_LOG_OUTPUT_LEVEL (viewer_connection->log_level)
#define BT_LOG_TAG "PLUGIN/SRC.CTF.LTTNG-LIVE/VIEWER"
#include "logging/comp-logging.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include <glib.h>

#include "compat/socket.h"
#include "compat/endian.h"
#include "compat/compiler.h"
#include "common/common.h"
#include <babeltrace2/babeltrace.h>

#include "lttng-live.h"
#include "viewer-connection.h"
#include "lttng-viewer-abi.h"
#include "data-stream.h"
#include "metadata.h"

static
ssize_t lttng_live_recv(struct live_viewer_connection *viewer_connection,
		void *buf, size_t len)
{
	ssize_t ret;
	size_t copied = 0, to_copy = len;
	struct lttng_live_msg_iter *lttng_live_msg_iter =
		viewer_connection->lttng_live_msg_iter;
	BT_SOCKET sock = viewer_connection->control_sock;

	do {
		ret = bt_socket_recv(sock, buf + copied, to_copy, 0);
		if (ret > 0) {
			BT_ASSERT(ret <= to_copy);
			copied += ret;
			to_copy -= ret;
		}
		if (ret == BT_SOCKET_ERROR && bt_socket_interrupted()) {
			if (!viewer_connection->in_query &&
					lttng_live_graph_is_canceled(lttng_live_msg_iter)) {
				break;
			} else {
				continue;
			}
		}
	} while (ret > 0 && to_copy > 0);

	if (ret > 0) {
		ret = copied;
	}

	/* ret = 0 means orderly shutdown, ret == BT_SOCKET_ERROR is error. */
	return ret;
}

static
ssize_t lttng_live_send(struct live_viewer_connection *viewer_connection,
		const void *buf, size_t len)
{
	struct lttng_live_msg_iter *lttng_live_msg_iter =
		viewer_connection->lttng_live_msg_iter;
	BT_SOCKET sock = viewer_connection->control_sock;
	ssize_t ret;

	for (;;) {
		ret = bt_socket_send_nosigpipe(sock, buf, len);
		if (ret == BT_SOCKET_ERROR && bt_socket_interrupted()) {
			if (!viewer_connection->in_query &&
					lttng_live_graph_is_canceled(lttng_live_msg_iter)) {
				break;
			} else {
				continue;
			}
		} else {
			break;
		}
	}
	return ret;
}

static
int parse_url(struct live_viewer_connection *viewer_connection)
{
	char error_buf[256] = { 0 };
	bt_self_component *self_comp = viewer_connection->self_comp;
	bt_self_component_class *self_comp_class = viewer_connection->self_comp_class;
	struct bt_common_lttng_live_url_parts lttng_live_url_parts = { 0 };
	int ret = -1;
	const char *path = viewer_connection->url->str;

	if (!path) {
		goto end;
	}

	lttng_live_url_parts = bt_common_parse_lttng_live_url(path, error_buf,
		sizeof(error_buf));
	if (!lttng_live_url_parts.proto) {
		BT_COMP_OR_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp,
			self_comp_class,"Invalid LTTng live URL format: %s",
			error_buf);
		goto end;
	}

	viewer_connection->relay_hostname = lttng_live_url_parts.hostname;
	lttng_live_url_parts.hostname = NULL;

	if (lttng_live_url_parts.port >= 0) {
		viewer_connection->port = lttng_live_url_parts.port;
	} else {
		viewer_connection->port = LTTNG_DEFAULT_NETWORK_VIEWER_PORT;
	}

	viewer_connection->target_hostname = lttng_live_url_parts.target_hostname;
	lttng_live_url_parts.target_hostname = NULL;

	if (lttng_live_url_parts.session_name) {
		viewer_connection->session_name = lttng_live_url_parts.session_name;
		lttng_live_url_parts.session_name = NULL;
	}

	BT_COMP_LOGI("Connecting to hostname : %s, port : %d, "
		"target hostname : %s, session name : %s, proto : %s",
		viewer_connection->relay_hostname->str,
		viewer_connection->port,
		!viewer_connection->target_hostname ?
		"<none>" : viewer_connection->target_hostname->str,
		!viewer_connection->session_name ?
		"<none>" : viewer_connection->session_name->str,
		lttng_live_url_parts.proto->str);
	ret = 0;

end:
	bt_common_destroy_lttng_live_url_parts(&lttng_live_url_parts);
	return ret;
}

static
int lttng_live_handshake(struct live_viewer_connection *viewer_connection)
{
	struct lttng_viewer_cmd cmd;
	struct lttng_viewer_connect connect;
	bt_self_component_class *self_comp_class = viewer_connection->self_comp_class;
	bt_self_component *self_comp = viewer_connection->self_comp;
	const size_t cmd_buf_len = sizeof(cmd) + sizeof(connect);
	char cmd_buf[cmd_buf_len];
	int ret;
	ssize_t ret_len;

	cmd.cmd = htobe32(LTTNG_VIEWER_CONNECT);
	cmd.data_size = htobe64((uint64_t) sizeof(connect));
	cmd.cmd_version = htobe32(0);

	connect.viewer_session_id = -1ULL;	/* will be set on recv */
	connect.major = htobe32(LTTNG_LIVE_MAJOR);
	connect.minor = htobe32(LTTNG_LIVE_MINOR);
	connect.type = htobe32(LTTNG_VIEWER_CLIENT_COMMAND);

	/*
	 * Merge the cmd and connection request to prevent a write-write
	 * sequence on the TCP socket. Otherwise, a delayed ACK will prevent the
	 * second write to be performed quickly in presence of Nagle's algorithm
	 */
	memcpy(cmd_buf, &cmd, sizeof(cmd));
	memcpy(cmd_buf + sizeof(cmd), &connect, sizeof(connect));
	ret_len = lttng_live_send(viewer_connection, &cmd_buf, cmd_buf_len);
	if (ret_len == BT_SOCKET_ERROR) {
		BT_COMP_OR_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp,
			self_comp_class, "Error sending version: %s",
			bt_socket_errormsg());
		goto error;
	}

	BT_ASSERT(ret_len == cmd_buf_len);

	ret_len = lttng_live_recv(viewer_connection, &connect, sizeof(connect));
	if (ret_len == 0) {
		BT_COMP_OR_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp,
			self_comp_class, "Remote side has closed connection");
		goto error;
	}
	if (ret_len == BT_SOCKET_ERROR) {
		BT_COMP_OR_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp,
			self_comp_class, "Error receiving version: %s",
			bt_socket_errormsg());
		goto error;
	}
	BT_ASSERT(ret_len == sizeof(connect));

	BT_COMP_LOGI("Received viewer session ID : %" PRIu64,
			(uint64_t) be64toh(connect.viewer_session_id));
	BT_COMP_LOGI("Relayd version : %u.%u", be32toh(connect.major),
			be32toh(connect.minor));

	if (LTTNG_LIVE_MAJOR != be32toh(connect.major)) {
		BT_COMP_OR_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp,
			self_comp_class, "Incompatible lttng-relayd protocol");
		goto error;
	}
	/* Use the smallest protocol version implemented. */
	if (LTTNG_LIVE_MINOR > be32toh(connect.minor)) {
		viewer_connection->minor =  be32toh(connect.minor);
	} else {
		viewer_connection->minor =  LTTNG_LIVE_MINOR;
	}
	viewer_connection->major = LTTNG_LIVE_MAJOR;
	ret = 0;
	return ret;

error:
	return -1;
}

static
int lttng_live_connect_viewer(struct live_viewer_connection *viewer_connection)
{
	struct hostent *host;
	struct sockaddr_in server_addr;
	bt_self_component_class *self_comp_class = viewer_connection->self_comp_class;
	bt_self_component *self_comp = viewer_connection->self_comp;
	int ret;

	if (parse_url(viewer_connection)) {
		BT_COMP_OR_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp,
			self_comp_class, "Failed to parse URL");
		goto error;
	}

	host = gethostbyname(viewer_connection->relay_hostname->str);
	if (!host) {
		BT_COMP_OR_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp,
			self_comp_class, "Cannot lookup hostname: hostname=\"%s\"",
			viewer_connection->relay_hostname->str);
		goto error;
	}

	if ((viewer_connection->control_sock = socket(AF_INET, SOCK_STREAM, 0)) == BT_INVALID_SOCKET) {
		BT_COMP_OR_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp,
			self_comp_class, "Socket creation failed: %s", bt_socket_errormsg());
		goto error;
	}

	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(viewer_connection->port);
	server_addr.sin_addr = *((struct in_addr *) host->h_addr);
	memset(&(server_addr.sin_zero), 0, 8);

	if (connect(viewer_connection->control_sock, (struct sockaddr *) &server_addr,
				sizeof(struct sockaddr)) == BT_SOCKET_ERROR) {
		BT_COMP_OR_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp,
			self_comp_class, "Connection failed: %s",
			bt_socket_errormsg());
		goto error;
	}
	if (lttng_live_handshake(viewer_connection)) {
		BT_COMP_OR_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp,
			self_comp_class, "Viewer handshake failed");
		goto error;
	}

	ret = 0;

	return ret;

error:
	if (viewer_connection->control_sock != BT_INVALID_SOCKET) {
		if (bt_socket_close(viewer_connection->control_sock) == BT_SOCKET_ERROR) {
			BT_COMP_OR_COMP_CLASS_LOGE(self_comp, self_comp_class,
				"Error closing socket: %s", bt_socket_errormsg());
		}
	}
	viewer_connection->control_sock = BT_INVALID_SOCKET;
	return -1;
}

static
void lttng_live_disconnect_viewer(
		struct live_viewer_connection *viewer_connection)
{
	if (viewer_connection->control_sock == BT_INVALID_SOCKET) {
		return;
	}
	if (bt_socket_close(viewer_connection->control_sock) == BT_SOCKET_ERROR) {
		BT_COMP_LOGE("Error closing socket: %s",
			bt_socket_errormsg());
		viewer_connection->control_sock = BT_INVALID_SOCKET;
	}
}

static
int list_update_session(bt_value *results,
		const struct lttng_viewer_session *session,
		bool *_found, struct live_viewer_connection *viewer_connection)
{
	bt_self_component_class *self_comp_class = viewer_connection->self_comp_class;
	bt_self_component *self_comp = viewer_connection->self_comp;
	int ret = 0;
	uint64_t i, len;
	bt_value *map = NULL;
	bt_value *hostname = NULL;
	bt_value *session_name = NULL;
	bt_value *btval = NULL;
	bool found = false;

	len = bt_value_array_get_length(results);
	for (i = 0; i < len; i++) {
		const char *hostname_str = NULL;
		const char *session_name_str = NULL;

		map = bt_value_array_borrow_element_by_index(results, i);
		hostname = bt_value_map_borrow_entry_value(map, "target-hostname");
		if (!hostname) {
			BT_COMP_OR_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp,
				self_comp_class,
				"Error borrowing \"target-hostname\" entry.");
			ret = -1;
			goto end;
		}
		session_name = bt_value_map_borrow_entry_value(map, "session-name");
		if (!session_name) {
			BT_COMP_OR_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp,
				self_comp_class,
				"Error borrowing \"session-name\" entry.");
			ret = -1;
			goto end;
		}
		hostname_str = bt_value_string_get(hostname);
		session_name_str = bt_value_string_get(session_name);

		if (strcmp(session->hostname, hostname_str) == 0
				&& strcmp(session->session_name, session_name_str) == 0) {
			int64_t val;
			uint32_t streams = be32toh(session->streams);
			uint32_t clients = be32toh(session->clients);

			found = true;

			btval = bt_value_map_borrow_entry_value(map, "stream-count");
			if (!btval) {
				BT_COMP_OR_COMP_CLASS_LOGE_APPEND_CAUSE(
					self_comp, self_comp_class,
					"Error borrowing \"stream-count\" entry.");
				ret = -1;
				goto end;
			}
			val = bt_value_integer_unsigned_get(btval);
			/* sum */
			val += streams;
			bt_value_integer_unsigned_set(btval, val);

			btval = bt_value_map_borrow_entry_value(map, "client-count");
			if (!btval) {
				BT_COMP_OR_COMP_CLASS_LOGE_APPEND_CAUSE(
					self_comp, self_comp_class,
					"Error borrowing \"client-count\" entry.");
				ret = -1;
				goto end;
			}
			val = bt_value_integer_unsigned_get(btval);
			/* max */
			val = bt_max_t(int64_t, clients, val);
			bt_value_integer_unsigned_set(btval, val);
		}

		if (found) {
			break;
		}
	}
end:
	*_found = found;
	return ret;
}

static
int list_append_session(bt_value *results,
		GString *base_url,
		const struct lttng_viewer_session *session,
		struct live_viewer_connection *viewer_connection)
{
	int ret = 0;
	bt_self_component_class *self_comp_class = viewer_connection->self_comp_class;
	bt_value_map_insert_entry_status insert_status;
	bt_value_array_append_element_status append_status;
	bt_value *map = NULL;
	GString *url = NULL;
	bool found = false;

	/*
	 * If the session already exists, add the stream count to it,
	 * and do max of client counts.
	 */
	ret = list_update_session(results, session, &found, viewer_connection);
	if (ret || found) {
		goto end;
	}

	map = bt_value_map_create();
	if (!map) {
		BT_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp_class,
			"Error creating map value.");
		ret = -1;
		goto end;
	}

	if (base_url->len < 1) {
		BT_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp_class,
			"Error: base_url length smaller than 1.");
		ret = -1;
		goto end;
	}
	/*
	 * key = "url",
	 * value = <string>,
	 */
	url = g_string_new(base_url->str);
	g_string_append(url, "/host/");
	g_string_append(url, session->hostname);
	g_string_append_c(url, '/');
	g_string_append(url, session->session_name);

	insert_status = bt_value_map_insert_string_entry(map, "url", url->str);
	if (insert_status != BT_VALUE_MAP_INSERT_ENTRY_STATUS_OK) {
		BT_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp_class,
			"Error inserting \"url\" entry.");
		ret = -1;
		goto end;
	}

	/*
	 * key = "target-hostname",
	 * value = <string>,
	 */
	insert_status = bt_value_map_insert_string_entry(map, "target-hostname",
		session->hostname);
	if (insert_status != BT_VALUE_MAP_INSERT_ENTRY_STATUS_OK) {
		BT_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp_class,
			"Error inserting \"target-hostname\" entry.");
		ret = -1;
		goto end;
	}

	/*
	 * key = "session-name",
	 * value = <string>,
	 */
	insert_status = bt_value_map_insert_string_entry(map, "session-name",
		session->session_name);
	if (insert_status != BT_VALUE_MAP_INSERT_ENTRY_STATUS_OK) {
		BT_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp_class,
			"Error inserting \"session-name\" entry.");
		ret = -1;
		goto end;
	}

	/*
	 * key = "timer-us",
	 * value = <integer>,
	 */
	{
		uint32_t live_timer = be32toh(session->live_timer);

		insert_status = bt_value_map_insert_unsigned_integer_entry(
			map, "timer-us", live_timer);
		if (insert_status != BT_VALUE_MAP_INSERT_ENTRY_STATUS_OK) {
			BT_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp_class,
				"Error inserting \"timer-us\" entry.");
			ret = -1;
			goto end;
		}
	}

	/*
	 * key = "stream-count",
	 * value = <integer>,
	 */
	{
		uint32_t streams = be32toh(session->streams);

		insert_status = bt_value_map_insert_unsigned_integer_entry(map,
			"stream-count", streams);
		if (insert_status != BT_VALUE_MAP_INSERT_ENTRY_STATUS_OK) {
			BT_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp_class,
				"Error inserting \"stream-count\" entry.");
			ret = -1;
			goto end;
		}
	}

	/*
	 * key = "client-count",
	 * value = <integer>,
	 */
	{
		uint32_t clients = be32toh(session->clients);

		insert_status = bt_value_map_insert_unsigned_integer_entry(map,
			"client-count", clients);
		if (insert_status != BT_VALUE_MAP_INSERT_ENTRY_STATUS_OK) {
			BT_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp_class,
				"Error inserting \"client-count\" entry.");
			ret = -1;
			goto end;
		}
	}

	append_status = bt_value_array_append_element(results, map);
	if (append_status != BT_VALUE_ARRAY_APPEND_ELEMENT_STATUS_OK) {
		BT_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp_class,
			"Error appending map to results.");
		ret = -1;
	}

end:
	if (url) {
		g_string_free(url, true);
	}
	BT_VALUE_PUT_REF_AND_RESET(map);
	return ret;
}

/*
 * Data structure returned:
 *
 * {
 *   <array> = {
 *     [n] = {
 *       <map> = {
 *         {
 *           key = "url",
 *           value = <string>,
 *         },
 *         {
 *           key = "target-hostname",
 *           value = <string>,
 *         },
 *         {
 *           key = "session-name",
 *           value = <string>,
 *         },
 *         {
 *           key = "timer-us",
 *           value = <integer>,
 *         },
 *         {
 *           key = "stream-count",
 *           value = <integer>,
 *         },
 *         {
 *           key = "client-count",
 *           value = <integer>,
 *         },
 *       },
 *     }
 *   }
 */

BT_HIDDEN
bt_component_class_query_method_status live_viewer_connection_list_sessions(
		struct live_viewer_connection *viewer_connection,
		const bt_value **user_result)
{
	bt_self_component_class *self_comp_class = viewer_connection->self_comp_class;
	bt_component_class_query_method_status status =
		BT_COMPONENT_CLASS_QUERY_METHOD_STATUS_OK;
	bt_value *result = NULL;
	struct lttng_viewer_cmd cmd;
	struct lttng_viewer_list_sessions list;
	uint32_t i, sessions_count;
	ssize_t ret_len;

	result = bt_value_array_create();
	if (!result) {
		BT_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp_class,
			"Error creating array");
		status = BT_COMPONENT_CLASS_QUERY_METHOD_STATUS_MEMORY_ERROR;
		goto error;
	}

	cmd.cmd = htobe32(LTTNG_VIEWER_LIST_SESSIONS);
	cmd.data_size = htobe64((uint64_t) 0);
	cmd.cmd_version = htobe32(0);

	ret_len = lttng_live_send(viewer_connection, &cmd, sizeof(cmd));
	if (ret_len == BT_SOCKET_ERROR) {
		BT_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp_class,
			"Error sending cmd: %s", bt_socket_errormsg());
		status = BT_COMPONENT_CLASS_QUERY_METHOD_STATUS_ERROR;
		goto error;
	}
	BT_ASSERT(ret_len == sizeof(cmd));

	ret_len = lttng_live_recv(viewer_connection, &list, sizeof(list));
	if (ret_len == 0) {
		BT_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp_class,
			"Remote side has closed connection");
		status = BT_COMPONENT_CLASS_QUERY_METHOD_STATUS_ERROR;
		goto error;
	}
	if (ret_len == BT_SOCKET_ERROR) {
		BT_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp_class,
			"Error receiving session list: %s",
			bt_socket_errormsg());
		status = BT_COMPONENT_CLASS_QUERY_METHOD_STATUS_ERROR;
		goto error;
	}
	BT_ASSERT(ret_len == sizeof(list));

	sessions_count = be32toh(list.sessions_count);
	for (i = 0; i < sessions_count; i++) {
		struct lttng_viewer_session lsession;

		ret_len = lttng_live_recv(viewer_connection, &lsession,
			sizeof(lsession));
		if (ret_len == 0) {
			BT_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp_class,
				"Remote side has closed connection");
			status = BT_COMPONENT_CLASS_QUERY_METHOD_STATUS_ERROR;
			goto error;
		}
		if (ret_len == BT_SOCKET_ERROR) {
			BT_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp_class,
				"Error receiving session: %s",
				bt_socket_errormsg());
			status = BT_COMPONENT_CLASS_QUERY_METHOD_STATUS_ERROR;
			goto error;
		}
		BT_ASSERT(ret_len == sizeof(lsession));
		lsession.hostname[LTTNG_VIEWER_HOST_NAME_MAX - 1] = '\0';
		lsession.session_name[LTTNG_VIEWER_NAME_MAX - 1] = '\0';
		if (list_append_session(result, viewer_connection->url,
				&lsession, viewer_connection)) {
			BT_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp_class,
				"Error appending session");
			status = BT_COMPONENT_CLASS_QUERY_METHOD_STATUS_ERROR;
			goto error;
		}
	}

	*user_result = result;
	goto end;
error:
	BT_VALUE_PUT_REF_AND_RESET(result);
end:
	return status;
}

static
int lttng_live_query_session_ids(struct lttng_live_msg_iter *lttng_live_msg_iter)
{
	struct lttng_viewer_cmd cmd;
	struct lttng_viewer_list_sessions list;
	struct lttng_viewer_session lsession;
	uint32_t i, sessions_count;
	ssize_t ret_len;
	uint64_t session_id;
	struct live_viewer_connection *viewer_connection =
		lttng_live_msg_iter->viewer_connection;
	bt_self_component *self_comp = viewer_connection->self_comp;

	cmd.cmd = htobe32(LTTNG_VIEWER_LIST_SESSIONS);
	cmd.data_size = htobe64((uint64_t) 0);
	cmd.cmd_version = htobe32(0);

	ret_len = lttng_live_send(viewer_connection, &cmd, sizeof(cmd));
	if (ret_len == BT_SOCKET_ERROR) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp, "Error sending cmd: %s",
			bt_socket_errormsg());
		goto error;
	}
	BT_ASSERT(ret_len == sizeof(cmd));

	ret_len = lttng_live_recv(viewer_connection, &list, sizeof(list));
	if (ret_len == 0) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Remote side has closed connection");
		goto error;
	}
	if (ret_len == BT_SOCKET_ERROR) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Error receiving session list: %s",
			bt_socket_errormsg());
		goto error;
	}
	BT_ASSERT(ret_len == sizeof(list));

	sessions_count = be32toh(list.sessions_count);
	for (i = 0; i < sessions_count; i++) {
		ret_len = lttng_live_recv(viewer_connection,
				&lsession, sizeof(lsession));
		if (ret_len == 0) {
			BT_COMP_LOGE_APPEND_CAUSE(self_comp,
				"Remote side has closed connection");
			goto error;
		}
		if (ret_len == BT_SOCKET_ERROR) {
			BT_COMP_LOGE_APPEND_CAUSE(self_comp,
				"Error receiving session: %s",
				bt_socket_errormsg());
			goto error;
		}
		BT_ASSERT(ret_len == sizeof(lsession));
		lsession.hostname[LTTNG_VIEWER_HOST_NAME_MAX - 1] = '\0';
		lsession.session_name[LTTNG_VIEWER_NAME_MAX - 1] = '\0';
		session_id = be64toh(lsession.id);

		BT_COMP_LOGI("Adding session %" PRIu64 " hostname: %s session_name: %s",
			session_id, lsession.hostname, lsession.session_name);

		if ((strncmp(lsession.session_name,
			viewer_connection->session_name->str,
			LTTNG_VIEWER_NAME_MAX) == 0) && (strncmp(lsession.hostname,
				viewer_connection->target_hostname->str,
				LTTNG_VIEWER_HOST_NAME_MAX) == 0)) {
			if (lttng_live_add_session(lttng_live_msg_iter, session_id,
					lsession.hostname,
					lsession.session_name)) {
				BT_COMP_LOGE_APPEND_CAUSE(self_comp,
					"Failed to add live session");
				goto error;
			}
		}
	}

	return 0;

error:
	return -1;
}

BT_HIDDEN
int lttng_live_create_viewer_session(
		struct lttng_live_msg_iter *lttng_live_msg_iter)
{
	struct lttng_viewer_cmd cmd;
	struct lttng_viewer_create_session_response resp;
	ssize_t ret_len;
	struct live_viewer_connection *viewer_connection =
		lttng_live_msg_iter->viewer_connection;
	bt_self_component *self_comp = viewer_connection->self_comp;

	cmd.cmd = htobe32(LTTNG_VIEWER_CREATE_SESSION);
	cmd.data_size = htobe64((uint64_t) 0);
	cmd.cmd_version = htobe32(0);

	ret_len = lttng_live_send(viewer_connection, &cmd, sizeof(cmd));
	if (ret_len == BT_SOCKET_ERROR) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp, "Error sending cmd: %s",
			bt_socket_errormsg());
		goto error;
	}
	BT_ASSERT(ret_len == sizeof(cmd));

	ret_len = lttng_live_recv(viewer_connection, &resp, sizeof(resp));
	if (ret_len == 0) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Remote side has closed connection");
		goto error;
	}
	if (ret_len == BT_SOCKET_ERROR) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Error receiving create session reply: %s",
			bt_socket_errormsg());
		goto error;
	}
	BT_ASSERT(ret_len == sizeof(resp));

	if (be32toh(resp.status) != LTTNG_VIEWER_CREATE_SESSION_OK) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Error creating viewer session");
		goto error;
	}
	if (lttng_live_query_session_ids(lttng_live_msg_iter)) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Failed to query live viewer session ids");
		goto error;
	}

	return 0;

error:
	return -1;
}

static
int receive_streams(struct lttng_live_session *session,
		uint32_t stream_count)
{
	ssize_t ret_len;
	uint32_t i;
	struct lttng_live_msg_iter *lttng_live_msg_iter =
		session->lttng_live_msg_iter;
	struct live_viewer_connection *viewer_connection =
		lttng_live_msg_iter->viewer_connection;
	bt_self_component *self_comp = viewer_connection->self_comp;

	BT_COMP_LOGI("Getting %" PRIu32 " new streams:", stream_count);
	for (i = 0; i < stream_count; i++) {
		struct lttng_viewer_stream stream;
		struct lttng_live_stream_iterator *live_stream;
		uint64_t stream_id;
		uint64_t ctf_trace_id;

		ret_len = lttng_live_recv(viewer_connection, &stream, sizeof(stream));
		if (ret_len == 0) {
			BT_COMP_LOGE_APPEND_CAUSE(self_comp,
				"Remote side has closed connection");
			goto error;
		}
		if (ret_len == BT_SOCKET_ERROR) {
			BT_COMP_LOGE_APPEND_CAUSE(self_comp,
				"Error receiving stream");
			goto error;
		}
		BT_ASSERT(ret_len == sizeof(stream));
		stream.path_name[LTTNG_VIEWER_PATH_MAX - 1] = '\0';
		stream.channel_name[LTTNG_VIEWER_NAME_MAX - 1] = '\0';
		stream_id = be64toh(stream.id);
		ctf_trace_id = be64toh(stream.ctf_trace_id);

		if (stream.metadata_flag) {
			BT_COMP_LOGI("    metadata stream %" PRIu64 " : %s/%s",
				stream_id, stream.path_name, stream.channel_name);
			if (lttng_live_metadata_create_stream(session,
					ctf_trace_id, stream_id,
					stream.path_name)) {
				BT_COMP_LOGE_APPEND_CAUSE(self_comp,
					"Error creating metadata stream");

				goto error;
			}
			session->lazy_stream_msg_init = true;
		} else {
			BT_COMP_LOGI("    stream %" PRIu64 " : %s/%s",
				stream_id, stream.path_name, stream.channel_name);
			live_stream = lttng_live_stream_iterator_create(session,
				ctf_trace_id, stream_id);
			if (!live_stream) {
				BT_COMP_LOGE_APPEND_CAUSE(self_comp,
					"Error creating stream");
				goto error;
			}
		}
	}
	return 0;

error:
	return -1;
}

BT_HIDDEN
enum lttng_live_attach_session_status lttng_live_attach_session(
		struct lttng_live_session *session)
{
	struct lttng_viewer_cmd cmd;
	enum lttng_live_attach_session_status attach_status;
	struct lttng_viewer_attach_session_request rq;
	struct lttng_viewer_attach_session_response rp;
	struct lttng_live_msg_iter *lttng_live_msg_iter =
		session->lttng_live_msg_iter;
	struct live_viewer_connection *viewer_connection =
		lttng_live_msg_iter->viewer_connection;
	bt_self_component *self_comp = viewer_connection->self_comp;
	uint64_t session_id = session->id;
	uint32_t streams_count;
	const size_t cmd_buf_len = sizeof(cmd) + sizeof(rq);
	char cmd_buf[cmd_buf_len];
	ssize_t ret_len;

	cmd.cmd = htobe32(LTTNG_VIEWER_ATTACH_SESSION);
	cmd.data_size = htobe64((uint64_t) sizeof(rq));
	cmd.cmd_version = htobe32(0);

	memset(&rq, 0, sizeof(rq));
	rq.session_id = htobe64(session_id);
	// TODO: add cmd line parameter to select seek beginning
	// rq.seek = htobe32(LTTNG_VIEWER_SEEK_BEGINNING);
	rq.seek = htobe32(LTTNG_VIEWER_SEEK_LAST);

	/*
	 * Merge the cmd and connection request to prevent a write-write
	 * sequence on the TCP socket. Otherwise, a delayed ACK will prevent the
	 * second write to be performed quickly in presence of Nagle's algorithm.
	 */
	memcpy(cmd_buf, &cmd, sizeof(cmd));
	memcpy(cmd_buf + sizeof(cmd), &rq, sizeof(rq));
	ret_len = lttng_live_send(viewer_connection, &cmd_buf, cmd_buf_len);
	if (ret_len == BT_SOCKET_ERROR) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Error sending attach request: %s",
			bt_socket_errormsg());
		goto error;
	}

	BT_ASSERT(ret_len == cmd_buf_len);
	ret_len = lttng_live_recv(viewer_connection, &rp, sizeof(rp));
	if (ret_len == 0) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Remote side has closed connection");
		goto error;
	}
	if (ret_len == BT_SOCKET_ERROR) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Error receiving attach response: %s",
			bt_socket_errormsg());
		goto error;
	}
	BT_ASSERT(ret_len == sizeof(rp));

	streams_count = be32toh(rp.streams_count);
	switch(be32toh(rp.status)) {
	case LTTNG_VIEWER_ATTACH_OK:
		break;
	case LTTNG_VIEWER_ATTACH_UNK:
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Session id %" PRIu64 " is unknown", session_id);
		goto error;
	case LTTNG_VIEWER_ATTACH_ALREADY:
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"There is already a viewer attached to this session");
		goto error;
	case LTTNG_VIEWER_ATTACH_NOT_LIVE:
		BT_COMP_LOGE_APPEND_CAUSE(self_comp, "Not a live session");
		goto error;
	case LTTNG_VIEWER_ATTACH_SEEK_ERR:
		BT_COMP_LOGE_APPEND_CAUSE(self_comp, "Wrong seek parameter");
		goto error;
	default:
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Unknown attach return code %u", be32toh(rp.status));
		goto error;
	}

	/* We receive the initial list of streams. */
	if (receive_streams(session, streams_count)) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp, "Error receiving streams");
		goto error;
	}

	session->attached = true;
	session->new_streams_needed = false;

	attach_status = LTTNG_LIVE_ATTACH_SESSION_STATUS_OK;
	goto end;

error:
	attach_status = LTTNG_LIVE_ATTACH_SESSION_STATUS_ERROR;

end:
	return attach_status;
}

BT_HIDDEN
int lttng_live_detach_session(struct lttng_live_session *session)
{
	struct lttng_viewer_cmd cmd;
	struct lttng_viewer_detach_session_request rq;
	struct lttng_viewer_detach_session_response rp;
	ssize_t ret_len;
	struct lttng_live_msg_iter *lttng_live_msg_iter =
		session->lttng_live_msg_iter;
	struct live_viewer_connection *viewer_connection =
		lttng_live_msg_iter->viewer_connection;
	uint64_t session_id = session->id;
	const size_t cmd_buf_len = sizeof(cmd) + sizeof(rq);
	char cmd_buf[cmd_buf_len];

	if (!session->attached) {
		return 0;
	}

	cmd.cmd = htobe32(LTTNG_VIEWER_DETACH_SESSION);
	cmd.data_size = htobe64((uint64_t) sizeof(rq));
	cmd.cmd_version = htobe32(0);

	memset(&rq, 0, sizeof(rq));
	rq.session_id = htobe64(session_id);

	/*
	 * Merge the cmd and connection request to prevent a write-write
	 * sequence on the TCP socket. Otherwise, a delayed ACK will prevent the
	 * second write to be performed quickly in presence of Nagle's algorithm.
	 */
	memcpy(cmd_buf, &cmd, sizeof(cmd));
	memcpy(cmd_buf + sizeof(cmd), &rq, sizeof(rq));
	ret_len = lttng_live_send(viewer_connection, &cmd_buf, cmd_buf_len);
	if (ret_len == BT_SOCKET_ERROR) {
		BT_COMP_LOGE("Error sending detach request: %s",
			bt_socket_errormsg());
		goto error;
	}

	BT_ASSERT(ret_len == cmd_buf_len);
	ret_len = lttng_live_recv(viewer_connection, &rp, sizeof(rp));
	if (ret_len == 0) {
		BT_COMP_LOGE("Remote side has closed connection");
		goto error;
	}
	if (ret_len == BT_SOCKET_ERROR) {
		BT_COMP_LOGE("Error receiving detach response: %s",
			bt_socket_errormsg());
		goto error;
	}
	BT_ASSERT(ret_len == sizeof(rp));

	switch(be32toh(rp.status)) {
	case LTTNG_VIEWER_DETACH_SESSION_OK:
		break;
	case LTTNG_VIEWER_DETACH_SESSION_UNK:
		BT_COMP_LOGW("Session id %" PRIu64 " is unknown", session_id);
		goto error;
	case LTTNG_VIEWER_DETACH_SESSION_ERR:
		BT_COMP_LOGW("Error detaching session id %" PRIu64 "", session_id);
		goto error;
	default:
		BT_COMP_LOGE("Unknown detach return code %u", be32toh(rp.status));
		goto error;
	}

	session->attached = false;

	return 0;

error:
	return -1;
}

BT_HIDDEN
enum lttng_live_get_one_metadata_status lttng_live_get_one_metadata_packet(
		struct lttng_live_trace *trace, FILE *fp, size_t *reply_len)
{
	uint64_t len = 0;
	enum lttng_live_get_one_metadata_status metadata_status;
	struct lttng_viewer_cmd cmd;
	struct lttng_viewer_get_metadata rq;
	struct lttng_viewer_metadata_packet rp;
	char *data = NULL;
	ssize_t ret_len;
	struct lttng_live_session *session = trace->session;
	struct lttng_live_msg_iter *lttng_live_msg_iter =
		session->lttng_live_msg_iter;
	struct lttng_live_metadata *metadata = trace->metadata;
	struct live_viewer_connection *viewer_connection =
		lttng_live_msg_iter->viewer_connection;
	bt_self_component *self_comp = viewer_connection->self_comp;
	const size_t cmd_buf_len = sizeof(cmd) + sizeof(rq);
	char cmd_buf[cmd_buf_len];

	rq.stream_id = htobe64(metadata->stream_id);
	cmd.cmd = htobe32(LTTNG_VIEWER_GET_METADATA);
	cmd.data_size = htobe64((uint64_t) sizeof(rq));
	cmd.cmd_version = htobe32(0);

	/*
	 * Merge the cmd and connection request to prevent a write-write
	 * sequence on the TCP socket. Otherwise, a delayed ACK will prevent the
	 * second write to be performed quickly in presence of Nagle's algorithm.
	 */
	memcpy(cmd_buf, &cmd, sizeof(cmd));
	memcpy(cmd_buf + sizeof(cmd), &rq, sizeof(rq));
	ret_len = lttng_live_send(viewer_connection, &cmd_buf, cmd_buf_len);
	if (ret_len == BT_SOCKET_ERROR) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Error sending get_metadata request: %s",
			bt_socket_errormsg());
		goto error;
	}

	BT_ASSERT(ret_len == cmd_buf_len);
	ret_len = lttng_live_recv(viewer_connection, &rp, sizeof(rp));
	if (ret_len == 0) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Remote side has closed connection");
		goto error;
	}
	if (ret_len == BT_SOCKET_ERROR) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Error receiving get_metadata response: %s",
			bt_socket_errormsg());
		goto error;
	}
	BT_ASSERT(ret_len == sizeof(rp));

	switch (be32toh(rp.status)) {
		case LTTNG_VIEWER_METADATA_OK:
			BT_COMP_LOGD("Received get_metadata response: ok");
			break;
		case LTTNG_VIEWER_NO_NEW_METADATA:
			BT_COMP_LOGD("Received get_metadata response: no new");
			metadata_status = LTTNG_LIVE_GET_ONE_METADATA_STATUS_END;
			goto end;
		case LTTNG_VIEWER_METADATA_ERR:
			/*
			 * The Relayd cannot find this stream id. Maybe its
			 * gone already. This can happen in short lived UST app
			 * in a per-pid session.
			 */
			BT_COMP_LOGD("Received get_metadata response: error");
			metadata_status = LTTNG_LIVE_GET_ONE_METADATA_STATUS_CLOSED;
			goto end;
		default:
			BT_COMP_LOGE_APPEND_CAUSE(self_comp,
				"Received get_metadata response: unknown");
			goto error;
	}

	len = be64toh(rp.len);
	BT_COMP_LOGD("Writing %" PRIu64" bytes to metadata", len);
	if (len <= 0) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Erroneous response length");
		goto error;
	}

	data = calloc(1, len);
	if (!data) {
		BT_COMP_LOGE_APPEND_CAUSE_ERRNO(self_comp,
			"Failed to allocate data buffer", ".");
		goto error;
	}
	ret_len = lttng_live_recv(viewer_connection, data, len);
	if (ret_len == 0) {
		BT_COMP_LOGI("Remote side has closed connection");
		goto error_free_data;
	}
	if (ret_len == BT_SOCKET_ERROR) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Error receiving trace packet: %s", bt_socket_errormsg());
		goto error_free_data;
	}
	BT_ASSERT(ret_len == len);

	do {
		ret_len = fwrite(data, 1, len, fp);
	} while (ret_len < 0 && errno == EINTR);
	if (ret_len < 0) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Writing in the metadata file stream");
		goto error_free_data;
	}
	BT_ASSERT(ret_len == len);
	free(data);
	*reply_len = len;
	metadata_status = LTTNG_LIVE_GET_ONE_METADATA_STATUS_OK;

	goto end;

error_free_data:
	free(data);
error:
	metadata_status = LTTNG_LIVE_GET_ONE_METADATA_STATUS_ERROR;

end:
	return metadata_status;
}

/*
 * Assign the fields from a lttng_viewer_index to a packet_index.
 */
static
void lttng_index_to_packet_index(struct lttng_viewer_index *lindex,
		struct packet_index *pindex)
{
	BT_ASSERT(lindex);
	BT_ASSERT(pindex);

	pindex->offset = be64toh(lindex->offset);
	pindex->packet_size = be64toh(lindex->packet_size);
	pindex->content_size = be64toh(lindex->content_size);
	pindex->ts_cycles.timestamp_begin = be64toh(lindex->timestamp_begin);
	pindex->ts_cycles.timestamp_end = be64toh(lindex->timestamp_end);
	pindex->events_discarded = be64toh(lindex->events_discarded);
}

BT_HIDDEN
enum lttng_live_iterator_status lttng_live_get_next_index(
		struct lttng_live_msg_iter *lttng_live_msg_iter,
		struct lttng_live_stream_iterator *stream,
		struct packet_index *index)
{
	struct lttng_viewer_cmd cmd;
	struct lttng_viewer_get_next_index rq;
	struct lttng_viewer_index rp;
	enum lttng_live_iterator_status retstatus = LTTNG_LIVE_ITERATOR_STATUS_OK;
	struct live_viewer_connection *viewer_connection =
		lttng_live_msg_iter->viewer_connection;
	bt_self_component *self_comp = viewer_connection->self_comp;
	struct lttng_live_trace *trace = stream->trace;
	const size_t cmd_buf_len = sizeof(cmd) + sizeof(rq);
	char cmd_buf[cmd_buf_len];
	uint32_t flags, status;
	ssize_t ret_len;

	cmd.cmd = htobe32(LTTNG_VIEWER_GET_NEXT_INDEX);
	cmd.data_size = htobe64((uint64_t) sizeof(rq));
	cmd.cmd_version = htobe32(0);


	memset(&rq, 0, sizeof(rq));
	rq.stream_id = htobe64(stream->viewer_stream_id);

	/*
	 * Merge the cmd and connection request to prevent a write-write
	 * sequence on the TCP socket. Otherwise, a delayed ACK will prevent the
	 * second write to be performed quickly in presence of Nagle's algorithm.
	 */
	memcpy(cmd_buf, &cmd, sizeof(cmd));
	memcpy(cmd_buf + sizeof(cmd), &rq, sizeof(rq));
	ret_len = lttng_live_send(viewer_connection, &cmd_buf, cmd_buf_len);
	if (ret_len == BT_SOCKET_ERROR) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Error sending get_next_index request: %s",
			bt_socket_errormsg());
		goto error;
	}

	BT_ASSERT(ret_len == cmd_buf_len);
	ret_len = lttng_live_recv(viewer_connection, &rp, sizeof(rp));
	if (ret_len == 0) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Remote side has closed connection");
		goto error;
	}
	if (ret_len == BT_SOCKET_ERROR) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Error receiving get_next_index response: %s",
			bt_socket_errormsg());
		goto error;
	}
	BT_ASSERT(ret_len == sizeof(rp));

	flags = be32toh(rp.flags);
	status = be32toh(rp.status);

	switch (status) {
	case LTTNG_VIEWER_INDEX_INACTIVE:
	{
		uint64_t ctf_stream_class_id;

		BT_COMP_LOGD("Received get_next_index response: inactive");
		memset(index, 0, sizeof(struct packet_index));
		index->ts_cycles.timestamp_end = be64toh(rp.timestamp_end);
		stream->current_inactivity_ts = index->ts_cycles.timestamp_end;
		ctf_stream_class_id = be64toh(rp.stream_id);
		if (stream->ctf_stream_class_id != -1ULL) {
			BT_ASSERT(stream->ctf_stream_class_id ==
				ctf_stream_class_id);
		} else {
			stream->ctf_stream_class_id = ctf_stream_class_id;
		}
		stream->state = LTTNG_LIVE_STREAM_QUIESCENT;
		break;
	}
	case LTTNG_VIEWER_INDEX_OK:
	{
		uint64_t ctf_stream_class_id;

		BT_COMP_LOGD("Received get_next_index response: OK");
		lttng_index_to_packet_index(&rp, index);
		ctf_stream_class_id = be64toh(rp.stream_id);
		if (stream->ctf_stream_class_id != -1ULL) {
			BT_ASSERT(stream->ctf_stream_class_id ==
				ctf_stream_class_id);
		} else {
			stream->ctf_stream_class_id = ctf_stream_class_id;
		}

		stream->state = LTTNG_LIVE_STREAM_ACTIVE_DATA;

		if (flags & LTTNG_VIEWER_FLAG_NEW_METADATA) {
			BT_COMP_LOGD("Received get_next_index response: new metadata needed");
			trace->new_metadata_needed = true;
		}
		if (flags & LTTNG_VIEWER_FLAG_NEW_STREAM) {
			BT_COMP_LOGD("Received get_next_index response: new streams needed");
			lttng_live_need_new_streams(lttng_live_msg_iter);
		}
		break;
	}
	case LTTNG_VIEWER_INDEX_RETRY:
		BT_COMP_LOGD("Received get_next_index response: retry");
		memset(index, 0, sizeof(struct packet_index));
		retstatus = LTTNG_LIVE_ITERATOR_STATUS_AGAIN;
		stream->state = LTTNG_LIVE_STREAM_ACTIVE_NO_DATA;
		goto end;
	case LTTNG_VIEWER_INDEX_HUP:
		BT_COMP_LOGD("Received get_next_index response: stream hung up");
		memset(index, 0, sizeof(struct packet_index));
		index->offset = EOF;
		retstatus = LTTNG_LIVE_ITERATOR_STATUS_END;
		stream->state = LTTNG_LIVE_STREAM_EOF;
		stream->has_stream_hung_up = true;
		break;
	case LTTNG_VIEWER_INDEX_ERR:
		BT_COMP_LOGD("Received get_next_index response: error");
		memset(index, 0, sizeof(struct packet_index));
		stream->state = LTTNG_LIVE_STREAM_ACTIVE_NO_DATA;
		goto error;
	default:
		BT_COMP_LOGD("Received get_next_index response: unknown value");
		memset(index, 0, sizeof(struct packet_index));
		stream->state = LTTNG_LIVE_STREAM_ACTIVE_NO_DATA;
		goto error;
	}
end:
	return retstatus;

error:
	if (lttng_live_graph_is_canceled(lttng_live_msg_iter)) {
		retstatus = LTTNG_LIVE_ITERATOR_STATUS_AGAIN;
	} else {
		retstatus = LTTNG_LIVE_ITERATOR_STATUS_ERROR;
	}
	return retstatus;
}

BT_HIDDEN
enum bt_msg_iter_medium_status lttng_live_get_stream_bytes(
		struct lttng_live_msg_iter *lttng_live_msg_iter,
		struct lttng_live_stream_iterator *stream, uint8_t *buf,
		uint64_t offset, uint64_t req_len, uint64_t *recv_len)
{
	enum bt_msg_iter_medium_status retstatus = BT_MSG_ITER_MEDIUM_STATUS_OK;
	struct lttng_viewer_trace_packet rp;
	struct lttng_viewer_cmd cmd;
	struct lttng_viewer_get_packet rq;
	struct live_viewer_connection *viewer_connection =
		lttng_live_msg_iter->viewer_connection;
	bt_self_component *self_comp = viewer_connection->self_comp;
	struct lttng_live_trace *trace = stream->trace;
	const size_t cmd_buf_len = sizeof(cmd) + sizeof(rq);
	char cmd_buf[cmd_buf_len];
	uint32_t flags, status;
	ssize_t ret_len;

	BT_COMP_LOGD("lttng_live_get_stream_bytes: offset=%" PRIu64 ", req_len=%" PRIu64,
			offset, req_len);
	cmd.cmd = htobe32(LTTNG_VIEWER_GET_PACKET);
	cmd.data_size = htobe64((uint64_t) sizeof(rq));
	cmd.cmd_version = htobe32(0);

	memset(&rq, 0, sizeof(rq));
	rq.stream_id = htobe64(stream->viewer_stream_id);
	rq.offset = htobe64(offset);
	rq.len = htobe32(req_len);

	/*
	 * Merge the cmd and connection request to prevent a write-write
	 * sequence on the TCP socket. Otherwise, a delayed ACK will prevent the
	 * second write to be performed quickly in presence of Nagle's algorithm.
	 */
	memcpy(cmd_buf, &cmd, sizeof(cmd));
	memcpy(cmd_buf + sizeof(cmd), &rq, sizeof(rq));
	ret_len = lttng_live_send(viewer_connection, &cmd_buf, cmd_buf_len);
	if (ret_len == BT_SOCKET_ERROR) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Error sending get_data_packet request: %s",
			bt_socket_errormsg());
		goto error;
	}

	BT_ASSERT(ret_len == cmd_buf_len);
	ret_len = lttng_live_recv(viewer_connection, &rp, sizeof(rp));
	if (ret_len == 0) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Remote side has closed connection");
		goto error;
	}
	if (ret_len == BT_SOCKET_ERROR) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Socket error receiving get_data response: %s",
			bt_socket_errormsg());
		goto error;
	}
	if (ret_len != sizeof(rp)) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp, "get_data_packet: "
			"expected %zu, received %zd", sizeof(rp), ret_len);
		goto error;
	}

	flags = be32toh(rp.flags);
	status = be32toh(rp.status);

	switch (status) {
	case LTTNG_VIEWER_GET_PACKET_OK:
		req_len = be32toh(rp.len);
		BT_COMP_LOGD("Received get_data_packet response: Ok, "
			"packet size : %" PRIu64 "", req_len);
		break;
	case LTTNG_VIEWER_GET_PACKET_RETRY:
		/* Unimplemented by relay daemon */
		BT_COMP_LOGD("Received get_data_packet response: retry");
		retstatus = BT_MSG_ITER_MEDIUM_STATUS_AGAIN;
		goto end;
	case LTTNG_VIEWER_GET_PACKET_ERR:
		if (flags & LTTNG_VIEWER_FLAG_NEW_METADATA) {
			BT_COMP_LOGD("get_data_packet: new metadata needed, try again later");
			trace->new_metadata_needed = true;
		}
		if (flags & LTTNG_VIEWER_FLAG_NEW_STREAM) {
			BT_COMP_LOGD("get_data_packet: new streams needed, try again later");
			lttng_live_need_new_streams(lttng_live_msg_iter);
		}
		if (flags & (LTTNG_VIEWER_FLAG_NEW_METADATA
				| LTTNG_VIEWER_FLAG_NEW_STREAM)) {
			retstatus = BT_MSG_ITER_MEDIUM_STATUS_AGAIN;
			goto end;
		}
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Received get_data_packet response: error");
		goto error;
	case LTTNG_VIEWER_GET_PACKET_EOF:
		retstatus = BT_MSG_ITER_MEDIUM_STATUS_EOF;
		goto end;
	default:
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Received get_data_packet response: unknown");
		goto error;
	}

	if (req_len == 0) {
		goto error;
	}

	ret_len = lttng_live_recv(viewer_connection, buf, req_len);
	if (ret_len == 0) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Remote side has closed connection");
		goto error;
	}
	if (ret_len == BT_SOCKET_ERROR) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Error receiving trace packet: %s",
			bt_socket_errormsg());
		goto error;
	}
	BT_ASSERT(ret_len == req_len);
	*recv_len = ret_len;
end:
	return retstatus;

error:
	if (lttng_live_graph_is_canceled(lttng_live_msg_iter)) {
		retstatus = BT_MSG_ITER_MEDIUM_STATUS_AGAIN;
	} else {
		retstatus = BT_MSG_ITER_MEDIUM_STATUS_ERROR;
	}
	return retstatus;
}

/*
 * Request new streams for a session.
 */
BT_HIDDEN
enum lttng_live_iterator_status lttng_live_get_new_streams(
		struct lttng_live_session *session)
{
	enum lttng_live_iterator_status status =
		LTTNG_LIVE_ITERATOR_STATUS_OK;
	struct lttng_viewer_cmd cmd;
	struct lttng_viewer_new_streams_request rq;
	struct lttng_viewer_new_streams_response rp;
	struct lttng_live_msg_iter *lttng_live_msg_iter =
		session->lttng_live_msg_iter;
	struct live_viewer_connection *viewer_connection =
		lttng_live_msg_iter->viewer_connection;
	bt_self_component *self_comp = viewer_connection->self_comp;
	uint32_t streams_count;
	const size_t cmd_buf_len = sizeof(cmd) + sizeof(rq);
	char cmd_buf[cmd_buf_len];
	ssize_t ret_len;

	if (!session->new_streams_needed) {
		return LTTNG_LIVE_ITERATOR_STATUS_OK;
	}

	cmd.cmd = htobe32(LTTNG_VIEWER_GET_NEW_STREAMS);
	cmd.data_size = htobe64((uint64_t) sizeof(rq));
	cmd.cmd_version = htobe32(0);

	memset(&rq, 0, sizeof(rq));
	rq.session_id = htobe64(session->id);

	/*
	 * Merge the cmd and connection request to prevent a write-write
	 * sequence on the TCP socket. Otherwise, a delayed ACK will prevent the
	 * second write to be performed quickly in presence of Nagle's algorithm.
	 */
	memcpy(cmd_buf, &cmd, sizeof(cmd));
	memcpy(cmd_buf + sizeof(cmd), &rq, sizeof(rq));
	ret_len = lttng_live_send(viewer_connection, &cmd_buf, cmd_buf_len);
	if (ret_len == BT_SOCKET_ERROR) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Error sending get_new_streams request: %s",
			bt_socket_errormsg());
		goto error;
	}

	BT_ASSERT(ret_len == cmd_buf_len);
	ret_len = lttng_live_recv(viewer_connection, &rp, sizeof(rp));
	if (ret_len == 0) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Remote side has closed connection");
		goto error;
	}
	if (ret_len == BT_SOCKET_ERROR) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Error receiving get_new_streams response");
		goto error;
	}
	BT_ASSERT(ret_len == sizeof(rp));

	streams_count = be32toh(rp.streams_count);

	switch(be32toh(rp.status)) {
	case LTTNG_VIEWER_NEW_STREAMS_OK:
		session->new_streams_needed = false;
		break;
	case LTTNG_VIEWER_NEW_STREAMS_NO_NEW:
		session->new_streams_needed = false;
		goto end;
	case LTTNG_VIEWER_NEW_STREAMS_HUP:
		session->new_streams_needed = false;
		session->closed = true;
		status = LTTNG_LIVE_ITERATOR_STATUS_END;
		goto end;
	case LTTNG_VIEWER_NEW_STREAMS_ERR:
		BT_COMP_LOGD("Received get_new_streams response: error");
		goto error;
	default:
		BT_COMP_LOGE_APPEND_CAUSE(self_comp,
			"Received get_new_streams response: Unknown:"
			"return code %u", be32toh(rp.status));
		goto error;
	}

	if (receive_streams(session, streams_count)) {
		BT_COMP_LOGE_APPEND_CAUSE(self_comp, "Error receiving streams");
		goto error;
	}
end:
	return status;

error:
	if (lttng_live_graph_is_canceled(lttng_live_msg_iter)) {
		status = LTTNG_LIVE_ITERATOR_STATUS_AGAIN;
	} else {
		status = LTTNG_LIVE_ITERATOR_STATUS_ERROR;
		BT_COMP_LOGE("Error receiving streams.");
	}
	return status;
}

BT_HIDDEN
struct live_viewer_connection *live_viewer_connection_create(
		const char *url, bool in_query,
		struct lttng_live_msg_iter *lttng_live_msg_iter,
		bt_self_component *self_comp,
		bt_self_component_class *self_comp_class,
		bt_logging_level log_level)
{
	struct live_viewer_connection *viewer_connection;

	viewer_connection = g_new0(struct live_viewer_connection, 1);

	if (bt_socket_init(log_level) != 0) {
		BT_COMP_OR_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp,
			self_comp_class, "Failed to init socket");
		goto error;
	}

	viewer_connection->log_level = log_level;

	viewer_connection->self_comp = self_comp;
	viewer_connection->self_comp_class = self_comp_class;

	viewer_connection->control_sock = BT_INVALID_SOCKET;
	viewer_connection->port = -1;
	viewer_connection->in_query = in_query;
	viewer_connection->lttng_live_msg_iter = lttng_live_msg_iter;
	viewer_connection->url = g_string_new(url);
	if (!viewer_connection->url) {
		BT_COMP_OR_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp,
			self_comp_class, "Failed to allocate URL buffer");
		goto error;
	}

	BT_COMP_LOGI("Establishing connection to url \"%s\"...", url);
	if (lttng_live_connect_viewer(viewer_connection)) {
		BT_COMP_OR_COMP_CLASS_LOGE_APPEND_CAUSE(self_comp,
			self_comp_class, "Failed to establish connection: "
			"url=\"%s\"", url);
		goto error;
	}
	BT_COMP_LOGI("Connection to url \"%s\" is established", url);
	return viewer_connection;

error:
	if (viewer_connection) {
		live_viewer_connection_destroy(viewer_connection);
	}

	return NULL;
}

BT_HIDDEN
void live_viewer_connection_destroy(
		struct live_viewer_connection *viewer_connection)
{
	BT_COMP_LOGI("Closing connection to url \"%s\"", viewer_connection->url->str);

	if (!viewer_connection) {
		goto end;
	}

	lttng_live_disconnect_viewer(viewer_connection);

	if (viewer_connection->url) {
		g_string_free(viewer_connection->url, true);
	}

	if (viewer_connection->relay_hostname) {
		g_string_free(viewer_connection->relay_hostname, true);
	}

	if (viewer_connection->target_hostname) {
		g_string_free(viewer_connection->target_hostname, true);
	}

	if (viewer_connection->session_name) {
		g_string_free(viewer_connection->session_name, true);
	}

	g_free(viewer_connection);

	bt_socket_fini();

end:
	return;
}
