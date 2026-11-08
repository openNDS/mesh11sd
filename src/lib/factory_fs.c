/**
 * @file factory_fs.c
 * @brief Collect device-tree, board, and permanent-MAC evidence without a shell.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#define _GNU_SOURCE
#include "native.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <uci.h>
#include <unistd.h>
#include <libubox/blobmsg_json.h>

static int read_text(int root, const char *path, char *buffer, size_t capacity)
{
	size_t length;

	if (mesh_read_bounded_at(root, path, buffer, capacity - 1, &length, NULL) != 0) {
		return -1;
	}
	while (length != 0 && (buffer[length - 1] == '\0' || buffer[length - 1] == '\n' ||
						   buffer[length - 1] == '\r')) {
		length--;
	}
	if (memchr(buffer, '\0', length) != NULL) {
		return -1;
	}
	buffer[length] = '\0';
	return 0;
}

static int number_file(int root, const char *path, unsigned int *value)
{
	char text[32];
	char *end;
	unsigned long number;

	if (read_text(root, path, text, sizeof(text)) < 0 || text[0] < '0' || text[0] > '9') {
		return -1;
	}
	errno = 0;
	number = strtoul(text, &end, 10);
	if (errno != 0 || *end != '\0' || number > UINT32_MAX) {
		return -1;
	}
	*value = (unsigned int)number;
	return 0;
}

static void dt_read(int root, const char *base, struct mesh_device_tree_mac *result)
{
	static const char *const properties[] = { "mac-address", "local-mac-address" };
	struct mesh_mac_evidence *evidence[2];
	char alias[256];
	char path[512];
	uint8_t bytes[7];
	size_t length;
	size_t i;

	evidence[0] = &result->mac_address;
	evidence[1] = &result->local_mac_address;
	snprintf(path, sizeof(path), "%s/aliases/label-mac-device", base);
	if (read_text(root, path, alias, sizeof(alias)) < 0 || alias[0] != '/' ||
		strstr(alias, "..") != NULL || strstr(alias, "//") != NULL) {
		return;
	}
	for (i = 0; i < 2; i++) {
		snprintf(path, sizeof(path), "%s%s/%s", base, alias, properties[i]);
		if (mesh_read_bounded_at(root, path, bytes, sizeof(bytes), &length, NULL) == 0 &&
			length == 6 && mesh_mac_valid(bytes)) {
			evidence[i]->present = true;
			memcpy(evidence[i]->mac, bytes, 6);
		}
	}
}

static struct blob_attr *unique_attr(struct blob_attr *table, const char *name, int type)
{
	struct blob_attr *attr;
	struct blob_attr *found;
	size_t remaining;

	found = NULL;
	blobmsg_for_each_attr(attr, table, remaining) {
		if (strcmp(blobmsg_name(attr), name) != 0) {
			continue;
		}
		if (found != NULL || blobmsg_type(attr) != type) {
			return NULL;
		}
		found = attr;
	}
	return found;
}

static int board_read(int root, struct mesh_mac_evidence *evidence)
{
	struct blob_buf buffer;
	struct blob_attr *system;
	struct blob_attr *label;
	char text[MESH_INPUT_MAX + 1];
	struct stat metadata;
	size_t length;
	int result;

	if (fstatat(root, "etc/board.json", &metadata, AT_SYMLINK_NOFOLLOW) < 0 && errno == ENOENT) {
		return 0;
	}
	if (mesh_read_bounded_at(root, "etc/board.json", text, MESH_INPUT_MAX, &length, NULL) < 0 ||
		memchr(text, '\0', length) != NULL) {
		return -1;
	}
	text[length] = '\0';
	memset(&buffer, 0, sizeof(buffer));
	blob_buf_init(&buffer, 0);
	result = -1;
	if (!blobmsg_add_json_from_string(&buffer, text)) {
		goto done;
	}
	system = unique_attr(buffer.head, "system", BLOBMSG_TYPE_TABLE);
	label = system != NULL ? unique_attr(system, "label_macaddr", BLOBMSG_TYPE_STRING) : NULL;
	if (label != NULL && mesh_mac_parse(blobmsg_get_string(label), evidence->mac) == 0) {
		evidence->present = true;
	}
	result = 0;
done:
	blob_buf_free(&buffer);
	mesh_sensitive_clear(text, sizeof(text));
	return result;
}

static int system_read(int root, struct mesh_mac_evidence *evidence)
{
	char text[MESH_INPUT_MAX];
	struct stat metadata;
	struct uci_context *context;
	struct uci_package *package;
	struct uci_element *element;
	struct uci_section *section;
	const char *value;
	size_t length;
	FILE *stream;
	int status;

	if (fstatat(root, "etc/config/system", &metadata, AT_SYMLINK_NOFOLLOW) < 0 && errno == ENOENT) {
		return 0;
	}
	if (mesh_read_bounded_at(root, "etc/config/system", text, sizeof(text), &length, NULL) < 0 ||
		memchr(text, '\0', length) != NULL) {
		return -1;
	}
	context = uci_alloc_context();
	stream = fmemopen(text, length, "r");
	package = NULL;
	status = context != NULL && stream != NULL ?
			 uci_import(context, stream, "system", &package, true) : UCI_ERR_IO;
	if (status == UCI_OK) {
		uci_foreach_element(&package->sections, element) {
			section = uci_to_section(element);
			if (strcmp(section->type, "system") != 0) {
				continue;
			}
			value = uci_lookup_option_string(context, section, "label_macaddr");
			if (value != NULL && mesh_mac_parse(value, evidence->mac) == 0) {
				evidence->present = true;
			}
			break;
		}
	}
	if (stream != NULL) {
		fclose(stream);
	}
	if (context != NULL) {
		uci_free_context(context);
	}
	mesh_sensitive_clear(text, sizeof(text));
	return status == UCI_OK ? 0 : -1;
}

static bool real_root(int root)
{
	struct stat selected;
	struct stat actual;

	return fstat(root, &selected) == 0 && stat("/", &actual) == 0 &&
		   selected.st_dev == actual.st_dev && selected.st_ino == actual.st_ino;
}

static int permanent_query(const char *name, uint8_t mac[6])
{
	struct {
		struct ethtool_perm_addr header;
		unsigned char bytes[32];
	} request;
	struct ifreq interface;
	int fd;
	int status;

	fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return -1;
	}
	memset(&request, 0, sizeof(request));
	memset(&interface, 0, sizeof(interface));
	request.header.cmd = ETHTOOL_GPERMADDR;
	request.header.size = sizeof(request.bytes);
	memcpy(interface.ifr_name, name, strlen(name) + 1);
	interface.ifr_data = (void *)&request;
	status = ioctl(fd, SIOCETHTOOL, &interface);
	close(fd);
	if (status != 0 || request.header.size != 6 || !mesh_mac_valid(request.bytes)) {
		return -1;
	}
	memcpy(mac, request.bytes, 6);
	return 0;
}

enum mesh_error mesh_factory_collect(int root_fd, const char *wan,
									 const struct mesh_discovery_ops *ops,
									 struct mesh_factory_input *output)
{
	struct mesh_factory_input result;
	struct mesh_factory_interface *interface;
	struct dirent *entry;
	struct stat metadata;
	char path[256];
	char text[32];
	DIR *directory;
	int fd;
	int status;

	if (output == NULL) {
		return MESH_E_INPUT;
	}
	memset(&result, 0, sizeof(result));
	result.wan_device = wan;
	dt_read(root_fd, "sys/firmware/devicetree/base", &result.device_tree[0]);
	dt_read(root_fd, "proc/device-tree", &result.device_tree[1]);
	dt_read(root_fd, "tmp/mesh11sd/devicetree", &result.device_tree[2]);
	if (board_read(root_fd, &result.board_json_system_label) < 0 ||
		system_read(root_fd, &result.first_uci_system_label) < 0) {
		return MESH_E_INPUT;
	}
	fd = mesh_open_at_root(root_fd, "sys/class/net", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0) {
		*output = result;
		return MESH_OK;
	}
	directory = fdopendir(fd);
	if (directory == NULL) {
		close(fd);
		return MESH_E_PLATFORM;
	}
	while ((entry = readdir(directory)) != NULL) {
		if (entry->d_name[0] == '.') {
			continue;
		}
		if (strlen(entry->d_name) >= 16 || result.interface_count == MESH_FACTORY_INTERFACES_MAX) {
			closedir(directory);
			return MESH_E_INPUT;
		}
		interface = &result.interfaces[result.interface_count];
		memcpy(interface->name, entry->d_name, strlen(entry->d_name) + 1);
		snprintf(path, sizeof(path), "sys/class/net/%.15s/ifindex", entry->d_name);
		if (number_file(root_fd, path, &interface->ifindex) < 0) {
			continue;
		}
		snprintf(path, sizeof(path), "sys/class/net/%.15s/type", entry->d_name);
		if (number_file(root_fd, path, &interface->arphrd_type) < 0) {
			continue;
		}
		snprintf(path, sizeof(path), "sys/class/net/%.15s/bridge", entry->d_name);
		interface->is_bridge = fstatat(root_fd, path, &metadata, 0) == 0 && S_ISDIR(metadata.st_mode);
		status = -1;
		if (ops != NULL && ops->permanent_mac != NULL) {
			status = ops->permanent_mac(ops->context, interface->name, interface->ethtool_permanent.mac);
		} else if (real_root(root_fd)) {
			status = permanent_query(interface->name, interface->ethtool_permanent.mac);
		}
		interface->ethtool_permanent.present = status == 0;
		snprintf(path, sizeof(path), "sys/class/net/%.15s/addr_assign_type", entry->d_name);
		interface->addr_assign_type = UINT32_MAX;
		(void)number_file(root_fd, path, &interface->addr_assign_type);
		snprintf(path, sizeof(path), "sys/class/net/%.15s/address", entry->d_name);
		if (read_text(root_fd, path, text, sizeof(text)) == 0 &&
			mesh_mac_parse(text, interface->sysfs_address.mac) == 0) {
			interface->sysfs_address.present = true;
		}
		result.interface_count++;
	}
	closedir(directory);
	*output = result;
	return MESH_OK;
}
