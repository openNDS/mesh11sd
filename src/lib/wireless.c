/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mesh11sd.h"

#include <ctype.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int mesh_get_mesh_iflist(struct mesh_ctx *ctx)
{
	char out[4096];
	char *argv[] = { "iw", "dev", NULL };
	char *p, *line;
	char iface[64];

	ctx->iflist_count = 0;
	if (mesh_run_capture(argv, out, sizeof(out)) != 0)
		return -1;
	p = out;
	while ((line = strsep(&p, "\n")) != NULL) {
		char *ip;
		if (!(ip = strstr(line, "Interface ")))
			continue;
		ip += 10;
		while (*ip == ' ')
			ip++;
		sscanf(ip, "%63s", iface);
		{
			char info[1024];
			char *iargv[] = { "iw", "dev", iface, "info", NULL };
			if (mesh_run_capture(iargv, info, sizeof(info)) != 0)
				continue;
			if (strstr(info, "type mesh") || strstr(info, "type Mesh") ||
			    strstr(info, "\ttype mesh")) {
				if (ctx->iflist_count < MESH11SD_MAX_IFACES) {
					snprintf(ctx->iflist[ctx->iflist_count],
						 sizeof(ctx->iflist[0]), "%s", iface);
					ctx->iflist_count++;
				}
			}
		}
	}
	return 0;
}

int mesh_iface_is_up(const char *ifname)
{
	char out[512];
	char *argv[] = { "ip", "link", "show", (char *)ifname, NULL };

	if (!ifname || !ifname[0])
		return 0;
	if (mesh_run_capture(argv, out, sizeof(out)) != 0)
		return 0;
	if (strstr(out, "state UP") || strstr(out, "state UNKNOWN"))
		return 1;
	return 0;
}

/* Stream `iw phy <phy> info` line-by-line. Full dump on Wi-Fi 7 can exceed 32KB
 * (mt7996e ~730+ lines); fixed buffers truncate before late sections. */
int mesh_phy_info_has(const char *phy, const char *needle)
{
	char *argv[] = { "iw", "phy", (char *)phy, "info", NULL };
	int pipefd[2];
	pid_t pid;
	FILE *fp;
	char line[1024];
	int found = 0;
	int status;

	if (!phy || !phy[0] || !needle || !needle[0])
		return 0;
	if (pipe(pipefd) < 0)
		return 0;
	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return 0;
	}
	if (pid == 0) {
		int dn;
		close(pipefd[0]);
		dup2(pipefd[1], 1);
		close(pipefd[1]);
		dn = open("/dev/null", O_WRONLY);
		if (dn >= 0) {
			dup2(dn, 2);
			close(dn);
		}
		execvp(argv[0], argv);
		_exit(127);
	}
	close(pipefd[1]);
	fp = fdopen(pipefd[0], "r");
	if (!fp) {
		close(pipefd[0]);
		waitpid(pid, &status, 0);
		return 0;
	}
	while (fgets(line, sizeof(line), fp)) {
		if (strstr(line, needle)) {
			found = 1;
			break;
		}
	}
	fclose(fp);
	waitpid(pid, &status, 0);
	return found;
}

/* v7 mobility_settings multi-band check:
 *   multi_band=$(iw phy "$phy" info | sed -n '/Supported wiphy radios/,/Globally valid interface combinations/{
 *     /Supported wiphy radios/d
 *     /Globally valid interface combinations/q
 *     p
 *   }')
 * Non-empty body between the two markers => MLO multi-band wiphy.
 */
int mesh_phy_is_multi_band(const char *phy)
{
	char *argv[] = { "iw", "phy", (char *)phy, "info", NULL };
	int pipefd[2];
	pid_t pid;
	FILE *fp;
	char line[1024];
	int in_section = 0;
	int has_body = 0;
	int status;

	if (!phy || !phy[0])
		return 0;
	if (pipe(pipefd) < 0)
		return 0;
	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return 0;
	}
	if (pid == 0) {
		int dn;
		close(pipefd[0]);
		dup2(pipefd[1], 1);
		close(pipefd[1]);
		dn = open("/dev/null", O_WRONLY);
		if (dn >= 0) {
			dup2(dn, 2);
			close(dn);
		}
		execvp(argv[0], argv);
		_exit(127);
	}
	close(pipefd[1]);
	fp = fdopen(pipefd[0], "r");
	if (!fp) {
		close(pipefd[0]);
		waitpid(pid, &status, 0);
		return 0;
	}
	while (fgets(line, sizeof(line), fp)) {
		if (!in_section) {
			if (strstr(line, "Supported wiphy radios"))
				in_section = 1;
			continue;
		}
		if (strstr(line, "Globally valid interface combinations"))
			break;
		/* v7 sed `p`: any line in the range counts (blank or not) */
		has_body = 1;
	}
	fclose(fp);
	waitpid(pid, &status, 0);
	return has_body;
}

int mesh_ensure_iface_arp_multicast(struct mesh_ctx *ctx, const char *ifname)
{
	char out[1024];
	char *argv[] = { "ip", "link", "show", (char *)ifname, NULL };
	int arp_on, mcast_on;

	if (!ifname || !ifname[0])
		return -1;
	if (mesh_run_capture(argv, out, sizeof(out)) != 0)
		return -1;
	/* ip link lists NOARP when ARP is off; MULTICAST when multicast is on */
	arp_on = (strstr(out, "NOARP") == NULL);
	mcast_on = (strstr(out, "MULTICAST") != NULL);
	if (!arp_on) {
		char *a[] = { "ip", "link", "set", (char *)ifname, "arp", "on", NULL };

		mesh_run_capture(a, NULL, 0);
		mesh_log_info(ctx, "Old value:%s arp=off, Setting new value:arp=on",
			      ifname);
	}
	if (!mcast_on) {
		char *a[] = { "ip", "link", "set", (char *)ifname, "multicast", "on", NULL };

		mesh_run_capture(a, NULL, 0);
		mesh_log_info(ctx,
			      "Old value:%s multicast=off, Setting new value:multicast=on",
			      ifname);
	}
	return 0;
}

int mesh_wait_for_interface(struct mesh_ctx *ctx, const char *ifname,
			    char *status_out, size_t status_len)
{
	int i;
	char out[512];
	char *argv[] = { "ip", "link", "show", (char *)ifname, NULL };

	snprintf(status_out, status_len, "down");
	for (i = 0; i < ctx->interface_timeout; i++) {
		if (mesh_run_capture(argv, out, sizeof(out)) == 0) {
			if (strstr(out, "state UP") || strstr(out, "state UNKNOWN")) {
				snprintf(status_out, status_len, "up");
				return 0;
			}
		}
		sleep(1);
	}
	mesh_log_debug(ctx, "%s is not up - giving up for now.", ifname);
	return -1;
}


/* After `wifi`, wait until every enabled AP/mesh wifi-iface is live in
 * hostapd or wpa_supplicant. On a shared wiphy (mt7996e phy0 = 2g+5g)
 * a burst of phy/bridge/mesh_param writes during 5g bring-up leaves
 * the AP ENABLED but unable to associate. v7 only avoids this because
 * its shell is slow enough that 5g is already up. */
#define WIFI_ENABLED_WAIT_TRIES	60	/* 30s at 500ms */
#define WIFI_ENABLED_POLL_US	500000
#define WIFI_ENABLED_SETTLE_S	2

static int wifi_section_from_mode_line(const char *line, char *sect, size_t sectlen)
{
	const char *dot, *dot2, *eq;

	if (!line || !sect || sectlen == 0)
		return -1;
	sect[0] = '\0';
	dot = strchr(line, '.');
	if (!dot)
		return -1;
	dot2 = strchr(dot + 1, '.');
	eq = strchr(line, '=');
	if (!dot2 || !eq || dot2 > eq)
		return -1;
	{
		size_t n = (size_t)(dot2 - (dot + 1));
		if (n >= sectlen)
			n = sectlen - 1;
		memcpy(sect, dot + 1, n);
		sect[n] = '\0';
	}
	return sect[0] ? 0 : -1;
}

static int uci_is_disabled(const char *tuple)
{
	char val[32];

	if (mesh_uci_get(tuple, val, sizeof(val)) != 0 || !val[0])
		return 0;
	return atoi(val) != 0;
}

static int mesh_iface_is_ready(const char *ifname)
{
	char info[2048];
	char *argv[] = { "iw", "dev", (char *)ifname, "info", NULL };

	if (!ifname || !ifname[0])
		return 0;
	if (mesh_run_capture(argv, info, sizeof(info)) != 0)
		return 0;
	if (!strstr(info, "type mesh point") && !strstr(info, "type Mesh"))
		return 0;
	/* A channel means the mesh vif has finished starting */
	if (strstr(info, "channel "))
		return 1;
	return mesh_iface_is_up(ifname);
}

static int tally_uci_show_mode_line(const char *line, int *nap, int *nmesh,
				    char mesh_ifs[][32], int max_mesh, int *nifs)
{
	char sect[96], key[160], device[64], ifname[64];
	int is_ap = (strstr(line, ".mode='ap'") != NULL ||
		     strstr(line, ".mode=ap") != NULL);
	int is_mesh = (strstr(line, ".mode='mesh'") != NULL ||
		       strstr(line, ".mode=mesh") != NULL);

	if (!is_ap && !is_mesh)
		return 0;
	if (wifi_section_from_mode_line(line, sect, sizeof(sect)) != 0)
		return 0;
	snprintf(key, sizeof(key), "wireless.%s.disabled", sect);
	if (uci_is_disabled(key))
		return 0;
	snprintf(key, sizeof(key), "wireless.%s.device", sect);
	if (mesh_uci_get(key, device, sizeof(device)) == 0 && device[0]) {
		snprintf(key, sizeof(key), "wireless.%s.disabled", device);
		if (uci_is_disabled(key))
			return 0;
	}
	if (is_ap)
		(*nap)++;
	if (is_mesh) {
		(*nmesh)++;
		snprintf(key, sizeof(key), "wireless.%s.ifname", sect);
		ifname[0] = '\0';
		if (mesh_uci_get(key, ifname, sizeof(ifname)) == 0 &&
		    ifname[0] && *nifs < max_mesh) {
			snprintf(mesh_ifs[*nifs], 32, "%s", ifname);
			(*nifs)++;
		}
	}
	return 1;
}

static int count_from_uci_show_stream(int *nap, int *nmesh,
				      char mesh_ifs[][32], int max_mesh, int *nifs)
{
	char *argv[] = { "uci", "show", "wireless", NULL };
	int pipefd[2];
	pid_t pid;
	FILE *fp;
	char line[512];
	int status;

	if (pipe(pipefd) < 0)
		return -1;
	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (pid == 0) {
		dup2(pipefd[1], STDOUT_FILENO);
		close(pipefd[0]);
		close(pipefd[1]);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(pipefd[1]);
	fp = fdopen(pipefd[0], "r");
	if (!fp) {
		close(pipefd[0]);
		waitpid(pid, &status, 0);
		return -1;
	}
	while (fgets(line, sizeof(line), fp)) {
		line[strcspn(line, "\n")] = '\0';
		tally_uci_show_mode_line(line, nap, nmesh, mesh_ifs, max_mesh, nifs);
	}
	fclose(fp);
	waitpid(pid, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;
	return 0;
}

static int parse_config_option(const char *line, const char *name,
			       char *out, size_t outlen)
{
	const char *p = line;
	size_t nlen = strlen(name);

	while (*p == ' ' || *p == '\t')
		p++;
	if (strncmp(p, "option ", 7) != 0)
		return -1;
	p += 7;
	while (*p == ' ' || *p == '\t')
		p++;
	if (strncmp(p, name, nlen) != 0)
		return -1;
	p += nlen;
	if (*p != ' ' && *p != '\t')
		return -1;
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '\'' || *p == '"') {
		char q = *p++;
		size_t i = 0;

		while (*p && *p != q && i + 1 < outlen)
			out[i++] = *p++;
		out[i] = '\0';
		return 0;
	}
	{
		size_t i = 0;

		while (*p && *p != ' ' && *p != '\t' && *p != '\n' && i + 1 < outlen)
			out[i++] = *p++;
		out[i] = '\0';
	}
	return out[0] ? 0 : -1;
}

static int radio_disabled_lookup(char names[][32], const int *dis, int n,
				 const char *radio)
{
	int i;

	if (!radio || !radio[0])
		return 0;
	for (i = 0; i < n; i++) {
		if (!strcmp(names[i], radio))
			return dis[i];
	}
	return 0;
}

static int count_from_wireless_file(const char *path, int *nap, int *nmesh,
				    char mesh_ifs[][32], int max_mesh, int *nifs)
{
	FILE *f;
	char line[256];
	char kind = 0;
	char name[32] = "", mode[16] = "", disabled[8] = "";
	char ifname[64] = "", device[64] = "";
	char radio_names[16][32];
	int radio_dis[16];
	int nrad = 0;

	f = fopen(path, "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		const char *p = line;

		while (*p == ' ' || *p == '\t')
			p++;
		if (!strncmp(p, "config ", 7)) {
			if (kind == 'i' && mode[0] && atoi(disabled) == 0 &&
			    !radio_disabled_lookup(radio_names, radio_dis, nrad, device)) {
				int is_ap = !strcmp(mode, "ap");
				int is_mesh = !strcmp(mode, "mesh");

				if (is_ap)
					(*nap)++;
				if (is_mesh) {
					(*nmesh)++;
					if (ifname[0] && *nifs < max_mesh) {
						snprintf(mesh_ifs[*nifs], 32, "%s", ifname);
						(*nifs)++;
					}
				}
			}
			if (kind == 'd' && name[0] && nrad < 16) {
				snprintf(radio_names[nrad], 32, "%s", name);
				radio_dis[nrad] = atoi(disabled) != 0;
				nrad++;
			}
			kind = 0;
			name[0] = mode[0] = disabled[0] = ifname[0] = device[0] = '\0';
			if (strstr(p, "wifi-device")) {
				const char *q = strchr(p, '\'');
				kind = 'd';
				if (q)
					sscanf(q + 1, "%31[^']", name);
			} else if (strstr(p, "wifi-iface")) {
				kind = 'i';
			}
			continue;
		}
		if (kind == 'd')
			parse_config_option(p, "disabled", disabled, sizeof(disabled));
		if (kind == 'i') {
			parse_config_option(p, "mode", mode, sizeof(mode));
			parse_config_option(p, "disabled", disabled, sizeof(disabled));
			parse_config_option(p, "ifname", ifname, sizeof(ifname));
			parse_config_option(p, "device", device, sizeof(device));
		}
	}
	if (kind == 'i' && mode[0] && atoi(disabled) == 0 &&
	    !radio_disabled_lookup(radio_names, radio_dis, nrad, device)) {
		if (!strcmp(mode, "ap"))
			(*nap)++;
		if (!strcmp(mode, "mesh")) {
			(*nmesh)++;
			if (ifname[0] && *nifs < max_mesh) {
				snprintf(mesh_ifs[*nifs], 32, "%s", ifname);
				(*nifs)++;
			}
		}
	}
	fclose(f);
	return 0;
}

static int count_enabled_ap_mesh_ifaces(int *ap_out, int *mesh_out,
				       char mesh_ifs[][32], int max_mesh)
{
	int nap = 0, nmesh = 0, nifs = 0;

	if (ap_out)
		*ap_out = 0;
	if (mesh_out)
		*mesh_out = 0;
	if (count_from_uci_show_stream(&nap, &nmesh, mesh_ifs, max_mesh, &nifs) != 0 ||
	    nap + nmesh == 0) {
		nap = nmesh = nifs = 0;
		memset(mesh_ifs, 0, sizeof(mesh_ifs[0]) * (size_t)max_mesh);
		count_from_wireless_file("/etc/config/wireless",
					 &nap, &nmesh, mesh_ifs, max_mesh, &nifs);
	}
	if (ap_out)
		*ap_out = nap;
	if (mesh_out)
		*mesh_out = nmesh;
	return nap + nmesh;
}

static int hostapd_object_enabled(const char *obj)
{
	char name[128];
	char out[2048];
	char *argv[] = { "ubus", "call", name, "get_status", NULL };

	if (!obj || !obj[0] || strlen(obj) >= sizeof(name))
		return 0;
	snprintf(name, sizeof(name), "%s", obj);
	if (mesh_run_capture(argv, out, sizeof(out)) != 0)
		return 0;
	if (strstr(out, "ENABLED"))
		return 1;
	return 0;
}

static int count_hostapd_enabled(void)
{
	char raw[16384];
	char *argv[] = { "ubus", "list", NULL };
	char *p, *line;
	int ready = 0;

	if (mesh_run_capture(argv, raw, sizeof(raw)) != 0)
		return 0;
	p = raw;
	while ((line = strsep(&p, "\n")) != NULL) {
		if (strncmp(line, "hostapd.", 8) != 0)
			continue;
		if (hostapd_object_enabled(line))
			ready++;
	}
	return ready;
}

int mesh_wait_for_wifi_enabled(struct mesh_ctx *ctx)
{
	int expected_ap = 0, expected_mesh = 0;
	char mesh_ifs[MESH11SD_MAX_IFACES][32];
	int tries;
	int ready_ap = 0, ready_mesh = 0;
	int ok = 0;
	int i;

	memset(mesh_ifs, 0, sizeof(mesh_ifs));

	for (tries = 0; tries < WIFI_ENABLED_WAIT_TRIES; tries++) {
		if (tries > 0)
			usleep(WIFI_ENABLED_POLL_US);
		memset(mesh_ifs, 0, sizeof(mesh_ifs));
		expected_ap = expected_mesh = 0;
		count_enabled_ap_mesh_ifaces(&expected_ap, &expected_mesh,
					     mesh_ifs, MESH11SD_MAX_IFACES);
		if (expected_ap + expected_mesh <= 0)
			continue;
		ready_ap = count_hostapd_enabled();
		ready_mesh = 0;
		for (i = 0; i < MESH11SD_MAX_IFACES && mesh_ifs[i][0]; i++) {
			if (mesh_iface_is_ready(mesh_ifs[i]))
				ready_mesh++;
		}
		/* If UCI has no ifname yet, fall back to the live mesh iflist */
		if (expected_mesh > 0 && !mesh_ifs[0][0]) {
			mesh_get_mesh_iflist(ctx);
			for (i = 0; i < ctx->iflist_count; i++) {
				if (mesh_iface_is_ready(ctx->iflist[i]))
					ready_mesh++;
			}
		}
		if (ready_ap >= expected_ap && ready_mesh >= expected_mesh) {
			ok = 1;
			break;
		}
	}

	if (expected_ap + expected_mesh <= 0)
		mesh_log_info(ctx, "wifi: no enabled AP/mesh interfaces to wait for");
	else if (ok)
		mesh_log_info(ctx, "wifi: all %d AP/mesh interfaces ENABLED",
			      expected_ap + expected_mesh);
	else
		mesh_log_info(ctx,
			      "wifi: timed out waiting for AP/mesh ENABLED (%d/%d AP, %d/%d mesh ready)",
			      ready_ap, expected_ap, ready_mesh, expected_mesh);
	sleep(WIFI_ENABLED_SETTLE_S);
	return ok ? 0 : -1;
}

int mesh_radiophy(const char *radio_or_section, char *phy_out, size_t phy_len)
{
	char section[128];
	char phy[64], path[256];
	char base_path[256], fullpath[512], phy_dir[576];
	char listing[1024];
	char *slash;
	int phy_count = 0;
	char only[32] = "";

	phy_out[0] = '\0';
	if (!radio_or_section || !radio_or_section[0])
		return -1;
	if (!strncmp(radio_or_section, "wireless.", 9))
		snprintf(section, sizeof(section), "%s", radio_or_section);
	else
		snprintf(section, sizeof(section), "wireless.%s", radio_or_section);

	{
		char check[8];
		if (mesh_uci_get(section, check, sizeof(check)) != 0)
			return -1;
	}
	snprintf(phy, sizeof(phy), "%s.phy", section);
	if (mesh_uci_get(phy, phy_out, phy_len) == 0 && phy_out[0])
		return 0;
	{
		char path_opt[160];
		snprintf(path_opt, sizeof(path_opt), "%s.path", section);
		if (mesh_uci_get(path_opt, path, sizeof(path)) != 0 || !path[0])
			return -1;
	}

	/* strip +N */
	snprintf(base_path, sizeof(base_path), "%s", path);
	slash = strchr(base_path, '+');
	if (slash)
		*slash = '\0';

	if (!strncmp(base_path, "platform/", 9))
		snprintf(fullpath, sizeof(fullpath), "/sys/devices/%s", base_path);
	else
		snprintf(fullpath, sizeof(fullpath), "/sys/devices/platform/%s", base_path);

	snprintf(phy_dir, sizeof(phy_dir), "%s/ieee80211", fullpath);
	{
		char *lsargv[] = { "ls", phy_dir, NULL };
		if (mesh_run_capture(lsargv, listing, sizeof(listing)) != 0)
			return -1;
	}
	{
		char *p = listing, *tok;
		while ((tok = strsep(&p, " \n\t")) != NULL) {
			if (!strncmp(tok, "phy", 3)) {
				phy_count++;
				snprintf(only, sizeof(only), "%s", tok);
			}
		}
	}
	if (phy_count == 1) {
		snprintf(phy_out, phy_len, "%s", only);
		return 0;
	}
	if (phy_count > 1) {
		/* take first phy for now (v7 has more complex multi-phy pick) */
		snprintf(phy_out, phy_len, "%s", only);
		return 0;
	}
	return -1;
}

int mesh_get_wiphys(struct mesh_ctx *ctx)
{
	char out[4096];
	char *argv[] = { "iw", "list", NULL };
	char show[8192];
	char *p, *line;
	int i;

	ctx->wiphy_count = 0;
	ctx->radio_count = 0;
	mesh_mkdir_p(MESH11SD_TMPDIR "/wiphys");
	mesh_mkdir_p(MESH11SD_TMPDIR "/radios");

	if (mesh_run_capture(argv, out, sizeof(out)) == 0) {
		p = out;
		while ((line = strsep(&p, "\n")) != NULL) {
			char *w;
			if (!(w = strstr(line, "Wiphy ")))
				continue;
			w += 6;
			while (*w == ' ')
				w++;
			if (ctx->wiphy_count < MESH11SD_MAX_RADIOS) {
				sscanf(w, "%15s", ctx->wiphys[ctx->wiphy_count]);
				ctx->wiphy_count++;
			}
		}
		/* reverse order as v7 */
		for (i = 0; i < ctx->wiphy_count / 2; i++) {
			char tmp[16];
			snprintf(tmp, sizeof(tmp), "%s", ctx->wiphys[i]);
			snprintf(ctx->wiphys[i], sizeof(ctx->wiphys[0]), "%s",
				 ctx->wiphys[ctx->wiphy_count - 1 - i]);
			snprintf(ctx->wiphys[ctx->wiphy_count - 1 - i],
				 sizeof(ctx->wiphys[0]), "%s", tmp);
		}
	}

	if (mesh_uci_show_filter("wireless", "wifi-device", show, sizeof(show)) == 0) {
		p = show;
		while ((line = strsep(&p, " \n")) != NULL) {
			char *dot = strchr(line, '.');
			char *eq = strchr(line, '=');
			if (!dot || !eq)
				continue;
			*eq = '\0';
			dot++;
			if (ctx->radio_count < MESH11SD_MAX_RADIOS) {
				snprintf(ctx->radios[ctx->radio_count],
					 sizeof(ctx->radios[0]), "%s", dot);
				ctx->radio_count++;
			}
		}
	}

	for (i = 0; i < ctx->wiphy_count; i++) {
		char info[131072];
		char *iargv[] = { "iw", "phy", ctx->wiphys[i], "info", NULL };
		char path[192];
		int mesh_capable = 1;
		int total_ifaces = 1;

		if (mesh_phy_info_has(ctx->wiphys[i], "mesh point"))
			mesh_capable = 0; /* v7: 0 means capable (grep -w -q exit) */
		else
			mesh_capable = 1;
		if (mesh_run_capture(iargv, info, sizeof(info)) == 0) {
			char *ti = strstr(info, "total <=");
			if (ti)
				sscanf(ti, "total <= %d", &total_ifaces);
		}
		snprintf(path, sizeof(path), "%s/wiphys/%s", MESH11SD_TMPDIR, ctx->wiphys[i]);
		{
			char content[256];
			snprintf(content, sizeof(content),
				 "wiphy=%s; mesh_capable=%d; total_ifaces=%d\n",
				 ctx->wiphys[i], mesh_capable, total_ifaces);
			mesh_file_write(path, content);
		}
		if (mesh_capable)
			mesh_log_info(ctx, "WARNING - radios on wiphy [ %s ] do not support mesh interfaces.",
				      ctx->wiphys[i]);
		mesh_log_debug(ctx, "Max number of VIFs on [ %s ] is [ %d ]",
			       ctx->wiphys[i], total_ifaces);
	}
	return 0;
}

int mesh_get_valid_channels(char *out, size_t outlen)
{
	char raw[8192];
	char *argv[] = { "iw", "list", NULL };
	char *p, *line;
	size_t used = 0;

	out[0] = '\0';
	if (mesh_run_capture(argv, raw, sizeof(raw)) != 0)
		return -1;
	p = raw;
	while ((line = strsep(&p, "\n")) != NULL) {
		char *br;
		int ch;
		if (!strstr(line, "MHz [") || strstr(line, "disabled") ||
		    strstr(line, "radar detection"))
			continue;
		br = strchr(line, '[');
		if (!br)
			continue;
		if (sscanf(br, "[%d]", &ch) == 1) {
			int n = snprintf(out + used, outlen - used, "%s%d",
					 used ? " " : "", ch);
			if (n < 0 || (size_t)n >= outlen - used)
				break;
			used += (size_t)n;
		}
	}
	return 0;
}

int mesh_is_channel_valid(const char *band, const char *channel)
{
	int ch, start, end;
	char valid[1024];
	char needle[16];

	if (!band || !channel)
		return 1;
	ch = atoi(channel);
	if (!strcmp(band, "2g")) {
		start = 1;
		end = 13;
	} else if (!strcmp(band, "5g")) {
		start = 36;
		end = 177;
	} else if (!strcmp(band, "6g")) {
		start = 1;
		end = 233;
	} else if (!strcmp(band, "60g")) {
		start = 1;
		end = 8;
	} else {
		return 1;
	}
	if (ch < start || ch > end)
		return 1;
	if (mesh_get_valid_channels(valid, sizeof(valid)) != 0)
		return 1;
	snprintf(needle, sizeof(needle), "%d", ch);
	{
		char *p = valid, *tok;
		while ((tok = strsep(&p, " ")) != NULL) {
			if (!strcmp(tok, needle))
				return 0;
		}
	}
	return 1;
}

int mesh_refresh_bridgemac(struct mesh_ctx *ctx)
{
	char device[64];
	char status[16];
	char out[1024];
	char content[256];
	char *argv[] = { "ip", "link", "show", "dev", device, NULL };

	if (ctx->portal_detect == 3) {
		/* CPE: the mesh bridge is the wan device, br-wan (v7) */
		char key[96];
		snprintf(key, sizeof(key), "network.%s.device", ctx->auto_mesh_network);
		if (mesh_uci_get(key, device, sizeof(device)) != 0 || !device[0])
			snprintf(device, sizeof(device), "br-wan");
	} else if (mesh_uci_get("network.lan.device", device, sizeof(device)) != 0 || !device[0]) {
		char key[96];
		snprintf(key, sizeof(key), "network.%s.device", ctx->auto_mesh_network);
		if (mesh_uci_get(key, device, sizeof(device)) != 0 || !device[0])
			snprintf(device, sizeof(device), "br-lan");
	}
	snprintf(ctx->device, sizeof(ctx->device), "%s", device);
	mesh_wait_for_interface(ctx, device, status, sizeof(status));
	ctx->devicemac[0] = '\0';
	if (mesh_run_capture(argv, out, sizeof(out)) == 0) {
		char *p = out, *line;
		int take = 0;
		while ((line = strsep(&p, "\n")) != NULL) {
			/* v7: grep -A 1 "$device:" — match the ifname field
			 * ("2: br-lan:"), not a later "master br-lan" token
			 * on eth1/lan ports. */
			const char *q = line;
			int is_dev = 0;

			if (*q >= '0' && *q <= '9') {
				while (*q >= '0' && *q <= '9')
					q++;
				if (*q == ':') {
					q++;
					while (*q == ' ')
						q++;
					{
						size_t n = strlen(device);
						if (strncmp(q, device, n) == 0 &&
						    q[n] == ':')
							is_dev = 1;
					}
				}
			}
			if (is_dev)
				take = 1;
			else if (take && strstr(line, "link/ether")) {
				char *m = strstr(line, "link/ether ");
				if (m) {
					sscanf(m + 11, "%31s", ctx->devicemac);
					break;
				}
			} else if (take && line[0] && line[0] != ' ') {
				take = 0;
			}
		}
	}
	snprintf(content, sizeof(content),
		 "device=\"%s\" ; devicemac=\"%s\"\n", ctx->device, ctx->devicemac);
	mesh_file_write(MESH11SD_TMPDIR "/devicemac", content);
	return ctx->devicemac[0] ? 0 : -1;
}

int mesh_sys_setup(struct mesh_ctx *ctx)
{
	const char *dev = ctx->device[0] ? ctx->device : "br-lan";
	char path[192];
	FILE *f;

	mesh_refresh_bridgemac(ctx);
	dev = ctx->device[0] ? ctx->device : "br-lan";

#define WRITE_SYS(fmt, val) do { \
	snprintf(path, sizeof(path), fmt, val); \
	f = fopen(path, "w"); \
	if (f) { fputs("1\n", f); fclose(f); } \
} while (0)

	/* arp/ndp defaults matching v7 sys_setup (simplified critical set) */
	{
		const char *keys[] = {
			"/proc/sys/net/ipv4/conf/all/arp_accept",
			"/proc/sys/net/ipv4/conf/default/arp_accept",
			NULL
		};
		int i;
		for (i = 0; keys[i]; i++) {
			f = fopen(keys[i], "w");
			if (f) { fputs("1\n", f); fclose(f); }
		}
		snprintf(path, sizeof(path), "/proc/sys/net/ipv4/conf/%s/arp_accept", dev);
		f = fopen(path, "w");
		if (f) { fputs("1\n", f); fclose(f); }

		f = fopen("/proc/sys/net/ipv6/conf/all/forwarding", "w");
		if (f) { fputs("1\n", f); fclose(f); }
		f = fopen("/proc/sys/net/ipv6/conf/default/forwarding", "w");
		if (f) { fputs("1\n", f); fclose(f); }
		snprintf(path, sizeof(path), "/proc/sys/net/ipv6/conf/%s/forwarding", dev);
		f = fopen(path, "w");
		if (f) { fputs("1\n", f); fclose(f); }
		snprintf(path, sizeof(path), "/proc/sys/net/ipv6/conf/%s/accept_ra", dev);
		f = fopen(path, "w");
		if (f) { fputs("0\n", f); fclose(f); }
		f = fopen("/proc/sys/net/ipv6/conf/all/accept_ra", "w");
		if (f) { fputs("2\n", f); fclose(f); }
		f = fopen("/proc/sys/net/ipv6/conf/default/accept_ra", "w");
		if (f) { fputs("2\n", f); fclose(f); }
	}
#undef WRITE_SYS
	return 0;
}

int mesh_set_txpower_uci(struct mesh_ctx *ctx)
{
	char show[4096];
	char *p, *line;

	if (!ctx->txpower[0])
		return 0;
	if (mesh_uci_show_filter("wireless", "wifi-device", show, sizeof(show)) != 0)
		return -1;
	p = show;
	while ((line = strsep(&p, " \n")) != NULL) {
		char *eq = strchr(line, '=');
		char radio[64];
		char cur[32];
		char setline[128];
		if (!eq)
			continue;
		*eq = '\0';
		snprintf(radio, sizeof(radio), "%s", line);
		snprintf(cur, sizeof(cur), "%s.txpower", radio);
		if (mesh_uci_get(cur, cur, sizeof(cur)) != 0) {
			snprintf(setline, sizeof(setline), "set %s.txpower='%s'",
				 radio, ctx->txpower);
			mesh_uci_batch_line(setline);
		}
	}
	return 0;
}

int mesh_cli_txpower(struct mesh_ctx *ctx, const char *arg)
{
	char status[16];
	int i, current = 0, new_power;
	char out[256];

	mesh_get_mesh_iflist(ctx);
	printf("===========================================================================\n");
	printf("Change the mesh transmit power\n");
	if (ctx->iflist_count == 0) {
		printf("    No mesh interfaces found\n");
		printf("===========================================================================\n\n");
		return 0;
	}
	for (i = 0; i < ctx->iflist_count; i++) {
		mesh_wait_for_interface(ctx, ctx->iflist[i], status, sizeof(status));
		if (!strcmp(status, "down"))
			continue;
		{
			char *argv[] = { "iw", "dev", ctx->iflist[i], "info", NULL };
			if (mesh_run_capture(argv, out, sizeof(out)) == 0) {
				char *tp = strstr(out, "txpower");
				if (tp)
					sscanf(tp, "txpower %d", &current);
			}
		}
		break;
	}
	if (!arg || !arg[0]) {
		printf("    Current TX power [ %d ] dBm \n", current);
		printf("    Usage: mesh11sd txpower [+|-] \n");
		printf("    where \"+\" increments by 3dBm and \"-\" decrements by 3dBm\n");
	} else if (!strcmp(arg, "+") || !strcmp(arg, "-")) {
		printf("    Previous TX power [ %d ] dBm \n", current);
		new_power = !strcmp(arg, "+") ? current + 3 : current - 3;
		{
			char pwr[16];
			char *argv[] = { "iw", "dev", ctx->iflist[i], "set", "txpower",
					 "fixed", pwr, NULL };
			snprintf(pwr, sizeof(pwr), "%d", new_power * 100);
			mesh_run_capture(argv, NULL, 0);
		}
		{
			char *argv[] = { "iw", "dev", ctx->iflist[i], "info", NULL };
			if (mesh_run_capture(argv, out, sizeof(out)) == 0) {
				char *tp = strstr(out, "txpower");
				if (tp)
					sscanf(tp, "txpower %d", &current);
			}
		}
		printf("\n    New TX power - [ %d ] dBm \n", current);
		snprintf(ctx->txpower, sizeof(ctx->txpower), "%d", current);
		mesh_uci_set("mesh11sd.setup.txpower", ctx->txpower);
	}
	printf("===========================================================================\n\n");
	return 0;
}

int mesh_cli_stations(struct mesh_ctx *ctx)
{
	char status[16];
	int i;

	mesh_get_mesh_iflist(ctx);
	printf("===========================================================================\n");
	for (i = 0; i < ctx->iflist_count; i++) {
		mesh_wait_for_interface(ctx, ctx->iflist[i], status, sizeof(status));
		if (!strcmp(status, "down"))
			continue;
		{
			char *argv[] = { "iw", "dev", ctx->iflist[i], "station", "dump", NULL };
			char out[16384];
			if (mesh_run_capture(argv, out, sizeof(out)) == 0)
				fputs(out, stdout);
			if (out[0] && out[strlen(out) - 1] != '\n')
				fputc('\n', stdout);
		}
	}
	printf("===========================================================================\n\n");
	return 0;
}

int mesh_cli_country(struct mesh_ctx *ctx, const char *code)
{
	char out[1024];
	char *argv[] = { "iw", "reg", "get", NULL };

	if (mesh_run_capture(argv, out, sizeof(out)) == 0) {
		char *p = strstr(out, "country");
		if (p) {
			char line[128];
			sscanf(p, "%127[^\n]", line);
			printf("%s\n", line);
		} else {
			fputs(out, stdout);
		}
	}
	if (!code || !code[0])
		return 0;
	if (ctx->commit_all) {
		printf("Unable to change regulatory domain. Auto config is committed - edit wireless config to change country settings.\n");
		return 1;
	}
	{
		char line[128];
		snprintf(line, sizeof(line), "set mesh11sd.setup.country='%s'", code);
		/* commit_single_change semantics */
		{
			char changes[4096];
			mesh_uci_changes("mesh11sd", changes, sizeof(changes));
			mesh_uci_revert("mesh11sd");
			mesh_uci_batch_line(line);
			mesh_uci_commit("mesh11sd");
			/* restore other uncommitted - simplified: re-apply changes lines */
			{
				char *p = changes, *ln;
				while ((ln = strsep(&p, "\n")) != NULL) {
					char setl[512];
					if (!ln[0] || strstr(ln, "country"))
						continue;
					snprintf(setl, sizeof(setl), "set %s", ln);
					mesh_uci_batch_line(setl);
				}
			}
		}
		snprintf(ctx->country, sizeof(ctx->country), "%s", code);
		printf("Country code set to [ %s ]. Restart or reboot for wireless to apply.\n", code);
	}
	return 0;
}

int mesh_cli_mesh_rssi_threshold(struct mesh_ctx *ctx, const char *dir, int force)
{
	/* v7 mesh_rssi_threshold [+|-] [force]:
	 * Without force: lock file + iw set so only NEW plinks use the value.
	 * With force: write UCI, drop the lock, print "Forcing new threshold
	 * on all peers...". The daemon's check_mesh_params then applies the
	 * value and all_nodes_rssi_update restarts wifi ("Disconnecting nodes").
	 */
	char status[16];
	int i, current = 0, newv, new_current;
	char out[128];
	char *iface = NULL;

	mesh_get_mesh_iflist(ctx);
	printf("===========================================================================\n");
	printf("Change the mesh rssi threshold\n");
	for (i = 0; i < ctx->iflist_count; i++) {
		mesh_wait_for_interface(ctx, ctx->iflist[i], status, sizeof(status));
		if (!strcmp(status, "down"))
			continue;
		{
			char *argv[] = { "iw", "dev", ctx->iflist[i], "get", "mesh_param",
					 "mesh_rssi_threshold", NULL };
			if (mesh_run_capture(argv, out, sizeof(out)) == 0)
				current = atoi(out);
		}
		iface = ctx->iflist[i];
		break;
	}
	if (!dir || !dir[0]) {
		printf("    Current threshold for new connections [ %d ] dBm \n", current);
		printf("    Usage: mesh11sd mesh_rssi_threshold [+|-] [force]\n");
		printf("    where \"+\" increments by 3dBm and \"-\" decrements by 3dBm\n");
		printf("    The keyword \"force\" forces the new threshold on all peers\n");
	} else if (!strcmp(dir, "+") || !strcmp(dir, "-")) {
		char val[16];

		newv = !strcmp(dir, "+") ? current + 3 : current - 3;
		snprintf(val, sizeof(val), "%d", newv);
		printf("    Previous threshold [ %d ] dBm \n", current);
		if (!force) {
			mesh_file_write(MESH11SD_TMPDIR "/mesh_rssi_threshold", val);
			if (iface) {
				char *argv[] = { "iw", "dev", iface, "set", "mesh_param",
						 "mesh_rssi_threshold", val, NULL };
				mesh_run_capture(argv, NULL, 0);
			}
			mesh_uci_set("mesh11sd.mesh_params.mesh_rssi_threshold", val);
			new_current = newv;
			if (iface) {
				char *argv[] = { "iw", "dev", iface, "get", "mesh_param",
						 "mesh_rssi_threshold", NULL };
				if (mesh_run_capture(argv, out, sizeof(out)) == 0)
					new_current = atoi(out);
			}
		} else {
			mesh_uci_set("mesh11sd.mesh_params.mesh_rssi_threshold", val);
			printf("\n    Forcing new threshold on all peers...\n");
			unlink(MESH11SD_TMPDIR "/mesh_rssi_threshold");
			new_current = newv;
		}
		printf("\n    New threshold - [ %d ] dBm \n", new_current);
	}
	printf("===========================================================================\n\n");
	return 0;
}


/* v7 check_mesh_phantom: mpath next-hop 00:00:00:00:00:00 poisons HWMP — delete */
int mesh_check_mesh_phantom(struct mesh_ctx *ctx)
{
	int i;

	mesh_get_mesh_iflist(ctx);
	for (i = 0; i < ctx->iflist_count; i++) {
		char out[8192];
		char *argv[] = {
			"iw", "dev", ctx->iflist[i], "mpath", "dump", NULL
		};
		char *p, *line;
		char prev[96] = "";

		if (mesh_run_capture(argv, out, sizeof(out)) != 0)
			continue;
		p = out;
		while ((line = strsep(&p, "\n")) != NULL) {
			char dest[32], nexthop[32];
			char key[96];

			if (!line[0])
				continue;
			/* skip header */
			if (!strncmp(line, "DEST", 4) || strstr(line, "NEXT HOP"))
				continue;
			if (sscanf(line, "%31s %31s", dest, nexthop) != 2)
				continue;
			if (strlen(dest) != 17 || strlen(nexthop) != 17)
				continue;
			snprintf(key, sizeof(key), "%s,%s", dest, nexthop);
			if (prev[0] && !strcmp(prev, key))
				continue;
			snprintf(prev, sizeof(prev), "%s", key);
			if (strcmp(nexthop, "00:00:00:00:00:00") != 0)
				continue;
			mesh_log_info(ctx, "Phantom meshnode detected [%s] - deleting",
				      dest);
			{
				char *del[] = {
					"iw", "dev", ctx->iflist[i], "mpath", "del",
					dest, NULL
				};
				mesh_run_capture(del, NULL, 0);
			}
		}
	}
	return 0;
}
