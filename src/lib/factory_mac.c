/* SPDX-License-Identifier: GPL-2.0-or-later
 * v7 get_factory_mac() order: system get_mac_label, network.wan.device,
 * eth1/eth0/lan0/lan1, first ether link.
 */
#include "mesh11sd.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int mac_looks_valid(const char *mac)
{
	size_t n;
	size_t i;

	if (!mac)
		return 0;
	n = strlen(mac);
	if (n != 17)
		return 0;
	for (i = 0; i < n; i++) {
		if ((i % 3) == 2) {
			if (mac[i] != ':')
				return 0;
		} else if (!isxdigit((unsigned char)mac[i])) {
			return 0;
		}
	}
	return 1;
}

static int read_link_ether(const char *dev, char *mac, size_t maclen)
{
	char out[1024];
	char *argv[] = { "ip", "link", "show", (char *)dev, NULL };
	char *p;

	if (!dev || !dev[0])
		return -1;
	if (mesh_run_capture(argv, out, sizeof(out)) != 0)
		return -1;
	p = strstr(out, "link/ether ");
	if (!p)
		return -1;
	sscanf(p + strlen("link/ether "), "%31s", mac);
	(void)maclen;
	return mac_looks_valid(mac) ? 0 : -1;
}

int mesh_get_factory_mac(struct mesh_ctx *ctx, char *mac_out, size_t mac_len)
{
	char mac[32];
	char buf[64];
	const char *fallbacks[] = { "eth1", "eth0", "lan0", "lan1", NULL };
	int i;

	if (!mac_out || mac_len < 18)
		return -1;
	mac_out[0] = '\0';
	mac[0] = '\0';

	/* 1. OpenWrt get_mac_label via system.sh (fixed command, no data in the string) */
	{
		char *argv[] = {
			"sh", "-c",
			". /lib/functions/system.sh 2>/dev/null && get_mac_label 2>/dev/null",
			NULL
		};

		mesh_run_capture(argv, mac, sizeof(mac));
		if (mac_looks_valid(mac)) {
			mesh_write_to_syslog(ctx, "info", "Factory MAC obtained from system function call");
			snprintf(mac_out, mac_len, "%s", mac);
			return 0;
		}
	}

	/* 2. network.wan.device */
	if (mesh_uci_get("network.wan.device", buf, sizeof(buf)) == 0 && buf[0]) {
		if (read_link_ether(buf, mac, sizeof(mac)) == 0) {
			mesh_write_to_syslog(ctx, "info", "Factory MAC obtained from network.wan.device");
			snprintf(mac_out, mac_len, "%s", mac);
			return 0;
		}
	}

	/* 3. common fallbacks */
	for (i = 0; fallbacks[i]; i++) {
		if (read_link_ether(fallbacks[i], mac, sizeof(mac)) == 0) {
			mesh_write_to_syslog(ctx, "info", "Factory MAC obtained from fallback interface");
			snprintf(mac_out, mac_len, "%s", mac);
			return 0;
		}
	}

	/* 4. first ether */
	{
		char out[4096];
		char dev[64];
		char *nl;
		char *argv[] = { "ip", "-o", "link", "show", "type", "ether", NULL };

		if (mesh_run_capture(argv, out, sizeof(out)) == 0 && out[0]) {
			nl = strchr(out, '\n');
			if (nl)
				*nl = '\0';
			/* "2: eth0: <...> ..." */
			if (sscanf(out, "%*d: %63[^:]", dev) == 1 &&
			    read_link_ether(dev, mac, sizeof(mac)) == 0) {
				mesh_write_to_syslog(ctx, "info",
					"Factory MAC obtained from first detected Ethernet interface");
				snprintf(mac_out, mac_len, "%s", mac);
				return 0;
			}
		}
	}

	mesh_write_to_syslog(ctx, "warn", "Could not determine factory MAC address");
	return -1;
}
