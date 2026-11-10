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

static void wait_for_nodes(struct mesh_ctx *ctx)
{
	int i;
	fputs(" Building node list * ", stdout);
	fflush(stdout);
	mesh_ccpy_setup(ctx);
	for (i = 0; i < 5; i++) {
		char br[64], pingaddr[96];
		char *pargv[] = { "ping", "-c", "2", pingaddr, NULL };
		snprintf(br, sizeof(br), "br-%s", ctx->auto_mesh_network);
		snprintf(pingaddr, sizeof(pingaddr), "ff02::2%%%s", br);
		mesh_run_capture(pargv, NULL, 0);
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

int mesh_cli_connect(struct mesh_ctx *ctx, int argc, char **argv)
{
	char nodeaddr[64] = "";
	int filter_list = 0;
	char entries[64][96];
	int n, i;

	puts("=============================================================================================================================================================================");
	puts(" Connect a remote terminal session on a remote meshnode");
	puts("    Usage: mesh11sd connect [Node ID]");
	puts("           mesh11sd connect filter_list   (show only nodes from this contiguous mesh)");
	puts("");

	if (ctx->auto_config == 0 || !ctx->detected_state[0]) {
		puts("======================================================================");
		puts("Unable to connect -  enable auto_config to connect to other meshnodes");
		puts("======================================================================");
		return 1;
	}
	if (argc >= 3 && !strcmp(argv[2], "filter_list"))
		filter_list = 1;
	else if (argc >= 3)
		normalize_nodeaddr(argv[2], nodeaddr, sizeof(nodeaddr));

	wait_for_nodes(ctx);
	mesh_get_portal_ula(ctx, "get");
	n = load_connectlist(entries, 64);

	if (!nodeaddr[0]) {
		puts(" If the node you are looking for is not in the list - re-run this command.");
		puts("=============================================================================================================================================================================");
		puts(" The following meshnodes are available for remote connection:");
	}

	for (i = 0; i < n; i++) {
		char linklocal[128] = "", ndpmac[32] = "";
		char *comma = strchr(entries[i], ',');
		char ndpmacf[40];
		int j;
		if (!comma)
			continue;
		*comma = '\0';
		snprintf(linklocal, sizeof(linklocal), "%s", entries[i]);
		snprintf(ndpmac, sizeof(ndpmac), "%s", comma + 1);
		for (j = 0; ndpmac[j]; j++)
			ndpmacf[j] = (ndpmac[j] == ':') ? '-' : ndpmac[j];
		ndpmacf[j] = '\0';

		if (nodeaddr[0]) {
			if (strcmp(nodeaddr, ndpmac) != 0)
				continue;
			printf("\nTrying to connect to meshnode \"%s\".....\n", ndpmacf);
			{
				char *ssh[] = { "ssh", NULL, NULL };
				char target[160];
				snprintf(target, sizeof(target), "root@%s", linklocal);
				ssh[1] = target;
				mesh_run_capture(ssh, NULL, 0);
			}
			printf("\nDisconnected from meshnode \"%s\"\n\n", ndpmacf);
			break;
		} else {
			char ip6[96];
			char *pct = strchr(linklocal, '%');
			snprintf(ip6, sizeof(ip6), "%s", linklocal);
			if (pct) {
				size_t nlen = (size_t)(pct - linklocal);
				if (nlen >= sizeof(ip6))
					nlen = sizeof(ip6) - 1;
				ip6[nlen] = '\0';
			}
			printf("Node ID: %s [ ipaddress: %s ]\n", ndpmacf, ip6);
		}
		(void)filter_list;
	}
	puts("=============================================================================================================================================================================");
	puts("");
	return 0;
}

int mesh_cli_copy(struct mesh_ctx *ctx, int argc, char **argv)
{
	char nodeaddr[64] = "";
	const char *filepath = NULL;
	char entries[64][96];
	int n, i;

	puts("=============================================================================================================================================================================");
	puts(" Copy a file to /tmp/mesh11sd/ on a remote meshnode");
	puts("    Usage: mesh11sd copy [Node ID] [path_to_file]");
	puts("           mesh11sd copy filter_list   (show only nodes from this contiguous mesh)");
	puts("");

	if (ctx->auto_config == 0) {
		puts("================================================================");
		puts("Unable to copy -  enable auto_config to copy to other meshnodes");
		puts("================================================================");
		return 1;
	}
	if (argc >= 3 && strcmp(argv[2], "filter_list") != 0)
		normalize_nodeaddr(argv[2], nodeaddr, sizeof(nodeaddr));
	if (argc >= 4)
		filepath = argv[3];

	wait_for_nodes(ctx);
	mesh_get_portal_ula(ctx, "get");
	n = load_connectlist(entries, 64);

	if (!nodeaddr[0]) {
		puts(" If the node you are looking for is not in the list - re-run this command.");
		puts("=============================================================================================================================================================================");
		puts(" The following meshnodes are available for remote copy:");
	}

	for (i = 0; i < n; i++) {
		char linklocal[128] = "", ndpmac[32] = "";
		char *comma = strchr(entries[i], ',');
		char ndpmacf[40];
		int j;
		if (!comma)
			continue;
		*comma = '\0';
		snprintf(linklocal, sizeof(linklocal), "%s", entries[i]);
		snprintf(ndpmac, sizeof(ndpmac), "%s", comma + 1);
		for (j = 0; ndpmac[j]; j++)
			ndpmacf[j] = (ndpmac[j] == ':') ? '-' : ndpmac[j];
		ndpmacf[j] = '\0';

		if (!nodeaddr[0]) {
			printf("Node ID: %s\n", ndpmacf);
			continue;
		}
		if (strcmp(nodeaddr, ndpmac) != 0)
			continue;
		if (!filepath || !filepath[0]) {
			puts("Missing path_to_file");
			return 1;
		}
		{
			char dest[256];
			const char *base = strrchr(filepath, '/');
			char *scp[] = { "scp", (char *)filepath, dest, NULL };
			base = base ? base + 1 : filepath;
			snprintf(dest, sizeof(dest), "root@%s:%s/%s",
				 linklocal, MESH11SD_TMPDIR, base);
			printf("Copying to meshnode \"%s\".....\n", ndpmacf);
			mesh_run_capture(scp, NULL, 0);
			printf("Done.\n");
		}
		break;
	}
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
