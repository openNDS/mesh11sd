/**
 * @file mrp_dns_guard.c
 * @brief Audit effective dnsmasq configuration for hidden overrides.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#define _GNU_SOURCE
#include "mrp.h"
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct scan {
	struct mesh_services *services;
	unsigned int files;
	bool interface_seen;
	bool interface_allowed;
	char own[512];
};

static int file(struct scan *scan, const char *path, unsigned int depth);

static int directory(struct scan *scan, const char *path, unsigned int depth)
{
	char name[512];
	char clean[256];
	char *filter;
	struct dirent *entry;
	struct stat metadata;
	DIR *dir;
	int fd;
	int length;
	int result = -1;
	bool conf_only = false;
	unsigned int count = 0;
	if (path == NULL || strlen(path) >= sizeof(clean)) {
		return -1;
	}
	memcpy(clean, path, strlen(path) + 1);
	filter = strchr(clean, ',');
	if (filter != NULL) {
		if (strcmp(filter, ",*.conf") != 0) {
			return -1;
		}
		*filter = '\0';
		conf_only = true;
	}
	if (clean[0] != '/' || depth > 4 ||
		(fd = mesh_open_at_root(scan->services->root, clean + 1, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) {
		return -1;
	}
	if (fstat(fd, &metadata) != 0 || metadata.st_uid != geteuid() || (metadata.st_mode & 0022) != 0) {
		close(fd);
		return -1;
	}
	dir = fdopendir(fd);
	if (dir == NULL) {
		close(fd);
		return -1;
	}
	errno = 0;
	while ((entry = readdir(dir)) != NULL) {
		size_t size = strlen(entry->d_name);
		if (++count > 256) {
			goto done;
		}
		/* dnsmasq deliberately ignores dotfiles and editor backups. */
		if (entry->d_name[0] == '.' || entry->d_name[0] == '#' ||
			(size != 0 && entry->d_name[size - 1] == '~') ||
			(conf_only && (size < 5 || strcmp(entry->d_name + size - 5, ".conf") != 0))) {
			errno = 0;
			continue;
		}
		if (fstatat(fd, entry->d_name, &metadata, AT_SYMLINK_NOFOLLOW) != 0) {
			goto done;
		}
		if (S_ISDIR(metadata.st_mode)) {
			errno = 0;
			continue;
		}
		length = snprintf(name, sizeof(name), "%s/%s", clean, entry->d_name);
		if (length < 0 || (size_t)length >= sizeof(name) || file(scan, name, depth + 1) != 0) {
			goto done;
		}
		errno = 0;
	}
	result = errno == 0 ? 0 : -1;
done:
	closedir(dir);
	return result;
}

static int file(struct scan *scan, const char *path, unsigned int depth)
{
	char *text;
	char *line;
	char *next;
	char *option;
	char *end;
	size_t length;
	struct mesh_file_stamp stamp;
	int result = -1;
	bool relevant;
	if (path == NULL || path[0] != '/' || depth > 4 || ++scan->files > 128) {
		return -1;
	}
	if (strcmp(path, scan->own) == 0) {
		/* Exact owned body is independently checked by services_dns.c. */
		return 0;
	}
	text = calloc(1, MESH_INPUT_MAX + 1);
	if (text == NULL) {
		return -1;
	}
	if (mesh_read_bounded_at(scan->services->root, path + 1, text, MESH_INPUT_MAX, &length, &stamp) != 0 ||
		memchr(text, '\0', length) != NULL || stamp.uid != geteuid() || (stamp.mode & 0022) != 0) {
		goto done;
	}
	text[length] = '\0';
	next = text;
	while ((line = strsep(&next, "\n")) != NULL) {
		while (*line == ' ' || *line == '\t') {
			line++;
		}
		length = strlen(line);
		while (length != 0 && isspace((unsigned char)line[length - 1])) {
			line[--length] = '\0';
		}
		if (*line == '#' || *line == '\0') {
			continue;
		}
		if (strncmp(line, "--", 2) == 0) {
			line += 2;
		}
		/* Parse the option name, not its raw spelling: whitespace and inline
		 * comments must never hide a probing/allocation override. Complex
		 * quoting for relevant values is unsupported rather than guessed. */
		end = line;
		while (*end != '\0' && *end != '=' && !isspace((unsigned char)*end)) {
			end++;
		}
		option = NULL;
		if (*end != '\0') {
			char delimiter = *end;
			*end++ = '\0';
			while (isspace((unsigned char)*end)) {
				end++;
			}
			if (delimiter == '=') {
				option = end;
			} else if (*end == '=') {
				option = end + 1;
				while (isspace((unsigned char)*option)) {
					option++;
				}
			}
		}
		if (strcmp(line, "no-ping") == 0 || strcmp(line, "dhcp-sequential-ip") == 0 ||
			strcmp(line, "dhcp-range") == 0 || strcmp(line, "dhcp-option") == 0 ||
			strcmp(line, "dhcp-option-force") == 0 || strcmp(line, "dhcp-ignore") == 0 ||
			strcmp(line, "dhcp-relay") == 0 || strcmp(line, "dhcp-lease-max") == 0 ||
			strcmp(line, "conf-script") == 0) {
			goto done;
		}
		relevant = strcmp(line, "no-dhcp-interface") == 0 || strcmp(line, "except-interface") == 0 ||
				   strcmp(line, "interface") == 0 || strcmp(line, "conf-file") == 0 || strcmp(line, "conf-dir") == 0;
		if (option == NULL) {
			if (relevant) {
				goto done;
			}
			continue;
		}
		/* The portal range has ONE owned tag. Do not silently reinterpret
		 * a conditional reservation depending on the old generated LAN tag. */
		if (strcmp(line, "dhcp-host") == 0 && strstr(option, "tag:") != NULL) {
			goto done;
		}
		if (relevant && (*option == '\0' || strpbrk(option, " \t\r\n#\"'\\") != NULL)) {
			goto done;
		}
		if (strcmp(line, "no-dhcp-interface") == 0 || strcmp(line, "except-interface") == 0 ||
			strcmp(line, "interface") == 0) {
			if (strpbrk(option, "*?[],") != NULL) {
				goto done;
			}
			if (strcmp(line, "interface") == 0) {
				scan->interface_seen = true;
				scan->interface_allowed = scan->interface_allowed || strcmp(option, scan->services->disk.bridge) == 0;
			} else if (strcmp(option, scan->services->disk.bridge) == 0) {
				goto done;
			}
		}
		if (strcmp(line, "conf-file") == 0 && file(scan, option, depth + 1) != 0) {
			goto done;
		}
		if (strcmp(line, "conf-dir") == 0 && directory(scan, option, depth + 1) != 0) {
			goto done;
		}
	}
	result = mesh_stamp_unchanged(scan->services->root, &stamp);
done:
	mesh_sensitive_clear(text, MESH_INPUT_MAX + 1);
	free(text);
	return result;
}

int mesh_mrp_dns_guard(struct mesh_services *services)
{
	struct scan scan = { .services = services };
	if (services == NULL || services->disk.dns_count != 1) {
		return -1;
	}
	snprintf(scan.own, sizeof(scan.own), "%s/mesh11sd-v8-%s.conf", services->disk.dns[0].directory, services->peer->owner);
	if (file(&scan, services->disk.dns[0].config, 0) != 0) {
		return -1;
	}
	return scan.interface_seen && !scan.interface_allowed ? -1 : 0;
}
