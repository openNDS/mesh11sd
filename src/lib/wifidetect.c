/* SPDX-License-Identifier: GPL-2.0-or-later
 * wifi_chipset_detect: per-phy wireless capabilities as JSON, written to
 * MESH11SD_TMPDIR/wifidetect. Same key names and layout as v7.
 */
#include "mesh11sd.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define WIFIDETECT_FILE MESH11SD_TMPDIR "/wifidetect"
#define MAX_PHYS 8

/* Driver name (substring) to chipset name, used when sysfs has no label. */
struct chip_name {
	const char *driver;
	const char *match;	/* substring of modalias/compatible, or NULL */
	const char *name;
};

static const struct chip_name chip_table[] = {
	/* MediaTek Filogic / mt79xx (matched on modalias) */
	{ NULL, "7996", "MediaTek MT7996 (Wi-Fi 7)" },
	{ NULL, "798", "MediaTek Filogic 830/810/880 (MT798x integrated WiFi)" },
	{ NULL, "7916", "MediaTek MT7916 (Wi-Fi 6E)" },
	{ NULL, "7915", "MediaTek MT7915E (Wi-Fi 6)" },
	{ NULL, "792", "MediaTek MT792x" },
	{ "mt7996e", NULL, "MediaTek MT7996 (Wi-Fi 7)" },
	/* MediaTek PCIe and USB */
	{ "mt7915e", NULL, "MediaTek MT7915E (Wi-Fi 6)" },
	{ "mt7615e", NULL, "MediaTek MT7615E (Wi-Fi 5)" },
	{ "mt7603e", NULL, "MediaTek MT7603E (2.4 GHz)" },
	{ "mt76x2e", NULL, "MediaTek MT76x2E (5 GHz)" },
	{ "mt76x0u", NULL, "MediaTek MT7610U (Wi-Fi 5 USB) (mt76x0u)" },
	/* MediaTek SoCs with mt76_wmac */
	{ "mt76_wmac", "mt7628", "MediaTek MT7628AN (integrated 2.4 GHz)" },
	{ "mt76_wmac", "mt7621", "MediaTek MT7621AN/AT (integrated 2.4 GHz)" },
	{ "mt76_wmac", "mt7622", "MediaTek MT7622 (integrated 2.4/5 GHz)" },
	{ "mt76_wmac", NULL, "Legacy MediaTek mt76_wmac SoC" },
	{ "mt7622-wmac", NULL, "MediaTek MT7622 (integrated 2.4 GHz)" },
	/* Ralink and Realtek */
	{ "rt2800_wmac", NULL, "MediaTek Ralink Embedded MT7620-RT2880 series(Wi-Fi 4)" },
	{ "rt2800pci", NULL, "MediaTek Ralink RT5370/RT3070 (2.4 GHz, Wi-Fi 4)" },
	{ "rt2800usb", NULL, "MediaTek Ralink RT5370/RT3070 (2.4 GHz, Wi-Fi 4)" },
	{ "rtl8192cu", NULL, "Realtek RTL8192CU (2.4/5 GHz, Wi-Fi 4)" },
	/* Marvell (matched on PCI_ID / SDIO_ID) */
	{ "mwlwifi", "11AB:2A55", "Marvell 88W8864 (2.4/5 GHz, Wi-Fi 5)" },
	{ "mwlwifi", "11AB:2B40", "Marvell 88W8964 (2.4/5 GHz, Wi-Fi 5)" },
	{ "mwlwifi", "11AB:2B38", "Marvell 88W8897 (2.4/5 GHz, Wi-Fi 5)" },
	{ "mwlwifi", NULL, "unknown Marvell Wi-Fi" },
	{ "mwifiex_sdio", "02DF:9135", "Marvell 88W8887 (2.4/5 GHz, Wi-Fi 5)" },
	{ "mwifiex_sdio", NULL, "unknown Marvell Wi-Fi" },
	/* Qualcomm Atheros */
	{ "ath9k", "qca9530-wmac", "Qualcomm Atheros AR953x series (2.4 GHz, Wi-Fi 4)" },
	{ "ath9k", "9556", "Qualcomm Atheros QCA9556 (integrated 2.4 GHz)" },
	{ "ath9k", "9557", "Qualcomm Atheros QCA9557 (integrated 2.4 GHz)" },
	{ "ath9k", "9558", "Qualcomm Atheros QCA9558 (integrated 2.4 GHz)" },
	{ "ath9k", "953", "Qualcomm Atheros QCA9531/QCA9533 (integrated 2.4 GHz)" },
	{ "ath9k", "956", "Qualcomm Atheros QCA9561/QCA9563 (integrated 2.4 GHz)" },
	{ "ath9k", NULL, "Qualcomm Atheros AR93xx/AR92xx/AR5416 series" },
	{ "ath10k_ahb", NULL, "Qualcomm Atheros QCA9886 (AHB)" },
	{ "ath10k_pci", NULL, "Qualcomm Atheros QCA9984/QCA988x/QCA986x series (PCI)" },
	{ "ath11k", "ipq5018-wifi", "Qualcomm IPQ5018 integrated 2.4 GHz" },
	{ "ath11k", "qcn6122", "Qualcomm QCN6122 (Wi-Fi 6E)" },
	{ "ath11k", NULL, "Qualcomm QCN9074/QCN5024" },
	{ "ath12k", NULL, "Qualcomm FastConnect 7800 (WCN785x Wi-Fi 7)" },
	/* Broadcom (matched on modalias or firmware name) */
	{ "brcmfmac", "bcm4329", "Broadcom BCM4329/BCM4330 (Wi-Fi 4)" },
	{ "brcmfmac", "bcm4330", "Broadcom BCM4329/BCM4330 (Wi-Fi 4)" },
	{ "brcmfmac", "bcm43430", "Broadcom BCM43430 (Wi-Fi 5)" },
	{ "brcmfmac", "bcm43455", "Broadcom BCM43455 (Wi-Fi 5)" },
	{ "brcmfmac", "bcm4356", "Broadcom BCM4356 (Wi-Fi 5)" },
	{ "brcmfmac", "bcm4378", "Broadcom BCM4378 (Wi-Fi 6)" },
	{ "brcmfmac", NULL, "Broadcom brcmfmac (Wi-Fi)" },
};

struct phy_caps {
	char name[32];
	char driver[64];
	char chipset[96];
	int band_24, band_5, band_6;
	int std_n, std_ac, std_ax, std_be;
	int mesh_point;
	int nss;
	int max_width;		/* MHz */
	int width_cap;		/* the mt76 80 MHz practical cap was applied */
	int ant_tx, ant_rx;
	int txqs, atf, aql, aql_runtime;
};

/* Read the first line of a file, without the newline. */
static int read_line(const char *path, char *out, size_t outlen)
{
	FILE *f = fopen(path, "r");

	out[0] = '\0';
	if (!f)
		return -1;
	if (!fgets(out, outlen, f))
		out[0] = '\0';
	fclose(f);
	out[strcspn(out, "\n")] = '\0';
	return 0;
}

/* Find "KEY=value" in a uevent style file. */
static void read_key(const char *path, const char *key, char *out, size_t outlen)
{
	FILE *f = fopen(path, "r");
	char line[256];
	size_t klen = strlen(key);

	out[0] = '\0';
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, key, klen) && line[klen] == '=') {
			line[strcspn(line, "\n")] = '\0';
			snprintf(out, outlen, "%s", line + klen + 1);
			break;
		}
	}
	fclose(f);
}

static int popcount(unsigned v)
{
	int c = 0;

	for (; v; v >>= 1)
		c += v & 1;
	return c;
}

/* Bands from the frequency lines ("* 2412.0 MHz [1]") of iw phy info. */
static void scan_band_freqs(struct phy_caps *p, const char *info)
{
	const char *s = info;

	while ((s = strstr(s, " MHz [")) != NULL) {
		const char *b = s;
		int mhz;

		while (b > info && b[-1] != '*' && b[-1] != '\n')
			b--;
		mhz = atoi(b);
		if (mhz >= 2400 && mhz < 2500)
			p->band_24 = 1;
		else if (mhz >= 5000 && mhz < 5950)
			p->band_5 = 1;
		else if (mhz >= 5950 && mhz < 7200)
			p->band_6 = 1;
		s += 6;
	}
}

static void scan_standards(struct phy_caps *p, const char *info)
{
	if (strstr(info, "HT Capabilities") || strstr(info, "HT TX/RX MCS"))
		p->std_n = 1;
	if (strstr(info, "VHT Capabilities") || strstr(info, "VHT RX MCS"))
		p->std_ac = 1;
	if (strstr(info, "HE MAC Capabilities") || strstr(info, "HE Iftypes") ||
	    strstr(info, "HE PHY Capabilities"))
		p->std_ax = 1;
	if (strstr(info, "EHT MAC Capabilities") || strstr(info, "EHT Iftypes") ||
	    strstr(info, "EHT PHY Capabilities"))
		p->std_be = 1;
	/* 802.11ac is 5 GHz only */
	if (p->band_24 && !p->band_5 && !p->band_6)
		p->std_ac = 0;
}

/* Spatial streams: HT MCS index range, then "N streams: MCS" lines. */
static void scan_nss(struct phy_caps *p, const char *info)
{
	const char *s;

	s = strstr(info, "HT TX/RX MCS rate indexes supported:");
	if (s) {
		const char *r = strstr(s, "0-");
		if (r) {
			int top = atoi(r + 2);
			if (top >= 0 && top < 64 && (top + 1) / 8 > p->nss)
				p->nss = (top + 1) / 8;
		}
	}
	for (s = info; (s = strstr(s, " streams: MCS")) != NULL; s++) {
		int n = s[-1] - '0';
		if (n >= 1 && n <= 8 && n > p->nss)
			p->nss = n;
	}
}

static void scan_antennas(struct phy_caps *p, const char *info)
{
	const char *s = strstr(info, "Available Antennas: TX 0x");
	unsigned tx = 0, rx = 0;

	if (!s)
		return;
	if (sscanf(s, "Available Antennas: TX 0x%x RX 0x%x", &tx, &rx) >= 1) {
		p->ant_tx = popcount(tx);
		p->ant_rx = popcount(rx);
	}
}

static int line_has_width(const char *line, size_t len, const char *w)
{
	char buf[256];

	if (len >= sizeof(buf))
		len = sizeof(buf) - 1;
	memcpy(buf, line, len);
	buf[len] = '\0';
	return strstr(buf, w) != NULL;
}

static void scan_width(struct phy_caps *p, const char *info)
{
	const char *radar = strcasestr(info, "radar detect widths:");

	/* mt76 drivers over-report 160 MHz on 5 GHz, so v7 caps them at 80 */
	if (!strncmp(p->driver, "mt76", 4) && p->band_5) {
		p->max_width = 80;
		p->width_cap = 1;
		return;
	}
	if (radar) {
		size_t len = strcspn(radar, "\n");
		static const int widths[] = { 320, 240, 160, 80, 40, 20 };
		char w[8];

		for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
			snprintf(w, sizeof(w), "%d", widths[i]);
			if (line_has_width(radar, len, w)) {
				p->max_width = widths[i];
				break;
			}
		}
	}
	if (!p->max_width) {
		if (strcasestr(info, "320 MHz"))
			p->max_width = 320;
		else if (strcasestr(info, "160 MHz"))
			p->max_width = 160;
		else if (strcasestr(info, "80 MHz") || strstr(info, "80+80"))
			p->max_width = 80;
		else if (strstr(info, "HT40") || strcasestr(info, "40 MHz"))
			p->max_width = 40;
		else
			p->max_width = 20;
	}
	if (p->std_be && strcasestr(info, "320MHz"))
		p->max_width = 320;
	if (p->band_24 && !p->band_5 && !p->band_6 && p->max_width > 40)
		p->max_width = 40;
}

static void scan_features(struct phy_caps *p, const char *info)
{
	char path[128];

	p->txqs = strstr(info, "[ TXQS ]") != NULL;
	p->atf = strstr(info, "[ AIRTIME_FAIRNESS ]") != NULL;
	p->aql = strstr(info, "[ AQL ]") != NULL;
	p->mesh_point = strstr(info, "* mesh point") != NULL;
	snprintf(path, sizeof(path), "/sys/kernel/debug/ieee80211/%s/aql_enable", p->name);
	p->aql_runtime = access(path, F_OK) == 0;
}

/* Chipset: sysfs label or name first, then the table. */
static void find_chipset(struct phy_caps *p, const char *dir)
{
	static const char *const label_files[] = {
		"device/label", "device/name", "mac80211/label", "mac80211/name", NULL
	};
	char path[256], modalias[256] = "", compat[256] = "", ids[64] = "", fw[128] = "";
	DIR *d;
	struct dirent *e;
	size_t i;

	for (i = 0; label_files[i]; i++) {
		snprintf(path, sizeof(path), "%s/%s", dir, label_files[i]);
		if (read_line(path, p->chipset, sizeof(p->chipset)) == 0 && p->chipset[0])
			return;
	}

	snprintf(path, sizeof(path), "%s/device/modalias", dir);
	read_line(path, modalias, sizeof(modalias));
	snprintf(path, sizeof(path), "%s/device/uevent", dir);
	read_key(path, "OF_COMPATIBLE_0", compat, sizeof(compat));
	read_key(path, "PCI_ID", ids, sizeof(ids));
	if (!ids[0])
		read_key(path, "SDIO_ID", ids, sizeof(ids));
	snprintf(path, sizeof(path), "%s/device/firmware", dir);
	d = opendir(path);
	while (d && (e = readdir(d)) != NULL) {
		if (strstr(e->d_name, "brcmfmac")) {
			snprintf(fw, sizeof(fw), "%s", e->d_name);
			break;
		}
	}
	if (d)
		closedir(d);

	for (i = 0; i < sizeof(chip_table) / sizeof(chip_table[0]); i++) {
		const struct chip_name *c = &chip_table[i];

		if (c->driver && !strstr(p->driver, c->driver))
			continue;
		if (!c->driver) {
			/* modalias-only rows are the mt79xx family */
			if (!strcasestr(modalias, "mt79") ||
			    !strcasestr(modalias, c->match))
				continue;
		} else if (c->match && !strcasestr(modalias, c->match) &&
			   !strcasestr(compat, c->match) &&
			   !strcasestr(ids, c->match) && !strcasestr(fw, c->match)) {
			continue;
		}
		snprintf(p->chipset, sizeof(p->chipset), "%s", c->name);
		return;
	}
	snprintf(p->chipset, sizeof(p->chipset), "unknown");
}

static void probe_phy(struct phy_caps *p, const char *name)
{
	static char info[65536];
	char dir[128], path[192];
	char *iw[] = { "iw", "phy", (char *)name, "info", NULL };

	memset(p, 0, sizeof(*p));
	snprintf(p->name, sizeof(p->name), "%s", name);
	snprintf(dir, sizeof(dir), "/sys/class/ieee80211/%s", name);
	snprintf(path, sizeof(path), "%s/device/uevent", dir);
	read_key(path, "DRIVER", p->driver, sizeof(p->driver));
	if (!p->driver[0])
		snprintf(p->driver, sizeof(p->driver), "n/a");

	info[0] = '\0';
	mesh_run_capture(iw, info, sizeof(info));
	scan_band_freqs(p, info);
	scan_standards(p, info);
	scan_nss(p, info);
	scan_antennas(p, info);
	if (!p->nss && p->ant_tx)
		p->nss = p->ant_tx;
	scan_width(p, info);
	scan_features(p, info);
	find_chipset(p, dir);
}

static void json_str(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			fputc('\\', f);
		if ((unsigned char)*s >= 0x20)
			fputc(*s, f);
	}
	fputc('"', f);
}

static const char *wifi_generation(const struct phy_caps *p)
{
	if (p->std_be)
		return "Wi-Fi 7";
	if (p->std_ax)
		return p->band_6 ? "Wi-Fi 6E" : "Wi-Fi 6";
	if (p->std_ac)
		return "Wi-Fi 5";
	if (p->std_n)
		return "Wi-Fi 4";
	return "Legacy";
}

/* Model name, skipping LED-like names v7 also skipped. */
static void get_device_name(char *out, size_t outlen)
{
	FILE *f = fopen("/etc/board.json", "r");
	char line[256];

	out[0] = '\0';
	if (f) {
		while (fgets(line, sizeof(line), f)) {
			char *p = strstr(line, "\"name\":");
			if (!p)
				continue;
			p = strchr(p + 7, '"');
			if (p && sscanf(p + 1, "%127[^\"]", out) == 1)
				break;
		}
		fclose(f);
		if (out[0] && (strstr(out, "WAN") || strstr(out, "LAN") ||
			       strstr(out, "WLAN") || strstr(out, "POWER") ||
			       strstr(out, "USB") || strstr(out, "WPS")))
			out[0] = '\0';
		return;
	}
	if (read_line("/tmp/sysinfo/model", out, outlen) == 0 && out[0])
		return;
	if (read_line("/tmp/sysinfo/board_name", out, outlen) == 0 && out[0])
		return;
	snprintf(out, outlen, "unknown");
}

static void release_field(const char *key, char *out, size_t outlen)
{
	FILE *f = fopen("/etc/openwrt_release", "r");
	char line[256];
	size_t klen = strlen(key);

	snprintf(out, outlen, "unknown");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, key, klen) && line[klen] == '=') {
			char *v = line + klen + 1;

			v[strcspn(v, "\n")] = '\0';
			if (*v == '\'' || *v == '"') {
				char q = *v++;
				char *end = strrchr(v, q);
				if (end)
					*end = '\0';
			}
			if (*v)
				snprintf(out, outlen, "%s", v);
			break;
		}
	}
	fclose(f);
}

/*
 * Theoretical maximum PHY rate in Mbit/s, the figure manufacturers
 * quote. v7 rates per spatial stream at 40 MHz, scaled by channel width.
 */
static int theoretical_mbps(const struct phy_caps *p)
{
	int base, mult;	/* mult is relative to 40 MHz, times 100 */
	int streams = p->nss ? p->nss : 1;

	if (p->std_be)
		base = 360;
	else if (p->std_ax)
		base = 300;
	else if (p->std_ac)
		base = 217;
	else if (p->std_n)
		base = 150;
	else
		return 0;

	switch (p->max_width) {
	case 320: mult = 800; break;
	case 240: mult = 600; break;
	case 160: mult = 400; break;
	case 80:  mult = 200; break;
	case 20:  mult = 50;  break;
	default:  mult = 100; break;
	}
	return base * streams * mult / 100;
}

/* Wi-Fi 7 MLO: sum of each band at its widest (2.4 at 40, 5 at 160, 6 at 320). */
static int aggregated_mlo_mbps(const struct phy_caps *p)
{
	int streams = p->nss ? p->nss : 1;
	int total = 0;

	if (p->band_24)
		total += 360 * streams;
	if (p->band_5)
		total += 1440 * streams;
	if (p->band_6)
		total += 2880 * streams;
	return total;
}

static int cmp_names(const void *a, const void *b)
{
	return strcmp((const char *)a, (const char *)b);
}

int mesh_wifi_chipset_detect(struct mesh_ctx *ctx)
{
	static const char *const rel_keys[] = {
		"DISTRIB_ID", "DISTRIB_RELEASE", "DISTRIB_REVISION",
		"DISTRIB_TARGET", "DISTRIB_ARCH", "DISTRIB_DESCRIPTION", NULL
	};
	static const char *const rel_names[] = {
		"Distribution", "Release", "Revision",
		"Target", "Architecture", "Description", NULL
	};
	char names[MAX_PHYS][32];
	int count = 0, i;
	char labelmac[32] = "", val[160];
	DIR *d;
	struct dirent *e;
	FILE *f;

	d = opendir("/sys/class/ieee80211");
	while (d && (e = readdir(d)) != NULL && count < MAX_PHYS) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(names[count++], sizeof(names[0]), "%s", e->d_name);
	}
	if (d)
		closedir(d);
	qsort(names, count, sizeof(names[0]), cmp_names);

	mesh_get_factory_mac(ctx, labelmac, sizeof(labelmac));
	f = fopen(WIFIDETECT_FILE, "w");
	if (!f)
		return 1;

	fprintf(f, "{\n  \"System@%s\": {\n", labelmac);
	for (i = 0; rel_keys[i]; i++) {
		release_field(rel_keys[i], val, sizeof(val));
		fprintf(f, "    \"%s\": ", rel_names[i]);
		json_str(f, val);
		fputs(",\n", f);
	}
	get_device_name(val, sizeof(val));
	if (val[0]) {
		fputs("    \"Device\": ", f);
		json_str(f, val);
		fputs(",\n", f);
	}
	fputs("    \"phy\": {\n", f);

	for (i = 0; i < count; i++) {
		struct phy_caps p;
		const char *sep = "";

		probe_phy(&p, names[i]);
		fprintf(f, "      \"%s\": {\n        \"Chipset\": ", p.name);
		json_str(f, p.chipset);
		fputs(",\n        \"Bands\": [", f);
		if (p.band_24) { fprintf(f, "%s\n          \"2.4GHz\"", sep); sep = ","; }
		if (p.band_5)  { fprintf(f, "%s\n          \"5GHz\"", sep); sep = ","; }
		if (p.band_6)  { fprintf(f, "%s\n          \"6GHz\"", sep); sep = ","; }
		if (!*sep)
			fputs("\n          \"unknown\"", f);
		fputs("\n        ],\n        \"Standards\": [", f);
		sep = "";
		if (p.std_n)  { fprintf(f, "%s\n          \"802.11n\"", sep); sep = ","; }
		if (p.std_ac) { fprintf(f, "%s\n          \"802.11ac\"", sep); sep = ","; }
		if (p.std_ax) { fprintf(f, "%s\n          \"802.11ax\"", sep); sep = ","; }
		if (p.std_be) { fprintf(f, "%s\n          \"802.11be\"", sep); sep = ","; }
		if (!*sep)
			fputs("\n          \"802.11legacy\"", f);
		fputs("\n        ],\n", f);
		fprintf(f, "        \"MeshPoint\": \"%s\",\n", p.mesh_point ? "802.11s" : "N/A");
		fprintf(f, "        \"Wi-FiGeneration\": \"%s\",\n", wifi_generation(&p));
		if (p.nss)
			fprintf(f, "        \"MIMO\": \"%dx%d\",\n", p.nss, p.nss);
		else
			fputs("        \"MIMO\": \"unknown\",\n", f);
		fprintf(f, "        \"MaxChannelWidth\": \"%d MHz\",\n", p.max_width);
		fprintf(f, "        \"ChannelWidthCap\": %s,\n", p.width_cap ? "true" : "false");
		if (p.ant_tx && p.ant_rx)
			fprintf(f, "        \"Antennas\": \"%dx%d\",\n", p.ant_tx, p.ant_rx);
		else if (p.nss)
			fprintf(f, "        \"Antennas\": \"%dx%d\",\n", p.nss, p.nss);
		else
			fputs("        \"Antennas\": \"unknown\",\n", f);
		{
			int mbps = theoretical_mbps(&p);

			if (mbps)
				fprintf(f, "        \"TheoreticalMaxMbps\": \"%d\",\n", mbps);
			else
				fputs("        \"TheoreticalMaxMbps\": \"unknown\",\n", f);
			if (p.std_be) {
				int agg = aggregated_mlo_mbps(&p);

				if (agg)
					fprintf(f, "        \"Aggregated_MLO_MaxMbps\": \"%d\",\n", agg);
				else
					fputs("        \"Aggregated_MLO_MaxMbps\": \"unknown\",\n", f);
			}
		}
		fprintf(f, "        \"TXQS\": %s,\n", p.txqs ? "true" : "false");
		fprintf(f, "        \"AIRTIME_FAIRNESS\": %s,\n", p.atf ? "true" : "false");
		fprintf(f, "        \"AQL_Extended\": %s,\n", p.aql ? "true" : "false");
		fprintf(f, "        \"AQL_Runtime\": %s,\n", p.aql_runtime ? "true" : "false");
		fputs("        \"Driver\": ", f);
		json_str(f, p.driver);
		fprintf(f, "\n      }%s\n", i + 1 < count ? "," : "");
	}
	fputs("    }\n  }\n}\n", f);
	fclose(f);
	return 0;
}

/*
 * When apmond is installed, put the wifidetect json into this node's
 * apmond reports as the "wifidetect" extension block. apmond keeps
 * blocks in RAM only, so it is set again every few minutes with a ttl
 * of three refreshes; if mesh11sd stops, the block expires by itself.
 */
#define APMOND_REFRESH_S 300
#define APMOND_BLOCK_MAX 16384

void mesh_wifidetect_to_apmond(struct mesh_ctx *ctx)
{
	static time_t last;
	static char data[APMOND_BLOCK_MAX + 1];
	static char arg[APMOND_BLOCK_MAX + 128];
	char *argv[] = { "ubus", "call", "apmond", "add_block", arg, NULL };
	time_t now = time(NULL);
	size_t n;
	FILE *f;

	if (last && now - last < APMOND_REFRESH_S)
		return;
	if (access("/usr/sbin/apmond", X_OK) != 0)
		return;
	last = now;

	if (access(WIFIDETECT_FILE, R_OK) != 0 && mesh_wifi_chipset_detect(ctx) != 0)
		return;
	f = fopen(WIFIDETECT_FILE, "r");
	if (!f)
		return;
	n = fread(data, 1, sizeof(data), f);
	fclose(f);
	if (n == 0 || n > APMOND_BLOCK_MAX) {
		mesh_log_info(ctx, "wifidetect is [ %zu ] bytes, not sent to apmond", n);
		return;
	}
	data[n] = '\0';
	snprintf(arg, sizeof(arg), "{\"name\":\"wifidetect\",\"data\":%s,\"ttl\":%d}",
		 data, 3 * APMOND_REFRESH_S);
	if (mesh_run_capture(argv, NULL, 0) == 0)
		mesh_log_debug(ctx, "wifidetect block sent to apmond");
	else
		mesh_log_debug(ctx, "apmond did not take the wifidetect block");
}
