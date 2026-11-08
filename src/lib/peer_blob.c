/**
 * @file peer_blob.c
 * @brief Build the bounded ubus request blobs for the peer backend.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#include "peer.h"
#include <string.h>

static bool fixed_storage(struct blob_buf *buffer, int required)
{
	(void)buffer;
	(void)required;
	return false;
}

int mesh_message_init(struct mesh_message *message)
{
	memset(message, 0, sizeof(*message));
	message->blob.buf = message->storage;
	message->blob.buflen = sizeof(message->storage);
	message->blob.grow = fixed_storage;
	return blob_buf_init(&message->blob, 0);
}

void mesh_message_clear(struct mesh_message *message)
{
	mesh_sensitive_clear(message, sizeof(*message));
}

static bool valid_list(const void *data, size_t length, unsigned int depth, bool named)
{
	const unsigned char *cursor = data;
	const unsigned char *previous;
	const struct blob_attr *item;
	const struct blob_attr *other;
	size_t remaining = length;
	size_t before;
	size_t padded;
	int type;

	if (depth > 12) {
		return false;
	}
	while (remaining != 0) {
		if (remaining < sizeof(*item)) {
			return false;
		}
		item = (const struct blob_attr *)cursor;
		padded = blob_pad_len(item);
		if (padded < sizeof(*item) || padded > remaining ||
			!blobmsg_check_attr_len(item, named, remaining)) {
			return false;
		}
		type = blobmsg_type(item);
		if (type == BLOBMSG_TYPE_STRING &&
			strlen(blobmsg_get_string((struct blob_attr *)item)) + 1 != (size_t)blobmsg_data_len(item)) {
			return false;
		}
		if (named) {
			previous = data;
			before = length - remaining;
			while (before != 0) {
				other = (const struct blob_attr *)previous;
				if (strcmp(blobmsg_name(item), blobmsg_name(other)) == 0) {
					return false;
				}
				previous += blob_pad_len(other);
				before -= blob_pad_len(other);
			}
		}
		if ((type == BLOBMSG_TYPE_TABLE || type == BLOBMSG_TYPE_ARRAY) &&
			!valid_list(blobmsg_data(item), blobmsg_data_len(item), depth + 1, type == BLOBMSG_TYPE_TABLE)) {
			return false;
		}
		cursor += padded;
		remaining -= padded;
	}
	return true;
}

bool mesh_reply_valid(const struct mesh_runtime_data *data)
{
	const struct blob_attr *root;
	if (data == NULL || data->length < sizeof(*root) || data->length > sizeof(data->bytes)) {
		return false;
	}
	root = (const struct blob_attr *)data->bytes;
	return blob_pad_len(root) == data->length && blob_raw_len(root) >= sizeof(*root) &&
		   valid_list(blob_data(root), blob_len(root), 0, true);
}

struct blob_attr *mesh_field(struct blob_attr *table, const char *name, int type)
{
	struct blob_attr *item;
	int remaining;
	if (table == NULL) {
		return NULL;
	}
	blobmsg_for_each_attr(item, table, remaining) {
		if (strcmp(blobmsg_name(item), name) == 0) {
			return type < 0 || blobmsg_type(item) == type ? item : NULL;
		}
	}
	return NULL;
}

struct blob_attr *mesh_root_field(const struct mesh_runtime_data *data, const char *name, int type)
{
	struct blob_attr *root;
	struct blob_attr *item;
	int remaining;
	if (!mesh_reply_valid(data)) {
		return NULL;
	}
	root = (struct blob_attr *)data->bytes;
	blob_for_each_attr(item, root, remaining) {
		if (strcmp(blobmsg_name(item), name) == 0) {
			return type < 0 || blobmsg_type(item) == type ? item : NULL;
		}
	}
	return NULL;
}

int mesh_message_copy(const struct mesh_message *message, struct mesh_runtime_data *output)
{
	size_t length;
	if (message->blob.head == NULL || output == NULL) {
		return -1;
	}
	length = blob_pad_len(message->blob.head);
	if (length > sizeof(output->bytes)) {
		return -1;
	}
	memset(output, 0, sizeof(*output));
	memcpy(output->bytes, message->blob.head, length);
	output->length = length;
	return 0;
}
