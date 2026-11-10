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
	char cmd[128];
	FILE *f;
	char line[256];
	char *p;

	if (!dev || !dev[0])
		return -1;
	snprintf(cmd, sizeof(cmd), "ip link show '%s' 2>/dev/null", dev);
	f = popen(cmd, "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		p = strstr(line, "link/ether ");
		if (!p)
			continue;
		p += strlen("link/ether ");
		sscanf(p, "%31s", mac);
		pclose(f);
		if (mac_looks_valid(mac))
			return 0;
		return -1;
	}
	pclose(f);
	(void)maclen;
	return -1;
}

int mesh_get_factory_mac(struct mesh_ctx *ctx, char *mac_out, size_t mac_len)
{
	char mac[32];
	char buf[64];
	FILE *f;
	const char *fallbacks[] = { "eth1", "eth0", "lan0", "lan1", NULL };
	int i;

	if (!mac_out || mac_len < 18)
		return -1;
	mac_out[0] = '\0';
	mac[0] = '\0';

	/* 1. OpenWrt get_mac_label via system.sh */
	f = popen(". /lib/functions/system.sh 2>/dev/null && get_mac_label 2>/dev/null", "r");
	if (f) {
		if (fgets(mac, sizeof(mac), f)) {
			size_t n = strlen(mac);
			while (n > 0 && (mac[n - 1] == '\n' || mac[n - 1] == '\r'))
				mac[--n] = '\0';
		}
		pclose(f);
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
	f = popen("ip -o link show type ether 2>/dev/null | head -n1", "r");
	if (f) {
		char line[256];
		char dev[64];
		if (fgets(line, sizeof(line), f)) {
			/* "2: eth0: <...> ..." */
			if (sscanf(line, "%*d: %63[^:]", dev) == 1) {
				if (read_link_ether(dev, mac, sizeof(mac)) == 0) {
					pclose(f);
					mesh_write_to_syslog(ctx, "info",
						"Factory MAC obtained from first detected Ethernet interface");
					snprintf(mac_out, mac_len, "%s", mac);
					return 0;
				}
			}
		}
		pclose(f);
	}

	mesh_write_to_syslog(ctx, "warn", "Could not determine factory MAC address");
	return -1;
}
