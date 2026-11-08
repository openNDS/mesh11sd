/**
 * @file services_dns.c
 * @brief Install the owned dnsmasq include and restart that instance.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#define _GNU_SOURCE
#include "services.h"
#include "mrp.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int list(struct mesh_services *services, struct mesh_runtime_data *reply)
{
	struct mesh_message request;
	int result = -1;
	if (mesh_message_init(&request) == 0 && blobmsg_add_string(&request.blob, "name", "dnsmasq") == 0 &&
		blobmsg_add_u8(&request.blob, "verbose", true) == 0) {
		result = mesh_peer_call(services->peer, "service", "list", &request, reply);
	}
	mesh_message_clear(&request);
	return result;
}

static struct blob_attr *instances(struct mesh_runtime_data *reply)
{
	return mesh_field(mesh_root_field(reply, "dnsmasq", BLOBMSG_TYPE_TABLE), "instances", BLOBMSG_TYPE_TABLE);
}

static uint32_t pid(struct blob_attr *instance)
{
	struct blob_attr *field = mesh_field(instance, "running", BLOBMSG_TYPE_BOOL);
	if (field == NULL || !blobmsg_get_bool(field)) {
		return 0;
	}
	field = mesh_field(instance, "pid", BLOBMSG_TYPE_INT32);
	return field != NULL ? blobmsg_get_u32(field) : 0;
}

static bool same_instance(struct blob_attr *before, struct blob_attr *after, bool selected)
{
	struct blob_attr *field;
	struct blob_attr *other;
	int remaining;
	size_t first = 0;
	size_t second = 0;

	if (before == NULL || after == NULL) {
		return false;
	}
	blobmsg_for_each_attr(field, before, remaining) {
		if (selected && (strcmp(blobmsg_name(field), "pid") == 0 || strcmp(blobmsg_name(field), "running") == 0 ||
						 strcmp(blobmsg_name(field), "exit_code") == 0)) {
			continue;
		}
		other = mesh_field(after, blobmsg_name(field), -1);
		if (other == NULL || blob_pad_len(field) != blob_pad_len(other) ||
			memcmp(field, other, blob_pad_len(field)) != 0) {
			return false;
		}
		first++;
	}
	blobmsg_for_each_attr(field, after, remaining) {
		if (selected && (strcmp(blobmsg_name(field), "pid") == 0 || strcmp(blobmsg_name(field), "running") == 0 ||
						 strcmp(blobmsg_name(field), "exit_code") == 0)) {
			continue;
		}
		second++;
	}
	return first == second;
}

static bool same_service(struct mesh_runtime_data *before, struct mesh_runtime_data *after, const char *selected)
{
	struct blob_attr *old = instances(before);
	struct blob_attr *now = instances(after);
	struct blob_attr *instance;
	int remaining;
	size_t count = 0;
	size_t total = 0;
	if (old == NULL || now == NULL) {
		return false;
	}
	blobmsg_for_each_attr(instance, old, remaining) {
		if (blobmsg_type(instance) != BLOBMSG_TYPE_TABLE ||
			!same_instance(instance, mesh_field(now, blobmsg_name(instance), BLOBMSG_TYPE_TABLE),
						   selected != NULL && strcmp(blobmsg_name(instance), selected) == 0)) {
			return false;
		}
		count++;
	}
	blobmsg_for_each_attr(instance, now, remaining) {
		total++;
	}
	return count == total;
}

static bool identifier(const char *name)
{
	size_t i;
	if (name[0] == '\0' || strlen(name) >= 64) {
		return false;
	}
	for (i = 0; name[i] != '\0'; i++) {
		if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= 'A' && name[i] <= 'Z') ||
			  (name[i] >= '0' && name[i] <= '9') || name[i] == '_')) {
			return false;
		}
	}
	return true;
}

static int config_file(struct mesh_services *services, struct mesh_dns_record *record,
						struct blob_attr *instance, struct mesh_file_stamp *stamp)
{
	unsigned char contents[MESH_INPUT_MAX + 1];
	struct blob_attr *command;
	struct blob_attr *argument;
	struct blob_attr *jail;
	struct blob_attr *mount;
	struct blob_attr *field;
	char expected[MESH_SERVICE_PATH];
	char directory[MESH_SERVICE_PATH];
	char *line;
	char *end;
	char *filter;
	const char *value;
	size_t length;
	size_t index = 0;
	int remaining;
	int fd = -1;
	struct stat metadata;
	bool want_config = false;
	bool found = false;
	bool foreground = false;
	bool have_directory = false;
	int result = -1;

	memset(contents, 0, sizeof(contents));
	if (!identifier(record->instance) || pid(instance) <= 1) {
		goto done;
	}
	snprintf(expected, sizeof(expected), "/var/etc/dnsmasq.conf.%s", record->instance);
	command = mesh_field(instance, "command", BLOBMSG_TYPE_ARRAY);
	if (command == NULL) {
		goto done;
	}
	blobmsg_for_each_attr(argument, command, remaining) {
		if (blobmsg_type(argument) != BLOBMSG_TYPE_STRING) {
			goto done;
		}
		value = blobmsg_get_string(argument);
		if (index++ == 0 && strcmp(value, "/usr/sbin/dnsmasq") != 0) {
			goto done;
		}
		if (want_config) {
			if (found || strcmp(value, expected) != 0) {
				goto done;
			}
			found = true;
			want_config = false;
		} else if (strcmp(value, "-C") == 0) {
			want_config = true;
		} else if (strcmp(value, "-k") == 0) {
			foreground = true;
		}
	}
	if (!found || want_config || !foreground ||
		mesh_read_bounded_at(services->root, expected + 1, contents, sizeof(contents) - 1,
							 &length, stamp) != 0 || memchr(contents, '\0', length) != NULL) {
		goto done;
	}
	contents[length] = '\0';
	line = (char *)contents;
	while (*line != '\0') {
		end = strchr(line, '\n');
		if (end != NULL) {
			*end = '\0';
		}
		if (strncmp(line, "conf-dir=", 9) == 0) {
			if (have_directory || strlen(line + 9) >= sizeof(directory)) {
				goto done;
			}
			memcpy(directory, line + 9, strlen(line + 9) + 1);
			filter = strchr(directory, ',');
			if (filter != NULL) {
				if (strcmp(filter, ",*.conf") != 0) {
					goto done;
				}
				*filter = '\0';
			}
			if (directory[0] != '/' || directory[1] == '\0' ||
				strchr(directory, '\r') != NULL || strchr(directory, '\n') != NULL) {
				goto done;
			}
			have_directory = true;
		}
		if (end == NULL) {
			break;
		}
		line = end + 1;
	}
	if (!have_directory) {
		goto done;
	}
	jail = mesh_field(instance, "jail", BLOBMSG_TYPE_TABLE);
	if (jail != NULL) {
		field = mesh_field(jail, "netns", BLOBMSG_TYPE_BOOL);
		mount = mesh_field(instance, "mount", BLOBMSG_TYPE_TABLE);
		if ((field != NULL && blobmsg_get_bool(field)) || mesh_field(mount, directory, BLOBMSG_TYPE_STRING) == NULL) {
			goto done;
		}
	}
	fd = mesh_open_at_root(services->root, directory + 1, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0 || fstat(fd, &metadata) != 0 || metadata.st_uid != geteuid() || (metadata.st_mode & 0022) != 0) {
		goto done;
	}
	if (record->directory[0] != '\0' && (strcmp(record->config, expected) != 0 || strcmp(record->directory, directory) != 0)) {
		goto done;
	}
	memcpy(record->config, expected, strlen(expected) + 1);
	memcpy(record->directory, directory, strlen(directory) + 1);
	result = 0;
done:
	if (fd >= 0) {
		close(fd);
	}
	mesh_sensitive_clear(contents, sizeof(contents));
	return result;
}

int mesh_services_dns_discover(struct mesh_services *services, const struct mesh_native_config *config)
{
	struct mesh_runtime_data reply = {0};
	struct blob_attr *all;
	struct blob_attr *instance;
	size_t i;
	size_t j;
	int result = -1;

	if (config->dns_suppression_needed && (config->dns_instance_count == 0 ||
		services->peer->transport.restart_dnsmasq == NULL)) {
		return -1;
	}
	if (services->recovering && services->disk.dns_count != (config->dns_suppression_needed ? config->dns_instance_count : 0)) {
		return -1;
	}
	if (!services->recovering) {
		services->disk.dns_count = config->dns_suppression_needed ? (uint8_t)config->dns_instance_count : 0;
	}
	if (services->disk.dns_count == 0) {
		return 0;
	}
	if (list(services, &reply) != 0 || (all = instances(&reply)) == NULL) {
		goto done;
	}
	for (i = 0; i < services->disk.dns_count; i++) {
		if (services->recovering && strcmp(services->disk.dns[i].instance, config->dns_instances[i]) != 0) {
			goto done;
		}
		memcpy(services->disk.dns[i].instance, config->dns_instances[i], sizeof(services->disk.dns[i].instance));
		instance = mesh_field(all, services->disk.dns[i].instance, BLOBMSG_TYPE_TABLE);
		if (config_file(services, &services->disk.dns[i], instance, &services->config_stamps[i]) != 0) {
			goto done;
		}
		for (j = 0; j < i; j++) {
			if (strcmp(services->disk.dns[i].directory, services->disk.dns[j].directory) == 0) {
				goto done;
			}
		}
	}
	services->dns_snapshot = reply;
	result = 0;
done:
	mesh_runtime_data_clear(&reply);
	return result;
}

static int own_file(struct mesh_services *services, size_t index, int action)
{
	struct mesh_dns_record *record = &services->disk.dns[index];
	struct stat metadata;
	char filename[96];
	char expected[2048];
	char contents[2048];
	size_t length;
	int directory;
	int fd;
	int result = -1;

	snprintf(filename, sizeof(filename), "mesh11sd-v8-%s.conf", services->peer->owner);
	if (services->disk.dns_state == 1 || services->disk.dns_state == 3 ||
		mesh_mrp_dns_text(services, services->disk.dns_state == 2, expected, sizeof(expected)) < 0) {
		return -1;
	}
	directory = mesh_open_at_root(services->root, record->directory + 1, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (directory < 0) {
		return -1;
	}
	if (fstatat(directory, filename, &metadata, AT_SYMLINK_NOFOLLOW) == 0) {
		if (!S_ISREG(metadata.st_mode) || metadata.st_uid != geteuid() || metadata.st_nlink != 1 ||
			(metadata.st_mode & 0777) != 0600 ||
			mesh_read_bounded_at(directory, filename, contents, sizeof(contents), &length, NULL) != 0 ||
			length != strlen(expected) || memcmp(contents, expected, length) != 0) {
			goto done;
		}
		result = action != 0 ? 0 : unlinkat(directory, filename, 0);
	} else if (errno == ENOENT) {
		if (action == 2) {
			goto done;
		}
		if (action == 0) {
			result = 0;
			goto done;
		}
		fd = openat(directory, filename, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
		if (fd < 0) {
			goto done;
		}
		result = write(fd, expected, strlen(expected)) == (ssize_t)strlen(expected) && fsync(fd) == 0 ? 0 : -1;
		if (close(fd) != 0) {
			result = -1;
		}
	}
	if (result == 0) {
		result = fsync(directory);
	}
done:
	close(directory);
	return result;
}

static int restart(struct mesh_services *services, size_t index)
{
	struct mesh_runtime_data before = {0};
	struct mesh_runtime_data after = {0};
	struct blob_attr *instance;
	uint32_t old_pid;
	uint32_t new_pid;
	unsigned int attempt;
	unsigned int stable = 0;
	uint32_t candidate_pid = 0;
	struct mesh_file_stamp candidate_stamp = {0};
	int result = -1;
	const char *name = services->disk.dns[index].instance;

	if (list(services, &before) != 0 || !same_service(&services->dns_snapshot, &before, NULL) ||
		mesh_stamp_unchanged(services->root, &services->config_stamps[index]) != 0) {
		goto done;
	}
	old_pid = pid(mesh_field(instances(&before), name, BLOBMSG_TYPE_TABLE));
	/* Platform rc.common restart <instance> performs delete/add of ONLY this
	 * instance; unlike HUP it really rereads config. No private procd cloning. */
	if (services->peer->transport.restart_dnsmasq(services->peer->transport.context, name) != 0) {
		goto done;
	}
	for (attempt = 0; attempt < 120; attempt++) {
		if (list(services, &after) == 0) {
			instance = mesh_field(instances(&after), name, BLOBMSG_TYPE_TABLE);
			new_pid = pid(instance);
			if (new_pid > 1 && new_pid != old_pid && same_service(&before, &after, name)) {
				if (new_pid != candidate_pid) {
					if (config_file(services, &services->disk.dns[index], instance, &candidate_stamp) != 0) {
						goto done;
					}
					candidate_pid = new_pid;
					stable = 1;
				} else if (mesh_stamp_unchanged(services->root, &candidate_stamp) == 0) {
					stable++;
				} else {
					goto done;
				}
				/* Nine observations separated by 250ms prove two seconds of
				 * stable PID/definition/generated configuration, not health
				 * forever and not a DHCP exchange. */
				if (stable >= 9) {
					services->config_stamps[index] = candidate_stamp;
					services->dns_snapshot = after;
					result = 0;
					break;
				}
			} else {
				candidate_pid = 0;
				stable = 0;
			}
		} else {
			candidate_pid = 0;
			stable = 0;
		}
		services->peer->transport.wait_ms(services->peer->transport.context, 250);
	}
done:
	mesh_runtime_data_clear(&before);
	mesh_runtime_data_clear(&after);
	return result;
}

int mesh_services_dns_guard(struct mesh_services *services)
{
	struct mesh_runtime_data current = {0};
	struct mesh_runtime_data stable = {0};
	struct blob_attr *old;
	struct blob_attr *now;
	struct blob_attr *instance;
	struct blob_attr *other;
	int remaining;
	size_t i;
	size_t count = 0;
	size_t total = 0;
	bool selected;
	bool regenerated = false;
	unsigned int attempt;
	int result = -1;
	if (services->disk.dns_count == 0) {
		return 0;
	}
	if (list(services, &current) != 0 || (old = instances(&services->dns_snapshot)) == NULL ||
		(now = instances(&current)) == NULL) {
		goto done;
	}
	blobmsg_for_each_attr(instance, old, remaining) {
		selected = false;
		for (i = 0; i < services->disk.dns_count; i++) {
			selected = selected || strcmp(blobmsg_name(instance), services->disk.dns[i].instance) == 0;
		}
		other = mesh_field(now, blobmsg_name(instance), BLOBMSG_TYPE_TABLE);
		if (!same_instance(instance, other, selected) || (selected && pid(other) <= 1)) {
			goto done;
		}
		count++;
	}
	blobmsg_for_each_attr(instance, now, remaining) {
		total++;
	}
	if (count != total) {
		goto done;
	}
	for (i = 0; i < services->disk.dns_count; i++) {
		instance = mesh_field(old, services->disk.dns[i].instance, BLOBMSG_TYPE_TABLE);
		other = mesh_field(now, services->disk.dns[i].instance, BLOBMSG_TYPE_TABLE);
		if (pid(instance) == pid(other)) {
			if (mesh_stamp_unchanged(services->root, &services->config_stamps[i]) != 0) {
				goto done;
			}
		} else {
			if (config_file(services, &services->disk.dns[i], other, &services->config_stamps[i]) != 0) {
				goto done;
			}
			regenerated = true;
		}
	}
	/* Platform-triggered regeneration after LAN protocol changes may restart
	 * ONLY selected owners. Accept a new PID only with unchanged procd command/
	 * jail/mount definition and the SAME validated include directory. Never
	 * accept a changed generated file under the same still-running PID. */
	if (regenerated) {
		for (attempt = 0; attempt < 8; attempt++) {
			services->peer->transport.wait_ms(services->peer->transport.context, 250);
			if (list(services, &stable) != 0 || !same_service(&current, &stable, NULL)) {
				goto done;
			}
			for (i = 0; i < services->disk.dns_count; i++) {
				if (mesh_stamp_unchanged(services->root, &services->config_stamps[i]) != 0) {
					goto done;
				}
			}
		}
	}
	services->dns_snapshot = current;
	result = 0;
done:
	mesh_runtime_data_clear(&current);
	mesh_runtime_data_clear(&stable);
	return result;
}

int mesh_services_dns_verify(struct mesh_services *services)
{
	size_t i;
	if (mesh_services_dns_guard(services) != 0 || (services->disk.dns_state == 2 && mesh_mrp_dns_guard(services) != 0)) {
		return -1;
	}
	for (i = 0; i < services->disk.dns_count; i++) {
		if (own_file(services, i, 2) != 0) {
			return -1;
		}
	}
	return 0;
}

int mesh_services_dns_apply(struct mesh_services *services, bool suppress)
{
	size_t i;
	if (mesh_services_dns_guard(services) != 0) {
		return -1;
	}
	for (i = 0; i < services->disk.dns_count; i++) {
		if (own_file(services, i, suppress) != 0 || restart(services, i) != 0) {
			return -1;
		}
	}
	return 0;
}

uint32_t mesh_services_dns_pid(struct mesh_services *services)
{
	if (services->disk.dns_count != 1) {
		return 0;
	}
	return pid(mesh_field(instances(&services->dns_snapshot), services->disk.dns[0].instance, BLOBMSG_TYPE_TABLE));
}

static int profile_file(struct mesh_services *services, bool portal)
{
	char name[96];
	char pending[112];
	char old[2048];
	char wanted[2048];
	char contents[2048];
	struct mesh_file_stamp stamp;
	struct stat metadata;
	size_t length;
	int directory;
	int fd;
	int result = -1;
	if (mesh_mrp_dns_text(services, !portal, old, sizeof(old)) < 0 ||
		mesh_mrp_dns_text(services, portal, wanted, sizeof(wanted)) < 0) {
		return -1;
	}
	snprintf(name, sizeof(name), "mesh11sd-v8-%s.conf", services->peer->owner);
	snprintf(pending, sizeof(pending), ".%s.pending", name);
	directory = mesh_open_at_root(services->root, services->disk.dns[0].directory + 1, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (directory < 0) {
		return -1;
	}
	if (fstatat(directory, name, &metadata, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(metadata.st_mode) ||
		metadata.st_uid != geteuid() || metadata.st_nlink != 1 || (metadata.st_mode & 0777) != 0600 ||
		mesh_read_bounded_at(directory, name, contents, sizeof(contents), &length, &stamp) != 0 ||
		!((length == strlen(old) && memcmp(contents, old, length) == 0) ||
		  (length == strlen(wanted) && memcmp(contents, wanted, length) == 0))) {
		goto done;
	}
	if (length == strlen(wanted) && memcmp(contents, wanted, length) == 0) {
		result = 0;
		goto done;
	}
	fd = openat(directory, pending, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) {
		goto done;
	}
	result = write(fd, wanted, strlen(wanted)) == (ssize_t)strlen(wanted) && fsync(fd) == 0 ? 0 : -1;
	if (close(fd) != 0) {
		result = -1;
	}
	if (result != 0) {
		unlinkat(directory, pending, 0);
		goto done;
	}
	result = mesh_stamp_unchanged(directory, &stamp) == 0 && renameat(directory, pending, directory, name) == 0 &&
			 fsync(directory) == 0 ? 0 : -1;
done:
	close(directory);
	return result;
}

int mesh_mrp_dns_profile(struct mesh_services *services, bool portal)
{
	unsigned int before = services->disk.dns_state;
	if (services->disk.dns_count != 1 || !services->journal_exists || mesh_services_dns_guard(services) != 0) {
		return -1;
	}
	if ((portal && before == 2) || (!portal && before == 0)) {
		return mesh_services_dns_verify(services);
	}
	if (portal) {
		size_t i;
		if (before == 3) {
			return -1;
		}
		if (services->peer->config == NULL || services->peer->plan == NULL ||
			services->peer->config->mrp.profile != MESH_MRP_IPV4_ONLY) {
			return -1;
		}
		for (i = 0; i < services->peer->config->stamp_count; i++) {
			if (mesh_stamp_unchanged(services->root, &services->peer->config->stamps[i]) != 0) {
				return -1;
			}
		}
		if (before == 0) {
			/* Regenerate the selected instance after stable LAN became
			 * proto=none, still with the owned suppression include active.
			 * Audit before enabling any allocator, not only after restart. */
			if (own_file(services, 0, 2) != 0 || mesh_services_dns_apply(services, true) != 0) {
				return -1;
			}
		}
		if (mesh_mrp_dns_guard(services) != 0) {
			return -1;
		}
		if (before == 0) {
			services->disk.portal_pool = services->peer->plan->ipv4;
		}
	}
	services->disk.dns_state = portal ? 1 : 3;
	if (mesh_services_journal(services) != 0 || profile_file(services, portal) != 0 || restart(services, 0) != 0) {
		return -1;
	}
	services->disk.dns_state = portal ? 2 : 0;
	return mesh_services_journal(services) == 0 ? mesh_services_dns_verify(services) : -1;
}
