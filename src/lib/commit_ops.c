/* SPDX-License-Identifier: GPL-2.0-or-later
 * Chunk 9: commit_changes / commit_all / revert_all — both UCI modes.
 */
#include "mesh11sd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const COMMIT_PKGS[] = {
	"mesh11sd", "wireless", "dhcp", "network", "firewall", "system", NULL
};

int mesh_cli_commit_changes(struct mesh_ctx *ctx, int argc, char **argv)
{
	char changes[4096] = "";
	int do_commit = (argc >= 3 && !strcmp(argv[2], "commit"));
	char debuglevel_change[256] = "";
	char rssi_change[256] = "";
	char txpower_change[256] = "";
	char leech_change[256] = "";
	char mob_change[256] = "";
	char *p, *line;

	puts("===========================================================================");
	puts("Commit changes, making them non volatile on restart/reboot");
	if (!do_commit) {
		puts("    Usage: mesh11sd commit_changes commit ");
		puts("    Writes changes to configuration file ");
		puts("===========================================================================");
		puts("");
		return 0;
	}

	mesh_uci_changes("mesh11sd", changes, sizeof(changes));
	p = changes;
	while ((line = strsep(&p, "\n")) != NULL) {
		if (!line[0])
			continue;
		if (strstr(line, "debuglevel"))
			snprintf(debuglevel_change, sizeof(debuglevel_change), "%s", line);
		if (strstr(line, "mesh_rssi_threshold"))
			snprintf(rssi_change, sizeof(rssi_change), "%s", line);
		if (strstr(line, "txpower"))
			snprintf(txpower_change, sizeof(txpower_change), "%s", line);
		if (strstr(line, "mesh_leechmode_enable"))
			snprintf(leech_change, sizeof(leech_change), "%s", line);
		if (strstr(line, "mesh_node_mobility_level"))
			snprintf(mob_change, sizeof(mob_change), "%s", line);
	}

	mesh_uci_revert("mesh11sd");
	if (debuglevel_change[0]) {
		char setl[280];
		snprintf(setl, sizeof(setl), "set %s", debuglevel_change);
		mesh_uci_batch_line(setl);
		puts("Debuglevel change committed");
	}
	if (rssi_change[0]) {
		char setl[280];
		snprintf(setl, sizeof(setl), "set %s", rssi_change);
		mesh_uci_batch_line(setl);
		puts("Mesh RSSI change committed");
	}
	if (txpower_change[0]) {
		char setl[280];
		snprintf(setl, sizeof(setl), "set %s", txpower_change);
		mesh_uci_batch_line(setl);
		puts("TX power change committed");
	}
	if (leech_change[0]) {
		char setl[280];
		snprintf(setl, sizeof(setl), "set %s", leech_change);
		mesh_uci_batch_line(setl);
		puts("Mesh leechmode change committed");
	}
	if (mob_change[0]) {
		char setl[280];
		snprintf(setl, sizeof(setl), "set %s", mob_change);
		mesh_uci_batch_line(setl);
		puts("Mesh node mobility level change committed");
	}
	mesh_uci_commit("mesh11sd");
	puts("===========================================================================");
	puts("");
	(void)ctx;
	return 0;
}

int mesh_cli_commit_all(struct mesh_ctx *ctx, int argc, char **argv)
{
	int silent = (argc >= 4 && !strcmp(argv[3], "silent"));
	int do_commit = (argc >= 3 && !strcmp(argv[2], "commit"));
	char status[16] = "0";
	char reply[32] = "";
	int i;

	if (ctx->auto_config == 0) {
		if (!silent)
			puts("Auto config is not enabled - aborting...");
		return 1;
	}
	mesh_uci_get("mesh11sd.setup.commit_all", status, sizeof(status));
	if (atoi(status) == 1) {
		if (!silent)
			puts("A commit_all command has already been run. Use revert_all to revert.");
		return 1;
	}

	if (!silent) {
		puts("==========================================================================================================");
		puts("Commit ***ALL*** changes, including auto_configuration changes, making them non volatile on restart/reboot");
		if (!do_commit) {
			puts("    Usage: mesh11sd commit_all commit ");
			puts("    Writes changes to configuration file ");
			puts("    Active only when auto_config is enabled ");
			puts("==========================================================================================================");
			puts("");
			return 0;
		}
		puts("    Dynamic auto_configure options will be written to non volatile storage and LuCi UI will be enabled..... ");
		puts("    This is for advanced users, continue at your own risk.");
		puts("    This process can be reverted with the \"revert_all\" command.");
		puts("");
		fputs("    Do you want to continue? [yes/no] ", stdout);
		fflush(stdout);
		if (!fgets(reply, sizeof(reply), stdin))
			reply[0] = '\0';
		{
			size_t n = strlen(reply);
			while (n && (reply[n - 1] == '\n' || reply[n - 1] == '\r'))
				reply[--n] = '\0';
		}
	}

	if (silent || !strcmp(reply, "yes")) {
		mesh_uci_batch_line("set mesh11sd.setup.commit_all='1'");
		mesh_uci_batch_line("set mesh11sd.setup.portal_use_default_ipv4='1'");
		for (i = 0; COMMIT_PKGS[i]; i++)
			mesh_uci_commit(COMMIT_PKGS[i]);
		ctx->commit_all = 1;
	} else if (!silent) {
		puts("Aborting commit_all");
	}
	if (!silent) {
		puts("==========================================================================================================");
		puts("");
	}
	return 0;
}

int mesh_cli_revert_all(struct mesh_ctx *ctx, int argc, char **argv)
{
	int do_revert = (argc >= 3 && !strcmp(argv[2], "revert"));
	char reply[32] = "";
	int i;

	if (ctx->auto_config == 0) {
		puts("Auto config is not enabled - aborting...");
		return 1;
	}
	puts("==========================================================================================================");
	puts("Revert ***ALL*** changes previously committed by a commit_all command");
	if (!do_revert) {
		puts("    Usage: mesh11sd revert_all revert ");
		puts("    Reverts previously committed changes ");
		puts("    Active only when auto_config is enabled ");
		puts("    Connection will be lost if you proceed as node will restart..... ");
		puts("==========================================================================================================");
		puts("");
		return 0;
	}
	puts("    Connection will be lost if you proceed as node will restart..... ");
	fputs("    Do you want to continue? [yes/no] ", stdout);
	fflush(stdout);
	if (!fgets(reply, sizeof(reply), stdin))
		reply[0] = '\0';
	{
		size_t n = strlen(reply);
		while (n && (reply[n - 1] == '\n' || reply[n - 1] == '\r'))
			reply[--n] = '\0';
	}
	if (!strcmp(reply, "yes")) {
		for (i = 0; COMMIT_PKGS[i]; i++) {
			char src[128], dst[64];
			snprintf(src, sizeof(src), "/etc/config/%s_mesh11sd_default",
				 COMMIT_PKGS[i]);
			snprintf(dst, sizeof(dst), "/etc/config/%s", COMMIT_PKGS[i]);
			if (access(src, R_OK) == 0) {
				char *argv_cp[] = { "cp", src, dst, NULL };
				mesh_run_capture(argv_cp, NULL, 0);
			}
		}
		puts("Revert complete, auto configuration will rebuild - restarting......");
		for (i = 0; i < 5; i++) {
			sleep(1);
			fputs("* ", stdout);
			fflush(stdout);
		}
		puts("");
		puts("==========================================================================================================");
		puts("");
		puts("Rebooting...");
		{
			char *sync_a[] = { "sync", NULL };
			char *reb[] = { "reboot", NULL };
			mesh_run_capture(sync_a, NULL, 0);
			mesh_run_capture(reb, NULL, 0);
		}
	} else {
		puts("Aborting revert_all");
	}
	puts("==========================================================================================================");
	puts("");
	return 0;
}

/* auto_config=2: silent commit_all once mesh is active (v7) */
int mesh_maybe_silent_commit_all(struct mesh_ctx *ctx)
{
	char status[16] = "0";
	char *fake_argv[] = { "mesh11sd", "commit_all", "commit", "silent", NULL };

	if (ctx->auto_config != 2)
		return 0;
	mesh_uci_get("mesh11sd.setup.commit_all", status, sizeof(status));
	if (atoi(status) == 1)
		return 0;
	return mesh_cli_commit_all(ctx, 4, fake_argv);
}
