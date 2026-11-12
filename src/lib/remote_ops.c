/* SPDX-License-Identifier: GPL-2.0-or-later
 * Chunk 8: connect/copy remote CLI + misc ops (leechmode, mobility level,
 * force_ipv4_download, dhcp4_renew, is_installed).
 */
#include "mesh11sd.h"

#include <ctype.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NDP_RESPONSE_MAX 32768

static char ndp_response[NDP_RESPONSE_MAX];

static void normalize_nodeaddr(const char *in, char *out, size_t outlen)
{
	size_t i, o = 0;
	for (i = 0; in && in[i] && o + 1 < outlen; i++) {
		char c = in[i];
		if (c == '-')
			c = ':';
		out[o++] = (char)tolower((unsigned char)c);
	}
	out[o] = '\0';
}

static void mac_to_dashed(const char *mac, char *out, size_t outlen)
{
	size_t i, o = 0;

	for (i = 0; mac && mac[i] && o + 1 < outlen; i++)
		out[o++] = (mac[i] == ':') ? '-' : mac[i];
	out[o] = '\0';
}

static void mac_strip_colons(const char *mac, char *out, size_t outlen)
{
	size_t i, o = 0;

	for (i = 0; mac && mac[i] && o + 1 < outlen; i++) {
		if (mac[i] != ':')
			out[o++] = (char)tolower((unsigned char)mac[i]);
	}
	out[o] = '\0';
}

static void load_tmpdir_quoted(const char *path, const char *key, char *out, size_t outlen)
{
	FILE *f;
	char line[256];
	char prefix[80];
	size_t plen;

	if (!out || !outlen)
		return;
	snprintf(prefix, sizeof(prefix), "%s=\"", key);
	plen = strlen(prefix);
	f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *p = strstr(line, prefix);
		char val[128];

		if (!p)
			continue;
		if (sscanf(p + plen, "%127[^\"]", val) == 1)
			snprintf(out, outlen, "%s", val);
	}
	fclose(f);
}

/* v7 connect/copy source tmpdir/is_portal and tmpdir/active_mesh_ifname. */
static void load_connect_runtime(struct mesh_ctx *ctx, int clear_if_missing)
{
	if (access(MESH11SD_TMPDIR "/is_portal", R_OK) == 0)
		load_tmpdir_quoted(MESH11SD_TMPDIR "/is_portal",
				   "detected_state", ctx->detected_state,
				   sizeof(ctx->detected_state));
	else if (clear_if_missing)
		ctx->detected_state[0] = '\0';

	if (!ctx->active_mesh_ifname[0])
		load_tmpdir_quoted(MESH11SD_TMPDIR "/active_mesh_ifname",
				   "active_mesh_ifname", ctx->active_mesh_ifname,
				   sizeof(ctx->active_mesh_ifname));
}

static void ndp_response_append(const char *chunk)
{
	size_t have, need;

	if (!chunk || !chunk[0])
		return;
	have = strlen(ndp_response);
	need = strlen(chunk) + 2; /* space + NUL */
	if (have + need > sizeof(ndp_response))
		need = sizeof(ndp_response) - have;
	if (need < 2)
		return;
	snprintf(ndp_response + have, need, " %s", chunk);
}

static void wait_for_nodes(struct mesh_ctx *ctx)
{
	int i;
	char br[64], pingaddr[96];
	char round[4096];

	fputs(" Building node list * ", stdout);
	fflush(stdout);
	mesh_ccpy_setup(ctx);
	ndp_response[0] = '\0';
	snprintf(br, sizeof(br), "br-%s", ctx->auto_mesh_network);
	snprintf(pingaddr, sizeof(pingaddr), "ff02::2%%%s", br);
	for (i = 0; i < 5; i++) {
		char *pargv[] = { "ping", "-c", "2", pingaddr, NULL };

		round[0] = '\0';
		mesh_run_capture(pargv, round, sizeof(round));
		ndp_response_append(round);
		fputs("* ", stdout);
		fflush(stdout);
	}
	fputc('\n', stdout);
}

static int load_connectlist(char entries[][96], int max)
{
	FILE *f = fopen(MESH11SD_TMPDIR "/connectlist", "r");
	int n = 0;
	char line[160];
	if (!f)
		return 0;
	while (n < max && fgets(line, sizeof(line), f)) {
		size_t L = strlen(line);
		while (L && (line[L - 1] == '\n' || line[L - 1] == '\r'))
			line[--L] = '\0';
		if (!line[0])
			continue;
		snprintf(entries[n], 96, "%s", line);
		n++;
	}
	fclose(f);
	return n;
}

/* v7 check_if_peer: 0 if octets 2,3,5 of ndpmac match a peerlist MAC. */
static int check_if_peer(struct mesh_ctx *ctx, const char *ndpmac)
{
	unsigned n[6], p[6];
	int i;

	if (sscanf(ndpmac, "%x:%x:%x:%x:%x:%x",
		   &n[0], &n[1], &n[2], &n[3], &n[4], &n[5]) != 6)
		return 1;
	for (i = 0; i < ctx->peerlist_count; i++) {
		if (sscanf(ctx->peerlist[i], "%x:%x:%x:%x:%x:%x",
			   &p[0], &p[1], &p[2], &p[3], &p[4], &p[5]) != 6)
			continue;
		if (n[1] == p[1] && n[2] == p[2] && n[4] == p[4])
			return 0;
	}
	return 1;
}

/* v7: column 2 of the mpp dump line containing ndpmac. */
static int lookup_label_mac(struct mesh_ctx *ctx, const char *ndpmac,
			    char *out, size_t outlen)
{
	char dump[16384];
	char *argv[] = { "iw", "dev", ctx->active_mesh_ifname, "mpp", "dump", NULL };
	char *p, *line;

	out[0] = '\0';
	if (!ctx->active_mesh_ifname[0])
		return -1;
	if (mesh_run_capture(argv, dump, sizeof(dump)) != 0)
		return -1;
	p = dump;
	while ((line = strsep(&p, "\n")) != NULL) {
		char c1[64], c2[64];

		if (!strstr(line, ndpmac))
			continue;
		if (sscanf(line, "%63s %63s", c1, c2) >= 2) {
			snprintf(out, outlen, "%s", c2);
			return 0;
		}
	}
	return -1;
}

static void maybe_rewrite_label_mac(const char *ndpmac, char *label_mac, size_t labellen)
{
	char ndp[16], lab[16];

	mac_strip_colons(ndpmac, ndp, sizeof(ndp));
	mac_strip_colons(label_mac, lab, sizeof(lab));
	if (strlen(ndp) < 12 || strlen(lab) < 12)
		return;
	if (strncmp(ndp + 8, lab + 8, 4) != 0)
		snprintf(label_mac, labellen, "xx:xx:xx:xx:%c%c:%c%c",
			 ndp[8], ndp[9], ndp[10], ndp[11]);
}

static void build_node_ula(struct mesh_ctx *ctx, const char *ndpmac,
			   char *out, size_t outlen)
{
	char hex[16];
	unsigned first;
	char eui[17];

	out[0] = '\0';
	mac_strip_colons(ndpmac, hex, sizeof(hex));
	if (strlen(hex) < 12)
		return;
	if (sscanf(hex, "%2x", &first) != 1)
		return;
	first ^= 0x02;
	/* first_byte + octets 2-3 + fffe + octets 4-6 */
	snprintf(eui, sizeof(eui), "%02x%c%c%c%cfffe%c%c%c%c%c%c",
		 first,
		 hex[2], hex[3], hex[4], hex[5],
		 hex[6], hex[7], hex[8], hex[9], hex[10], hex[11]);
	snprintf(out, outlen, "%s:%s:%c%c%c%c:%c%c%c%c:%c%c%c%c:%c%c%c%c",
		 ctx->ula_base, ctx->ula_subnet,
		 eui[0], eui[1], eui[2], eui[3],
		 eui[4], eui[5], eui[6], eui[7],
		 eui[8], eui[9], eui[10], eui[11],
		 eui[12], eui[13], eui[14], eui[15]);
}

/* ssh/scp must inherit the terminal; mesh_run_capture swallows stdout. */
static int run_interactive(char *const argv[])
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		execvp(argv[0], argv);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) < 0)
		return -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;
	return 0;
}

static int linklocal_in_response(const char *linklocal)
{
	char ip6[96];
	char *pct = strchr(linklocal, '%');
	size_t nlen;

	snprintf(ip6, sizeof(ip6), "%s", linklocal);
	if (pct) {
		nlen = (size_t)(pct - linklocal);
		if (nlen >= sizeof(ip6))
			nlen = sizeof(ip6) - 1;
		ip6[nlen] = '\0';
	}
	return ndp_response[0] && strstr(ndp_response, ip6) != NULL;
}

static int ping6_linklocal(struct mesh_ctx *ctx, const char *ip6)
{
	char br[64];
	char *pargv[] = { "ping6", "-q", "-c", "1", "-W", "1", "-I", br,
			  (char *)ip6, NULL };

	snprintf(br, sizeof(br), "br-%s", ctx->auto_mesh_network);
	return mesh_run_capture(pargv, NULL, 0) == 0;
}

static void print_node_line(struct mesh_ctx *ctx, const char *ndpmacf,
			    const char *label_mac, const char *ndpmac,
			    const char *ip6)
{
	char node_ula[128];

	build_node_ula(ctx, ndpmac, node_ula, sizeof(node_ula));
	if (!ping6_linklocal(ctx, ip6))
		return;
	if (node_ula[0])
		printf("Node ID: %s [ label mac: %s ] [ ULA: %s ] [ web_ui: https://[%s] ]\n",
		       ndpmacf, label_mac, node_ula, node_ula);
	else
		printf("%s [ ipaddress: %s ]\n", ndpmacf, ip6);
}

enum { REMOTE_CONNECT = 0, REMOTE_COPY = 1 };

static void walk_connectlist(struct mesh_ctx *ctx, int mode,
			     const char *nodeaddr, const char *filepath)
{
	char entries[64][96];
	int n, i;
	int show_all = ctx->connect_show_all;

	if (!ctx->peerlist_count)
		return;
	mesh_get_portal_ula(ctx, "get");
	n = load_connectlist(entries, 64);
	for (i = 0; i < n; i++) {
		char linklocal[128] = "", ndpmac[32] = "";
		char *comma = strchr(entries[i], ',');
		char ndpmacf[40], label_mac[64], ip6[96];
		char *pct;

		if (!comma)
			continue;
		*comma = '\0';
		snprintf(linklocal, sizeof(linklocal), "%s", entries[i]);
		snprintf(ndpmac, sizeof(ndpmac), "%s", comma + 1);

		if (show_all != 1 && check_if_peer(ctx, ndpmac) != 0)
			continue;

		if (lookup_label_mac(ctx, ndpmac, label_mac, sizeof(label_mac)) != 0 ||
		    !label_mac[0])
			continue;

		if (!linklocal_in_response(linklocal))
			continue;

		maybe_rewrite_label_mac(ndpmac, label_mac, sizeof(label_mac));
		mac_to_dashed(ndpmac, ndpmacf, sizeof(ndpmacf));

		snprintf(ip6, sizeof(ip6), "%s", linklocal);
		pct = strchr(ip6, '%');
		if (pct)
			*pct = '\0';

		if (!nodeaddr[0]) {
			print_node_line(ctx, ndpmacf, label_mac, ndpmac, ip6);
			continue;
		}
		if (strcmp(nodeaddr, ndpmac) != 0)
			continue;

		if (mode == REMOTE_CONNECT) {
			char target[160];
			char *ssh[] = { "ssh", target, NULL };

			snprintf(target, sizeof(target), "root@%s", linklocal);
			printf("\nTrying to connect to meshnode \"%s\".....\n", ndpmacf);
			run_interactive(ssh);
			printf("\nDisconnected from meshnode \"%s\"\n\n", ndpmacf);
			break;
		}
		if (mode == REMOTE_COPY && filepath && filepath[0]) {
			char dest[256];
			const char *base = strrchr(filepath, '/');
			char *scp[] = { "scp", (char *)filepath, dest, NULL };

			base = base ? base + 1 : filepath;
			snprintf(dest, sizeof(dest), "root@[%s]:%s/%s",
				 linklocal, MESH11SD_TMPDIR, base);
			printf("\nTrying to copy [ %s/%s ] to meshnode [ %s ].....\n",
			       MESH11SD_TMPDIR, base, ndpmacf);
			run_interactive(scp);
			printf("\nDisconnected from meshnode \"%s\"\n\n", ndpmacf);
			break;
		}
	}
}

int mesh_cli_connect(struct mesh_ctx *ctx, int argc, char **argv)
{
	char nodeaddr[64] = "";

	load_connect_runtime(ctx, 1);

	if (ctx->auto_config == 0 || !ctx->detected_state[0]) {
		puts("======================================================================");
		puts("Unable to connect -  enable auto_config to connect to other meshnodes");
		puts("======================================================================");
		return 1;
	}
	if (argc >= 3 && !strcmp(argv[2], "filter_list"))
		ctx->connect_show_all = 0;
	else if (argc >= 3)
		normalize_nodeaddr(argv[2], nodeaddr, sizeof(nodeaddr));

	puts("=============================================================================================================================================================================");
	puts(" Connect a remote terminal session on a remote meshnode");
	puts("    Usage: mesh11sd connect [Node ID]");
	puts("           mesh11sd connect filter_list   (show only nodes from this contiguous mesh)");
	puts("");
	wait_for_nodes(ctx);
	puts("");

	if (!nodeaddr[0]) {
		puts(" If the node you are looking for is not in the list - re-run this command.");
		puts("=============================================================================================================================================================================");
		puts(" The following meshnodes are available for remote connection:");
	}

	walk_connectlist(ctx, REMOTE_CONNECT, nodeaddr, NULL);

	puts("=============================================================================================================================================================================");
	puts("");
	return 0;
}

int mesh_cli_copy(struct mesh_ctx *ctx, int argc, char **argv)
{
	char nodeaddr[64] = "";
	const char *filepath = NULL;

	load_connect_runtime(ctx, 0);

	if (ctx->auto_config == 0) {
		puts("================================================================");
		puts("Unable to copy -  enable auto_config to copy to other meshnodes");
		puts("================================================================");
		return 1;
	}
	if (argc >= 3 && !strcmp(argv[2], "filter_list"))
		ctx->connect_show_all = 0;
	else if (argc >= 3)
		normalize_nodeaddr(argv[2], nodeaddr, sizeof(nodeaddr));
	if (argc >= 4)
		filepath = argv[3];

	puts("=============================================================================================================================================================================");
	puts(" Copy a file to /tmp/mesh11sd/ on a remote meshnode");
	puts("    Usage: mesh11sd copy [Node ID] [path_to_file]");
	puts("           mesh11sd copy filter_list   (show only nodes from this contiguous mesh)");
	puts("");
	wait_for_nodes(ctx);
	puts("");

	if (!nodeaddr[0]) {
		puts(" If the node you are looking for is not in the list - re-run this command.");
		puts("=============================================================================================================================================================================");
		puts(" The following meshnodes are available for remote copy:");
	}

	walk_connectlist(ctx, REMOTE_COPY, nodeaddr, filepath);

	puts("=============================================================================================================================================================================");
	puts("");
	return 0;
}

int mesh_cli_mesh_leechmode(struct mesh_ctx *ctx, const char *arg)
{
	char setl[96];
	const char *msg;

	puts("===========================================================================");
	if (!arg || !arg[0]) {
		printf("Usage: mesh11sd mesh_leechmode [enable/disable]\n");
		printf("Option mesh_leechmode_enable current value [ %d ]\n",
		       ctx->mesh_gate_only);
		puts("===========================================================================");
		puts("");
		return 0;
	}
	if (!strcmp(arg, "enable")) {
		ctx->mesh_gate_only = 1;
		snprintf(setl, sizeof(setl),
			 "set mesh11sd.setup.mesh_leechmode_enable='1'");
		mesh_uci_batch_line(setl);
		msg = "Option mesh_leechmode_enable set to [ 1 ]";
	} else if (!strcmp(arg, "disable")) {
		ctx->mesh_gate_only = 0;
		snprintf(setl, sizeof(setl),
			 "set mesh11sd.setup.mesh_leechmode_enable='0'");
		mesh_uci_batch_line(setl);
		msg = "Option mesh_leechmode_enable set to [ 0 ]";
	} else {
		printf("Invalid option [ %s ]\n", arg);
		puts("===========================================================================");
		puts("");
		return 1;
	}
	puts(msg);
	puts("===========================================================================");
	puts("");
	return 0;
}

int mesh_cli_mesh_node_mobility_level(struct mesh_ctx *ctx, const char *arg)
{
	puts("===========================================================================");
	if (!arg || !arg[0]) {
		puts("Usage: mesh11sd mesh_node_mobility_level <level>");
		puts("    where level can be 0, 1, 2, 3, 4, 5 and 6");
		printf("Current level is [ %d ]\n", ctx->mesh_node_mobility_level);
		puts("===========================================================================");
		puts("");
		return 0;
	}
	{
		int level = atoi(arg);
		char setl[96];
		if (level < 0 || level > 6) {
			puts("Invalid level");
			return 1;
		}
		snprintf(setl, sizeof(setl),
			 "set mesh11sd.setup.mesh_node_mobility_level='%d'", level);
		mesh_uci_batch_line(setl);
		ctx->mesh_node_mobility_level = level;
		printf("Option mesh_node_mobility_level set to [ %d ]\n", level);
		mesh_mobility_settings(ctx, "all");
	}
	puts("===========================================================================");
	puts("");
	return 0;
}

int mesh_cli_force_ipv4_download(struct mesh_ctx *ctx)
{
	FILE *f;
	char line[64];
	(void)ctx;
	f = fopen("/usr/bin/wget", "r");
	if (!f)
		return 127;
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return 127;
	}
	fclose(f);
	if (!strncmp(line, "#!/bin/sh", 9))
		return 127;
	rename("/usr/bin/wget", "/usr/bin/wget.bak");
	f = fopen("/usr/bin/wget", "w");
	if (!f)
		return 127;
	fputs("#!/bin/sh\n/usr/bin/wget.bak -4 \"$@\"\n", f);
	fclose(f);
	chmod("/usr/bin/wget", 0755);
	return 0;
}

int mesh_cli_download_revert_to_default(struct mesh_ctx *ctx)
{
	FILE *f;
	char line[64];
	(void)ctx;
	f = fopen("/usr/bin/wget", "r");
	if (!f)
		return 127;
	if (!fgets(line, sizeof(line), f) || strncmp(line, "#!/bin/sh", 9) != 0) {
		fclose(f);
		return 127;
	}
	fclose(f);
	unlink("/usr/bin/wget");
	rename("/usr/bin/wget.bak", "/usr/bin/wget");
	return 0;
}

int mesh_cli_dhcp4_renew(struct mesh_ctx *ctx, const char *dev)
{
	char dhcpdevice[64];
	char host[64] = "OpenWrt";
	char hx[96];
	char pidpath[128];

	if (dev && dev[0])
		snprintf(dhcpdevice, sizeof(dhcpdevice), "%s", dev);
	else {
		char key[96];
		snprintf(key, sizeof(key), "network.%s.device", ctx->auto_mesh_network);
		if (mesh_uci_get(key, dhcpdevice, sizeof(dhcpdevice)) != 0)
			snprintf(dhcpdevice, sizeof(dhcpdevice), "br-%s",
				 ctx->auto_mesh_network);
	}
	mesh_uci_get("system.@system[0].hostname", host, sizeof(host));
	snprintf(hx, sizeof(hx), "hostname:%s", host);
	snprintf(pidpath, sizeof(pidpath), "/var/run/udhcpc-renew-%s.pid", dhcpdevice);
	{
		char *argv[] = {
			"udhcpc", "-p", pidpath, "-f", "-q", "-n", "-t0", "-A", "0",
			"-i", dhcpdevice, "-x", hx, NULL
		};
		/* v7 backgrounds; we run briefly */
		if (fork() == 0) {
			mesh_run_capture(argv, NULL, 0);
			_exit(0);
		}
	}
	sleep(6);
	if (access(pidpath, R_OK) == 0) {
		FILE *f = fopen(pidpath, "r");
		int pid = 0;
		mesh_log_debug(ctx, "Killing dhcp client-renew as it failed on [ %s ]",
			       dhcpdevice);
		if (f && fscanf(f, "%d", &pid) == 1 && pid > 0)
			kill(pid, SIGTERM);
		if (f)
			fclose(f);
	} else {
		char *argv[] = { "/sbin/service", "dnsmasq", "reload", NULL };
		mesh_run_capture(argv, NULL, 0);
	}
	return 0;
}

int mesh_cli_is_installed(struct mesh_ctx *ctx, const char *pkg)
{
	(void)ctx;
	if (!pkg || !pkg[0]) {
		puts("Usage: mesh11sd is_installed [packagename]");
		return 1;
	}
	if (mesh_is_installed(pkg)) {
		printf("%s is installed\n", pkg);
		return 0;
	}
	printf("%s is not installed\n", pkg);
	return 1;
}
