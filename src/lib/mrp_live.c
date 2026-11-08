/**
 * @file mrp_live.c
 * @brief Check forwarding, nft expressions, and the gateway DNS socket.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#define _GNU_SOURCE
#include "mrp.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <libubox/blobmsg_json.h>

static uint64_t clock_ms(void)
{
	struct timespec now;
	return clock_gettime(CLOCK_MONOTONIC, &now) == 0 ? (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 : UINT64_MAX;
}

static int run(char *const *arguments, char *output, size_t capacity)
{
	char *const environment[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "HOME=/", "LC_ALL=C", NULL };
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attributes;
	int pipes[2] = {-1, -1};
	pid_t child = -1;
	pid_t waited;
	size_t length = 0;
	ssize_t size;
	int status = 0;
	int spawned = -1;
	int result = -1;
	bool exited = false;
	bool have_pipe = false;
	bool write_open = false;
	bool eof = output == NULL;
	uint64_t start = clock_ms();
	unsigned int attempt;
	struct pollfd event;
	if (start == UINT64_MAX || (output != NULL && capacity < 2)) {
		return -1;
	}
	if (output != NULL) {
		if (pipe2(pipes, O_CLOEXEC) != 0) {
			return -1;
		}
		have_pipe = true;
		write_open = true;
		if (fcntl(pipes[0], F_SETFL, O_NONBLOCK) != 0) {
			goto done;
		}
	}
	if (posix_spawn_file_actions_init(&actions) != 0) {
		goto done;
	}
	if (posix_spawnattr_init(&attributes) != 0) {
		posix_spawn_file_actions_destroy(&actions);
		goto done;
	}
	if (posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) == 0 &&
		posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) == 0 &&
		(output == NULL ? posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0) :
						  posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO)) == 0 &&
		posix_spawnattr_setpgroup(&attributes, 0) == 0 &&
		posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP) == 0) {
		spawned = posix_spawn(&child, arguments[0], &actions, &attributes, arguments, environment);
	}
	posix_spawnattr_destroy(&attributes);
	posix_spawn_file_actions_destroy(&actions);
	if (spawned != 0) {
		goto done;
	}
	if (write_open) {
		close(pipes[1]);
		write_open = false;
	}
	for (attempt = 0; attempt < 600 && clock_ms() - start < 30000; attempt++) {
		if (!eof) {
			if (length == capacity - 1) {
				goto kill_child;
			}
			size = read(pipes[0], output + length, capacity - 1 - length);
			if (size > 0) {
				length += (size_t)size;
				continue;
			}
			if (size == 0) {
				eof = true;
			} else if (errno != EAGAIN && errno != EINTR) {
				goto kill_child;
			}
		}
		if (!exited) {
			waited = waitpid(child, &status, WNOHANG);
			if (waited == child) {
				exited = true;
			} else if (waited < 0 && errno != EINTR) {
				goto kill_child;
			}
		}
		if (exited && eof) {
			if (output != NULL) {
				output[length] = '\0';
			}
			result = WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
			goto done;
		}
		event.fd = pipes[0];
		event.events = POLLIN;
		event.revents = 0;
		poll(&event, pipes[0] >= 0 ? 1 : 0, 50);
	}
kill_child:
	if (!exited) {
		kill(-child, SIGKILL);
		while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
		}
	}
done:
	if (have_pipe) {
		close(pipes[0]);
	}
	if (write_open) {
		close(pipes[1]);
	}
	return result;
}

int mesh_mrp_live_reload(void)
{
	char *const arguments[] = { "/sbin/fw4", "reload", NULL };
	return run(arguments, NULL, 0);
}

int mesh_mrp_live_chain(const char *chain, struct mesh_runtime_data *reply)
{
	char *const arguments[] = { "/usr/sbin/nft", "--json", "list", "chain", "inet", "fw4", (char *)chain, NULL };
	char *text;
	size_t i;
	struct mesh_message message;
	int result = -1;
	mesh_message_init(&message);
	if (chain == NULL || chain[0] == '\0' || strlen(chain) >= 64) {
		goto done;
	}
	for (i = 0; chain[i] != '\0'; i++) {
		if (!((chain[i] >= 'a' && chain[i] <= 'z') || (chain[i] >= 'A' && chain[i] <= 'Z') ||
			  (chain[i] >= '0' && chain[i] <= '9') || chain[i] == '_')) {
			goto done;
		}
	}
	text = calloc(1, MESH_RUNTIME_BYTES);
	if (text == NULL) {
		goto done;
	}
	if (run(arguments, text, MESH_RUNTIME_BYTES) == 0 && blobmsg_add_json_from_string(&message.blob, text)) {
		result = mesh_message_copy(&message, reply);
	} else {
		char *const listing[] = { "/usr/sbin/nft", "--json", "list", "chains", "inet", NULL };
		struct blob_attr *entries;
		struct blob_attr *entry;
		struct blob_attr *item;
		struct blob_attr *table_name;
		struct blob_attr *chain_name;
		int left;
		bool found = false;
		mesh_message_clear(&message);
		mesh_message_init(&message);
		if (run(listing, text, MESH_RUNTIME_BYTES) == 0 && blobmsg_add_json_from_string(&message.blob, text) &&
			mesh_message_copy(&message, reply) == 0 &&
			(entries = mesh_root_field(reply, "nftables", BLOBMSG_TYPE_ARRAY)) != NULL) {
			blobmsg_for_each_attr(entry, entries, left) {
				item = mesh_field(entry, "chain", BLOBMSG_TYPE_TABLE);
				table_name = mesh_field(item, "table", BLOBMSG_TYPE_STRING);
				chain_name = mesh_field(item, "name", BLOBMSG_TYPE_STRING);
				if (table_name != NULL && chain_name != NULL && strcmp(blobmsg_get_string(table_name), "fw4") == 0 &&
					strcmp(blobmsg_get_string(chain_name), chain) == 0) {
					found = true;
				}
			}
			if (!found) {
				mesh_message_clear(&message);
				mesh_message_init(&message);
				entries = blobmsg_open_array(&message.blob, "nftables");
				if (entries != NULL) {
					blobmsg_close_array(&message.blob, entries);
					result = mesh_message_copy(&message, reply);
				}
			}
		}
	}
	mesh_sensitive_clear(text, MESH_RUNTIME_BYTES);
	free(text);
done:
	mesh_message_clear(&message);
	return result;
}

static const unsigned char question[] = {
	19, 'm','e','s','h','1','1','s','d','-','v','8','-','g','a','t','e','w','a','y',
	7, 'i','n','v','a','l','i','d', 0, 0, 1, 0, 1
};

static unsigned int read16(const unsigned char *data)
{
	return ((unsigned int)data[0] << 8) | data[1];
}

static int skip_name(const unsigned char *data, size_t size, size_t *offset)
{
	unsigned int labels = 0;
	while (*offset < size && labels++ < 128) {
		unsigned int length = data[(*offset)++];
		if (length == 0) {
			return 0;
		}
		if ((length & 0xc0) == 0xc0) {
			if (*offset >= size || (((length & 63) << 8) | data[*offset]) >= size) {
				return -1;
			}
			(*offset)++;
			return 0;
		}
		if (length > 63 || length > size - *offset) {
			return -1;
		}
		*offset += length;
	}
	return -1;
}

int mesh_mrp_dns_response(const unsigned char *data, size_t size, uint16_t id, uint32_t address)
{
	size_t offset = 12 + sizeof(question);
	unsigned int count;
	unsigned int i;
	unsigned int length;
	if (data == NULL || size < offset || read16(data) != id || (data[2] & 0xfa) != 0x80 ||
		(data[3] & 15) != 0 || read16(data + 4) != 1 || memcmp(data + 12, question, sizeof(question)) != 0) {
		return -1;
	}
	count = read16(data + 6);
	if (count == 0 || count > 32) {
		return -1;
	}
	for (i = 0; i < count; i++) {
		if (skip_name(data, size, &offset) != 0 || size - offset < 10) {
			return -1;
		}
		length = read16(data + offset + 8);
		if (length > size - offset - 10) {
			return -1;
		}
		if (read16(data + offset) == 1 && read16(data + offset + 2) == 1 && length == 4 &&
			(((uint32_t)data[offset + 10] << 24) | ((uint32_t)data[offset + 11] << 16) |
			 ((uint32_t)data[offset + 12] << 8) | data[offset + 13]) == address) {
			return 0;
		}
		offset += 10 + length;
	}
	return -1;
}

static bool dns_answer(uint32_t gateway)
{
	unsigned char query[12 + sizeof(question)] = {0};
	unsigned char answer[4096];
	struct sockaddr_in target = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(gateway) };
	struct timeval timeout = { .tv_sec = 2 };
	uint16_t id;
	ssize_t size;
	int fd;
	bool result = false;
	if (getrandom(&id, sizeof(id), 0) != sizeof(id) || (fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)) < 0) {
		return false;
	}
	query[0] = id >> 8;
	query[1] = id & 255;
	query[2] = 1;
	query[5] = 1;
	memcpy(query + 12, question, sizeof(question));
	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
		connect(fd, (struct sockaddr *)&target, sizeof(target)) == 0 &&
		send(fd, query, sizeof(query), 0) == sizeof(query) && (size = recv(fd, answer, sizeof(answer), 0)) > 0) {
		result = mesh_mrp_dns_response(answer, (size_t)size, id, gateway) == 0;
	}
	close(fd);
	return result;
}

int mesh_mrp_udp67_entry(const char *line, uint64_t *inode)
{
	unsigned int port;
	unsigned long long value;
	if (line == NULL || inode == NULL || strlen(line) >= 512 ||
		sscanf(line, "%*u: %*8x:%x %*8x:%*x %*x %*x:%*x %*x:%*x %*x %*u %*u %llu", &port, &value) != 2 ||
		port != 67 || value == 0) {
		return -1;
	}
	*inode = value;
	return 0;
}

static bool owned_dhcp_socket(uint32_t pid)
{
	char path[64];
	char target[128];
	char line[512];
	unsigned long long sockets[256];
	unsigned long long inode;
	uint64_t parsed_inode;
	size_t count = 0;
	size_t scanned = 0;
	size_t i;
	ssize_t size;
	struct dirent *entry;
	DIR *directory;
	FILE *table;
	bool result = false;
	if (pid <= 1) {
		return false;
	}
	snprintf(path, sizeof(path), "/proc/%u/fd", pid);
	directory = opendir(path);
	if (directory == NULL) {
		return false;
	}
	while ((entry = readdir(directory)) != NULL) {
		if (++scanned > 4096) {
			closedir(directory);
			return false;
		}
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}
		size = readlinkat(dirfd(directory), entry->d_name, target, sizeof(target) - 1);
		if (size > 0 && (size_t)size < sizeof(target) - 1) {
			target[size] = '\0';
			if (sscanf(target, "socket:[%llu]", &inode) == 1) {
				if (count == 256) {
					closedir(directory);
					return false;
				}
				sockets[count++] = inode;
			}
		}
	}
	closedir(directory);
	table = fopen("/proc/net/udp", "re");
	if (table == NULL) {
		return false;
	}
	count = count > 256 ? 256 : count;
	scanned = 0;
	while (fgets(line, sizeof(line), table) != NULL && ++scanned <= 4096) {
		if (mesh_mrp_udp67_entry(line, &parsed_inode) == 0) {
			for (i = 0; i < count; i++) {
				result = result || sockets[i] == parsed_inode;
			}
		}
	}
	fclose(table);
	return scanned <= 4096 && result;
}

int mesh_mrp_live_probe(const char *bridge, const char *uplink, uint32_t gateway,
						uint8_t prefix, uint32_t dns_pid)
{
	struct ifaddrs *addresses;
	struct ifaddrs *item;
	enum mesh_role role;
	bool found = false;
	char forwarding[8];
	int fd;
	ssize_t size;
	if (prefix == 0 || prefix > 30 || getifaddrs(&addresses) != 0) {
		return -1;
	}
	for (item = addresses; item != NULL; item = item->ifa_next) {
		if (item->ifa_addr != NULL && item->ifa_netmask != NULL && item->ifa_addr->sa_family == AF_INET &&
			strcmp(item->ifa_name, bridge) == 0 &&
			ntohl(((struct sockaddr_in *)item->ifa_addr)->sin_addr.s_addr) == gateway &&
			ntohl(((struct sockaddr_in *)item->ifa_netmask)->sin_addr.s_addr) == (UINT32_MAX << (32 - prefix))) {
			found = true;
		}
	}
	freeifaddrs(addresses);
	fd = open("/proc/sys/net/ipv4/ip_forward", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return -1;
	}
	size = read(fd, forwarding, sizeof(forwarding));
	close(fd);
	return found && ((size == 2 && memcmp(forwarding, "1\n", 2) == 0) || (size == 1 && forwarding[0] == '1')) &&
		   mesh_probe_wan_role(uplink, &role) == MESH_OK && role == MESH_ROLE_MRP && owned_dhcp_socket(dns_pid) &&
		   dns_answer(gateway) ? 0 : -1;
}
