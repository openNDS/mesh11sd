/**
 * @file peer_backend.c
 * @brief Own the private procd runtime record and its lifetime lock.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#define _GNU_SOURCE
#include "peer.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <unistd.h>

static bool equal(const struct mesh_runtime_data *a, const struct mesh_runtime_data *b)
{
	return a->length == b->length && a->length <= MESH_RUNTIME_BYTES &&
		   memcmp(a->bytes, b->bytes, a->length) == 0;
}

static int name_request(struct mesh_message *request)
{
	if (mesh_message_init(request) != 0) {
		return -1;
	}
	return blobmsg_add_string(&request->blob, "name", MESH_PEER_SERVICE);
}

int mesh_peer_call(struct mesh_peer *peer, const char *object, const char *method,
				struct mesh_message *request, struct mesh_runtime_data *reply)
{
	int status;
	mesh_runtime_data_clear(reply);
	status = peer->transport.invoke(peer->transport.context, object, method, request->blob.head, reply);
	if (status != 0) {
		return status;
	}
	if (reply->length != 0 && !mesh_reply_valid(reply)) {
		return -1;
	}
	return 0;
}

static int owned_read(void *context, struct mesh_runtime_data *output)
{
	static const char *const kinds[] = { "wifi-device", "wifi-iface", "network-interface" };
	struct mesh_peer *peer = context;
	struct mesh_message request;
	struct mesh_message normalized;
	struct mesh_runtime_data reply;
	struct blob_attr *service;
	struct blob_attr *data;
	struct blob_attr *field;
	struct blob_attr *item;
	int remaining;
	size_t count = 0;
	size_t i;
	int result = -1;

	memset(&reply, 0, sizeof(reply));
	mesh_message_init(&normalized);
	if (!peer->initialized || name_request(&request) != 0) {
		return -1;
	}
	if (mesh_peer_call(peer, "service", "list", &request, &reply) != 0 || !mesh_reply_valid(&reply)) {
		goto done;
	}
	service = mesh_root_field(&reply, MESH_PEER_SERVICE, BLOBMSG_TYPE_TABLE);
	if (service == NULL) {
		if (blob_len((struct blob_attr *)reply.bytes) != 0) {
			goto done;
		}
		mesh_runtime_data_clear(output);
		peer->present = false;
		result = 0;
		goto done;
	}
	field = mesh_field(service, "autostart", BLOBMSG_TYPE_BOOL);
	if (field == NULL || blobmsg_get_bool(field)) {
		goto done;
	}
	field = mesh_field(service, "instances", -1);
	if (field != NULL && (blobmsg_type(field) != BLOBMSG_TYPE_TABLE || blobmsg_data_len(field) != 0)) {
		goto done;
	}
	data = mesh_field(service, "data", BLOBMSG_TYPE_TABLE);
	field = mesh_field(data, MESH_PEER_OWNER, BLOBMSG_TYPE_STRING);
	if (field == NULL || strcmp(blobmsg_get_string(field), peer->owner) != 0) {
		goto done;
	}
	blobmsg_for_each_attr(item, data, remaining) {
		count++;
	}
	field = mesh_field(data, "firewall", BLOBMSG_TYPE_ARRAY);
	if (count != (field != NULL ? 5U : 4U)) {
		goto done;
	}
	for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
		field = mesh_field(data, kinds[i], BLOBMSG_TYPE_TABLE);
		if (field == NULL || blobmsg_add_blob(&normalized.blob, field) != 0) {
			goto done;
		}
	}
	field = mesh_field(data, "firewall", BLOBMSG_TYPE_ARRAY);
	if (field != NULL && blobmsg_add_blob(&normalized.blob, field) != 0) {
		goto done;
	}
	result = mesh_message_copy(&normalized, output);
	if (result == 0) {
		peer->present = true;
	}
done:
	mesh_message_clear(&request);
	mesh_message_clear(&normalized);
	mesh_runtime_data_clear(&reply);
	return result;
}

static int owned_replace(void *context, const struct mesh_runtime_data *expected,
						 const struct mesh_runtime_data *replacement)
{
	struct mesh_peer *peer = context;
	struct mesh_runtime_data current;
	struct mesh_runtime_data after;
	struct mesh_runtime_data reply;
	struct mesh_message request;
	struct blob_attr *item;
	struct blob_attr *root;
	void *data;
	int remaining;
	int status;
	int result = -1;
	const char *method;

	memset(&current, 0, sizeof(current));
	memset(&after, 0, sizeof(after));
	memset(&reply, 0, sizeof(reply));
	memset(&request, 0, sizeof(request));
	if (peer->session_lock < 0 || owned_read(peer, &current) != 0 || !equal(expected, &current) ||
		(replacement->length != 0 && !mesh_reply_valid(replacement))) {
		goto done;
	}
	if (name_request(&request) != 0) {
		goto done;
	}
	if (replacement->length == 0) {
		if (current.length == 0) {
			result = 0;
			goto done;
		}
		method = "delete";
	} else {
		method = current.length == 0 ? "set" : "set_data";
		if (current.length == 0 && blobmsg_add_u8(&request.blob, "autostart", false) != 0) {
			goto done;
		}
		data = blobmsg_open_table(&request.blob, "data");
		if (data == NULL || blobmsg_add_string(&request.blob, MESH_PEER_OWNER, peer->owner) != 0) {
			goto done;
		}
		root = (struct blob_attr *)replacement->bytes;
		blob_for_each_attr(item, root, remaining) {
			if (blobmsg_add_blob(&request.blob, item) != 0) {
				goto done;
			}
		}
		blobmsg_close_table(&request.blob, data);
	}
	/* Lifetime lock + private procd namespace + pre/post ownership/read-back.
	 * procd has no cross-service CAS; privileged noncooperating writers cannot
	 * be made atomic. No other service or committed file is replaced. */
	status = peer->transport.invoke(peer->transport.context, "service", method, request.blob.head, &reply);
	if (owned_read(peer, &after) == 0) {
		if (equal(&after, replacement)) {
			result = 0;
		} else if (status != 0 && equal(&after, expected)) {
			result = -1;
		} else {
			result = MESH_PEER_CALL_UNCERTAIN;
		}
	} else {
		result = MESH_PEER_CALL_UNCERTAIN;
	}
done:
	mesh_runtime_data_clear(&current);
	mesh_runtime_data_clear(&after);
	mesh_runtime_data_clear(&reply);
	mesh_message_clear(&request);
	return result;
}

int mesh_peer_reload(void *context);
int mesh_peer_verify(void *context, const struct mesh_plan *plan);

static int owner_file(struct mesh_peer *peer)
{
	unsigned char random[16];
	static const char digits[] = "0123456789abcdef";
	struct stat metadata;
	char existing[34];
	size_t length;
	size_t i;
	int fd;

	fd = openat(peer->directory, "owner", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd >= 0) {
		if (fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_nlink != 1 ||
			(metadata.st_mode & 0777) != 0600 || metadata.st_uid != geteuid()) {
			close(fd);
			return -1;
		}
		close(fd);
		if (mesh_read_bounded_at(peer->directory, "owner", existing, sizeof(existing), &length, NULL) != 0 || length != 32) {
			return -1;
		}
		for (i = 0; i < length; i++) {
			if (strchr(digits, existing[i]) == NULL || existing[i] == '\0') {
				return -1;
			}
		}
		memcpy(peer->owner, existing, length);
		peer->owner[length] = '\0';
		return 0;
	}
	if (errno != ENOENT || getrandom(random, sizeof(random), 0) != sizeof(random)) {
		return -1;
	}
	for (i = 0; i < sizeof(random); i++) {
		peer->owner[2 * i] = digits[random[i] >> 4];
		peer->owner[2 * i + 1] = digits[random[i] & 15];
	}
	peer->owner[32] = '\0';
	fd = openat(peer->directory, "owner", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) {
		return -1;
	}
	if (write(fd, peer->owner, 32) != 32 || fsync(fd) != 0) {
		close(fd);
		return -1;
	}
	if (close(fd) != 0 || fsync(peer->directory) != 0) {
		return -1;
	}
	return 0;
}

int mesh_peer_open(struct mesh_peer *peer, int directory, const struct mesh_peer_transport *transport)
{
	struct stat metadata;
	struct mesh_runtime_data current;
	int fd;

	if (peer == NULL || transport == NULL || transport->invoke == NULL || transport->now_ms == NULL ||
		transport->wait_ms == NULL || transport->interfaces == NULL || transport->wiphy == NULL ||
		transport->verify_bridge == NULL || fstat(directory, &metadata) != 0 || !S_ISDIR(metadata.st_mode) ||
		metadata.st_uid != geteuid() || (metadata.st_mode & 0777) != 0700) {
		return -1;
	}
	memset(peer, 0, sizeof(*peer));
	peer->session_lock = -1;
	peer->directory = directory;
	peer->transport = *transport;
	fd = openat(directory, "session.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) {
		return -1;
	}
	if (fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_nlink != 1 ||
		metadata.st_uid != geteuid() || (metadata.st_mode & 0777) != 0600 || flock(fd, LOCK_EX | LOCK_NB) != 0) {
		close(fd);
		return -1;
	}
	peer->session_lock = fd;
	if (owner_file(peer) != 0) {
		mesh_peer_close(peer);
		return -1;
	}
	peer->initialized = true;
	peer->ops.context = peer;
	peer->ops.read = owned_read;
	peer->ops.replace = owned_replace;
	peer->ops.reload = mesh_peer_reload;
	peer->ops.verify = mesh_peer_verify;
	memset(&current, 0, sizeof(current));
	if (owned_read(peer, &current) != 0) {
		mesh_runtime_data_clear(&current);
		mesh_peer_close(peer);
		return -1;
	}
	mesh_runtime_data_clear(&current);
	return 0;
}

void mesh_peer_close(struct mesh_peer *peer)
{
	if (peer == NULL) {
		return;
	}
	if (peer->session_lock >= 0) {
		flock(peer->session_lock, LOCK_UN);
		close(peer->session_lock);
	}
	mesh_sensitive_clear(peer, sizeof(*peer));
	peer->session_lock = -1;
}
