/* SPDX-License-Identifier: GPL-2.0-or-later
 * Chunk 10: mesh backhaul LED helpers (v7 set_led / check_mesh_active_led).
 */
#include "mesh11sd.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int mesh_set_led(struct mesh_ctx *ctx, const char *action)
{
	const char *led_action = action;
	DIR *d;
	struct dirent *de;
	char path[192];

	if (!ctx->mesh_backhaul_led[0])
		snprintf(ctx->mesh_backhaul_led, sizeof(ctx->mesh_backhaul_led), "auto");
	if (!strcmp(ctx->mesh_backhaul_led, "none"))
		return 0;
	if (!action || !action[0])
		return 127;

	if (!strcmp(action, "init")) {
		d = opendir("/sys/class/leds");
		if (!d)
			return 0;
		while ((de = readdir(d)) != NULL) {
			if (de->d_name[0] == '.')
				continue;
			snprintf(path, sizeof(path), "/sys/class/leds/%s/trigger", de->d_name);
			{
				FILE *f = fopen(path, "w");
				if (f) { fputs("default-on\n", f); fclose(f); }
			}
		}
		closedir(d);
		sleep(1);
		d = opendir("/sys/class/leds");
		if (!d)
			return 0;
		while ((de = readdir(d)) != NULL) {
			if (de->d_name[0] == '.')
				continue;
			snprintf(path, sizeof(path), "/sys/class/leds/%s/trigger", de->d_name);
			{
				FILE *f = fopen(path, "w");
				if (f) { fputs("none\n", f); fclose(f); }
			}
		}
		closedir(d);
		return 0;
	}

	if (!strcmp(action, "off") || !strcmp(action, "none"))
		led_action = "none";
	else if (!strcmp(action, "on") || !strcmp(action, "default-on"))
		led_action = "default-on";
	else if (!strcmp(action, "beat") || !strcmp(action, "heartbeat"))
		led_action = "heartbeat";

	if (strcmp(ctx->mesh_backhaul_led, "auto") != 0) {
		snprintf(path, sizeof(path), "/sys/class/leds/%s/", ctx->mesh_backhaul_led);
		if (access(path, F_OK) == 0) {
			snprintf(path, sizeof(path), "/sys/class/leds/%s/trigger",
				 ctx->mesh_backhaul_led);
			{
				FILE *f = fopen(path, "w");
				if (f) {
					fprintf(f, "%s\n", led_action);
					fclose(f);
				}
			}
			return 0;
		}
	}

	d = opendir("/sys/class/leds");
	if (!d)
		return 0;
	while ((de = readdir(d)) != NULL) {
		const char *name = de->d_name;
		const char *last = strrchr(name, ':');
		int hit = 0;
		if (name[0] == '.')
			continue;
		last = last ? last + 1 : name;
		if (!strcmp(last, "mesh") || !strcmp(last, "power") ||
		    !strcmp(last, "pwr") || !strcmp(last, "system") ||
		    !strcmp(last, "status") || !strcmp(last, "run") ||
		    !strcmp(last, "wlan") ||
		    !strcmp(name, "green-2") || !strcmp(name, "pwr") ||
		    !strcmp(name, "system") || !strcmp(name, "status") ||
		    !strcmp(name, "run"))
			hit = 1;
		if (!hit)
			continue;
		snprintf(path, sizeof(path), "/sys/class/leds/%s/trigger", name);
		{
			FILE *f = fopen(path, "w");
			if (f) {
				fprintf(f, "%s\n", led_action);
				fclose(f);
			}
		}
		break;
	}
	closedir(d);
	return 0;
}

int mesh_check_mesh_active_led(struct mesh_ctx *ctx)
{
	char out[4096];
	char *argv[] = { "iw", "dev", ctx->active_mesh_ifname, "mpath", "dump", NULL };
	int peers = 0;
	char *p, *line;

	if (!ctx->active_mesh_ifname[0]) {
		if (access(MESH11SD_TMPDIR "/active_mesh_ifname", R_OK) == 0) {
			FILE *f = fopen(MESH11SD_TMPDIR "/active_mesh_ifname", "r");
			char linebuf[128];
			if (f && fgets(linebuf, sizeof(linebuf), f)) {
				char *q = strstr(linebuf, "active_mesh_ifname=\"");
				if (q)
					sscanf(q + 20, "%63[^\"]", ctx->active_mesh_ifname);
			}
			if (f)
				fclose(f);
		}
	}
	if (!ctx->active_mesh_ifname[0])
		return 0;
	if (mesh_run_capture(argv, out, sizeof(out)) != 0) {
		mesh_set_led(ctx, "on");
		return 0;
	}
	p = out;
	while ((line = strsep(&p, "\n")) != NULL) {
		if (!line[0] || strstr(line, "DEST") || strstr(line, "dest"))
			continue;
		peers++;
	}
	if (peers > 0)
		mesh_set_led(ctx, "heartbeat");
	else
		mesh_set_led(ctx, "on");
	return 0;
}
