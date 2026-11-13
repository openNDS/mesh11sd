/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mesh11sd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void print_help(void)
{
	puts(
"\n"
"  Usage: mesh11sd [option] [argument...]]\n"
"\n"
"####\n"
"  Option: -h --help help\n"
"  Returns: This help information\n"
"\n"
"####\n"
"  Option: -v --version version\n"
"  Returns: The mesh11sd version\n"
"\n"
"####\n"
"  Option: debuglevel\n"
"  Argument: 0 - silent, 1 - notice, 2 - info, 3 - debug\n"
"  Returns: The mesh11sd debug level\n"
"\n"
"####\n"
"  Option: enable\n"
"  Returns: \"1\" and exit code 0 if successful, exit code 1 if was already enabled\n"
"\n"
"####\n"
"  Option: disable\n"
"  Returns: \"0\" and exit code 0 if successful, exit code 1 if was already disabled\n"
"\n"
"####\n"
"  Option: status\n"
"  Returns: the mesh status in json format (minimal in this alpha)\n"
"\n"
"####\n"
"  Option: get_factory_mac\n"
"  Returns: factory / label MAC address\n"
"\n"
"####\n"
"  Option: str_to_hex\n"
"  Usage: mesh11sd str_to_hex [access_point_data_string]\n"
"\n"
"####\n"
"  Option: hex_to_str\n"
"  Usage: mesh11sd hex_to_str [hex encoded_access_point_data_string]\n"
"\n"
"####\n"
"  Option: is_hex\n"
"  Usage: mesh11sd is_hex [string]\n"
"\n"
"####\n"
"  Option: is_ipv4addr_valid [string]\n"
"\n"
"####\n"
"  Option: write_to_syslog\n"
"  Usage: mesh11sd write_to_syslog [string to log] [debuglevel (debug, info, warn, notice, err or emerg)]\n"
"\n"
"####\n"
"  Option: write_log\n"
"  Usage: mesh11sd write_log [string to log] [debuglevel (debug, info, warn, notice, err or emerg)]\n"
"\n"
"####\n"
"  Option: restore_all\n"
"  Usage: mesh11sd restore_all restore\n"
"  Restores wireless, dhcp, network, firewall and system to how they were before mesh11sd first ran, keeping the mesh11sd config, then reboots\n"
"\n"
"####\n"
"  Option: read_log\n"
"  Usage: mesh11sd read_log [ -f ]\n"
"\n"
"####\n"
"  For further documentation, see: https://github.com/openNDS/mesh11sd#readme\n"
);
}



int mesh_cli_dispatch(struct mesh_ctx *ctx, int argc, char **argv)
{
	const char *cmd;

	if (argc < 2 || !argv[1][0] ||
	    !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help") ||
	    !strcmp(argv[1], "help")) {
		print_help();
		return 0;
	}

	cmd = argv[1];

	if (!strcmp(cmd, "-v") || !strcmp(cmd, "--version") || !strcmp(cmd, "version")) {
		printf("mesh11sd version %s\n", MESH11SD_VERSION);
		return 0;
	}

	/* Commands below need setup (as v7 calls get_current_setup).
	 * The daemon reads it itself at startup, so skip it here. */
	if (strcmp(cmd, "daemon") != 0)
		mesh_ctx_load_setup(ctx);

	if (!strcmp(cmd, "debuglevel")) {
		if (argc < 3 || !argv[2][0]) {
			printf("%d\n", ctx->debuglevel);
			return 0;
		} else {
			int v = atoi(argv[2]);
			char val[8];

			if (v < 0 || v > 3) {
				puts("Invalid debuglevel requested");
				return 0;
			}
			snprintf(val, sizeof(val), "%d", v);
			mesh_uci_set("mesh11sd.setup.debuglevel", val);
			printf("%d\n", v);
			return 0;
		}
	}

	if (!strcmp(cmd, "enable")) {
		if (ctx->enabled == 1) {
			puts("1");
			return 1;
		}
		mesh_uci_set("mesh11sd.setup.enabled", "1");
		puts("1");
		return 0;
	}

	if (!strcmp(cmd, "disable")) {
		if (ctx->enabled == 0) {
			puts("0");
			return 1;
		}
		mesh_uci_set("mesh11sd.setup.enabled", "0");
		puts("0");
		return 0;
	}

	if (!strcmp(cmd, "status"))
		return mesh_cli_status(ctx);

	if (!strcmp(cmd, "get_factory_mac")) {
		char mac[32];
		int rc = mesh_get_factory_mac(ctx, mac, sizeof(mac));

		if (rc == 0)
			printf("%s\n", mac);
		else
			puts("");
		return rc == 0 ? 0 : 1;
	}

	if (!strcmp(cmd, "str_to_hex")) {
		char out[4096];
		const char *in = (argc >= 3) ? argv[2] : "";
		char path[128];
		FILE *f;

		if (mesh_str_to_hex(in, out, sizeof(out)) != 0)
			return 1;
		mkdir(MESH11SD_TMPDIR "/apmond", 0755);
		snprintf(path, sizeof(path), "%s/apmond/hexencoded", MESH11SD_TMPDIR);
		f = fopen(path, "w");
		if (f) {
			fputs(out, f);
			fclose(f);
		}
		fputs(out, stdout);
		return 0;
	}

	if (!strcmp(cmd, "hex_to_str")) {
		char out[4096];
		const char *in = (argc >= 3) ? argv[2] : "";

		if (mesh_hex_to_str(in, out, sizeof(out)) != 0) {
			fputs("", stdout);
			return 0;
		}
		fputs(out, stdout);
		return 0;
	}

	if (!strcmp(cmd, "is_hex"))
		return mesh_is_hex((argc >= 3) ? argv[2] : "") ? 0 : 1;

	if (!strcmp(cmd, "is_ipv4addr_valid")) {
		char canon[32];
		int ok = mesh_is_ipv4addr_valid((argc >= 3) ? argv[2] : "", canon, sizeof(canon));

		if (ok)
			fputs(canon, stdout);
		return ok ? 0 : 1;
	}

	if (!strcmp(cmd, "write_to_syslog")) {
		if (argc < 3 || !argv[2][0])
			return 1;
		ctx->mute = 0;
		mesh_write_to_syslog(ctx, (argc >= 4) ? argv[3] : "notice", argv[2]);
		return 0;
	}

	if (!strcmp(cmd, "write_log")) {
		if (argc < 3 || !argv[2][0])
			return 1;
		mesh_write_log(ctx, (argc >= 4) ? argv[3] : "notice", argv[2]);
		return 0;
	}

	if (!strcmp(cmd, "read_log")) {
		int follow = 0;

		if (argc >= 3 && (!strcmp(argv[2], "-f") || !strcmp(argv[2], "-F")))
			follow = 1;
		return mesh_read_log(ctx, follow);
	}

	if (!strcmp(cmd, "connect"))
		return mesh_cli_connect(ctx, argc, argv);
	if (!strcmp(cmd, "copy"))
		return mesh_cli_copy(ctx, argc, argv);
	if (!strcmp(cmd, "mesh_leechmode"))
		return mesh_cli_mesh_leechmode(ctx, argc >= 3 ? argv[2] : NULL);
	if (!strcmp(cmd, "mesh_node_mobility_level"))
		return mesh_cli_mesh_node_mobility_level(ctx, argc >= 3 ? argv[2] : NULL);
	if (!strcmp(cmd, "force_ipv4_download"))
		return mesh_cli_force_ipv4_download(ctx);
	if (!strcmp(cmd, "download_revert_to_default"))
		return mesh_cli_download_revert_to_default(ctx);
	if (!strcmp(cmd, "dhcp4_renew"))
		return mesh_cli_dhcp4_renew(ctx, argc >= 3 ? argv[2] : NULL);
	if (!strcmp(cmd, "is_installed"))
		return mesh_cli_is_installed(ctx, argc >= 3 ? argv[2] : NULL);

	if (!strcmp(cmd, "commit_changes"))
		return mesh_cli_commit_changes(ctx, argc, argv);
	if (!strcmp(cmd, "commit_all"))
		return mesh_cli_commit_all(ctx, argc, argv);
	if (!strcmp(cmd, "revert_all"))
		return mesh_cli_revert_all(ctx, argc, argv);
	if (!strcmp(cmd, "restore_all"))
		return mesh_cli_restore_all(ctx, argc, argv);

	if (!strcmp(cmd, "daemon"))
		return mesh_daemon_run(ctx);

	if (!strcmp(cmd, "get_portal_ula")) {
		mesh_get_portal_ula(ctx, argc >= 3 ? argv[2] : "get");
		if (access(MESH11SD_TMPDIR "/active_portal", R_OK) == 0) {
			FILE *f = fopen(MESH11SD_TMPDIR "/active_portal", "r");
			char line[256];
			if (f && fgets(line, sizeof(line), f))
				fputs(line, stdout);
			if (f)
				fclose(f);
		}
		return 0;
	}

	if (!strcmp(cmd, "get_portal_type")) {
		mesh_get_portal_type(ctx);
		printf("%s\n", ctx->portal_type[0] ? ctx->portal_type : "UNKNOWN");
		return 0;
	}

	if (!strcmp(cmd, "set_ula_prefix")) {
		const char *mode = argc >= 3 ? argv[2] : "get";
		mesh_set_ula_prefix(ctx, mode);
		fputs(ctx->ula_prefix, stdout);
		return 0;
	}


	if (!strcmp(cmd, "txpower"))
		return mesh_cli_txpower(ctx, argc >= 3 ? argv[2] : NULL);

	if (!strcmp(cmd, "stations"))
		return mesh_cli_stations(ctx);

	if (!strcmp(cmd, "country"))
		return mesh_cli_country(ctx, argc >= 3 ? argv[2] : NULL);

	if (!strcmp(cmd, "mesh_rssi_threshold")) {
		int force = (argc >= 4 && !strcmp(argv[3], "force"));
		return mesh_cli_mesh_rssi_threshold(ctx, argc >= 3 ? argv[2] : NULL, force);
	}

	if (!strcmp(cmd, "get_valid_channels")) {
		char ch[1024];
		if (mesh_get_valid_channels(ch, sizeof(ch)) == 0)
			printf("%s\n", ch);
		return 0;
	}

	if (!strcmp(cmd, "is_channel_valid")) {
		int rc = mesh_is_channel_valid(argc >= 3 ? argv[2] : "",
					       argc >= 4 ? argv[3] : "");
		puts(rc == 0 ? "valid" : "invalid");
		return rc;
	}

	if (!strcmp(cmd, "get_wiphys")) {
		mesh_get_wiphys(ctx);
		printf("wiphys:");
		for (int i = 0; i < ctx->wiphy_count; i++)
			printf(" %s", ctx->wiphys[i]);
		printf("\nradios:");
		for (int i = 0; i < ctx->radio_count; i++)
			printf(" %s", ctx->radios[i]);
		printf("\n");
		return 0;
	}

	if (!strcmp(cmd, "radiophy")) {
		char phy[64];
		int rc = mesh_radiophy(argc >= 3 ? argv[2] : "", phy, sizeof(phy));
		if (rc == 0)
			printf("%s\n", phy);
		return rc == 0 ? 0 : 1;
	}

	if (!strcmp(cmd, "auto_config")) {
		const char *arg = argc >= 3 ? argv[2] : NULL;
		char reply[64] = "";
		char changes[8192] = "";
		char *p, *line;
		const char *testflag = MESH11SD_TMPDIR "/auto_config_test";

		if (!arg || !arg[0]) {
			puts("");
			puts("Usage:");
			puts("mesh11sd auto_config test - turn on auto config test mode ");
			puts("mesh11sd auto_config enable - configure auto config mode ");
			puts("mesh11sd auto_config disable - disable auto config mode ");
			puts("");
			return 1;
		}
		if (ctx->auto_config != 0 && !strcmp(arg, "test")) {
			puts("auto_config is already enabled, no need to test");
			unlink(testflag);
			return 0;
		}
		if (ctx->auto_config != 0 && !strcmp(arg, "enable")) {
			unlink(testflag);
			puts("auto_config is already enabled");
			return 0;
		}
		if (ctx->auto_config == 0 && !strcmp(arg, "disable")) {
			unlink(testflag);
			puts("auto_config is already disabled");
			return 0;
		}
		if (access(testflag, F_OK) == 0 && ctx->auto_config == 0 &&
		    strcmp(arg, "enable") != 0) {
			puts("Auto config is already running in test mode!");
			return 1;
		}
		if (!strcmp(arg, "test")) {
			if (access(testflag, F_OK) == 0) {
				puts("Auto config test mode is already running!");
				return 1;
			}
			puts("");
			puts("This will test autoconfiguration.");
			puts("Rebooting will revert the test autoconfiguration.");
			puts("");
			puts("    Do you want to continue? [yes/no] ");
			puts("");
			if (!fgets(reply, sizeof(reply), stdin))
				reply[0] = '\0';
			{
				size_t n = strlen(reply);
				while (n && (reply[n - 1] == '\n' || reply[n - 1] == '\r'))
					reply[--n] = '\0';
			}
			if (!strcmp(reply, "yes"))
				mesh_file_write(testflag, "");
			return 0;
		}
		if (!strcmp(arg, "enable") || !strcmp(arg, "disable")) {
			int enable = !strcmp(arg, "enable");
			puts("");
			if (enable) {
				puts("This will enable and start autoconfiguration.");
				puts("Autoconfiguration will start immediately and again on every restart.");
			} else {
				puts("This will disable and stop autoconfiguration.");
				puts("You must restart the device for this to take effect.");
			}
			puts("");
			puts("    Do you want to continue? [yes/no] ");
			puts("");
			if (!fgets(reply, sizeof(reply), stdin))
				reply[0] = '\0';
			{
				size_t n = strlen(reply);
				while (n && (reply[n - 1] == '\n' || reply[n - 1] == '\r'))
					reply[--n] = '\0';
			}
			if (!strcmp(reply, "yes")) {
				unlink(testflag);
				mesh_uci_changes("mesh11sd", changes, sizeof(changes));
				mesh_uci_revert("mesh11sd");
				mesh_uci_set("mesh11sd.setup.auto_config", enable ? "1" : "0");
				mesh_uci_commit("mesh11sd");
				/* re-apply previous uncommitted changes (v7) */
				p = changes;
				while ((line = strsep(&p, "\n")) != NULL) {
					char setl[512];
					if (!line[0])
						continue;
					snprintf(setl, sizeof(setl), "set %s", line);
					mesh_uci_batch_line(setl);
				}
				ctx->auto_config = enable ? 1 : 0;
			}
			return 0;
		}
		puts("Invalid option....");
		return 1;
	}

	fprintf(stderr, "Unrecognised command - For help, try mesh11sd --help \n");
	return 1;
}
