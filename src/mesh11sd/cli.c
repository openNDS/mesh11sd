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
"  Option: auto_config\n"
"  Configure auto config mode\n"
"  Usage:\n"
"    mesh11sd auto_config test | enable | disable\n"
"      Takes immediate effect. Any current connection is likely to be lost and may require reconnection on a different IPv4 address.\n"
"\n"
"        test - turn on auto config test mode. A reboot will revert the auto configuration.\n"
"        enable - enable auto config mode.\n"
"        disable - disable auto config mode.\n"
"\n"
"####\n"
"  Option: status\n"
"  Returns: the mesh status in json format\n"
"\n"
"####\n"
"  Option: connect\n"
"  Connect a remote terminal session on a remote meshnode\n"
"\n"
"  Usage:\n"
"    mesh11sd connect\n"
"        Lists all reachable mesh nodes (including island meshes)\n"
"\n"
"    mesh11sd connect filter_list\n"
"        Lists available mesh nodes in this contiguous mesh only\n"
"\n"
"    mesh11sd connect [Node ID]\n"
"        Connects to the requested meshnode\n"
"\n"
"####\n"
"  Option: copy\n"
"  Copy a file to /tmp/mesh11sd/ on a remote meshnode\n"
"\n"
"  Usage:\n"
"    mesh11sd copy\n"
"        Lists all reachable mesh nodes (including island meshes)\n"
"\n"
"    mesh11sd copy filter_list\n"
"        Lists available mesh nodes in this contiguous mesh only\n"
"\n"
"    mesh11sd copy [Node ID] [path_to_source_file]\n"
"        Copies the file to /tmp/mesh11sd/ on the requested meshnode\n"
"\n"
"####\n"
"  Option: txpower\n"
"  Change the mesh transmit power\n"
"  Usage: mesh11sd txpower [+|-]\n"
"    where \"+\" increments by 3dBm and \"-\" decrements by 3dBm\n"
"    Takes effect immediately\n"
"\n"
"####\n"
"  Option: stations\n"
"  List all mesh peer stations directly connected to this mesh peer station (one hop)\n"
"  Usage: mesh11sd stations\n"
"\n"
"####\n"
"  Option: mesh_rssi_threshold\n"
"  Change the mesh rssi threshold\n"
"  Usage: mesh11sd mesh_rssi_threshold [+|-] [force]\n"
"    where \"+\" increments by 3dBm and \"-\" decrements by 3dBm\n"
"    Takes effect immediately on NEW connections\n"
"    The keyword \"force\" forces the new threshold on all connected peers\n"
"    Warning - \"force\" will briefly remove this node from the mesh network, taking a few seconds to rejoin\n"
"\n"
"####\n"
"  Option: mesh_leechmode\n"
"  Usage: mesh11sd mesh_leechmode [enable/disable]\n"
"  If enabled, the mesh node connects to the mesh backhaul but does not forward packets or contribute to the mesh.\n"
"\n"
"####\n"
"  Option: mesh_node_mobility_level\n"
"  Usage: mesh11sd mesh_node_mobility_level [level]\n"
"  Sets the mesh node mobility level\n"
"    where level can be 0, 1, 2, 3, 4, 5 and 6\n"
"    Level 0 - not recommended for normal use - node must be stationary and carefully positioned\n"
"    Level 1 - Enables mesh_hwmp_rts for on air collision avoidance, enables transmit queue and aql_threshold to minimise latency, enables rapid path convergence\n"
"    Level 2 - supports inter node relative velocities up to 1.5 metres per second\n"
"    Levels 3 to 5 - support progressively higher relative inter node velocities at the expense of a larger and larger backhaul overhead.\n"
"    Level 6 - is a special configuration that is PA / voice optimised (push-to-talk announcement use-case).\n"
"\n"
"####\n"
"  Option: commit_changes\n"
"  Usage: mesh11sd commit_changes commit\n"
"    Commits changes to debuglevel, mesh_leechmode, txpower, mesh_rssi_threshold and mesh_node_mobility_level\n"
"    to non volatile configuration (make permanent). Other staged changes are kept but not committed\n"
"\n"
"####\n"
"  Option: commit_all\n"
"  Usage: mesh11sd commit_all commit\n"
"    Writes all changes made by auto_config to configuration file\n"
"    Active only when auto_config is enabled\n"
"\n"
"####\n"
"  Option: revert_all\n"
"  Usage: mesh11sd revert_all revert\n"
"    Reverts all changes made by commit_all\n"
"    Active only when auto_config is enabled\n"
"\n"
"####\n"
"  Option: force_ipv4_download\n"
"  Usage: mesh11sd force_ipv4_download\n"
"    Forces apk (wget) to use ipv4 for its downloads\n"
"\n"
"####\n"
"  Option: download_revert_to_default\n"
"  Usage: mesh11sd download_revert_to_default\n"
"    Reverts apk (wget) to default for its downloads\n"
"\n"
"####\n"
"  Option: dhcp4_renew\n"
"  Usage: mesh11sd dhcp4_renew\n"
"    Renews the current dhcp4 lease\n"
"\n"
"####\n"
"  Option: is_installed\n"
"  Usage: mesh11sd is_installed [packagename]\n"
"    Checks if \"packagename\" is installed\n"
"    Returns the installation status of the package and exit code 0 if installed\n"
"\n"
"####\n"
"  Option: get_portal_ula\n"
"  Usage: mesh11sd get_portal_ula <mode>\n"
"    where mode is:\n"
"    1. get or not set - Gets the current portal unique local ipv6 address (ula)\n"
"    2. discover - Selects the best portal unique local ipv6 address (ula)\n"
"\n"
"####\n"
"  Option: set_ula_prefix\n"
"  Usage: mesh11sd set_ula_prefix [get|set|revert]\n"
"    get - gets the current ula prefix\n"
"    set - sets the ula prefix for the mesh backhaul based on the mesh id\n"
"    revert - reverts a previously set ula prefix\n"
"\n"
"####\n"
"  Option: str_to_hex\n"
"  Usage: mesh11sd str_to_hex [access_point_data_string]\n"
"    Run length hex encodes access_point_data from apmond format\n"
"\n"
"####\n"
"  Option: hex_to_str\n"
"  Usage: mesh11sd hex_to_str [hex encoded_access_point_data_string]\n"
"    Decode run length hex encoded access_point_data to apmond format\n"
"\n"
"####\n"
"  Option: is_hex\n"
"  Usage: mesh11sd is_hex [string]\n"
"    Check if string contains only valid hex characters (0-9, a-f, A-F)\n"
"    Return code 0 if string is valid hex, 1 otherwise.\n"
"\n"
"####\n"
"  Option: is_ipv4addr_valid [string]\n"
"    Check if string contains a single ipv4 address\n"
"    Return code 0 if string is valid ipv4 address, 1 otherwise.\n"
"\n"
"####\n"
"  Option: country\n"
"  Usage: mesh11sd country [country_code]\n"
"    Shows the regulatory domain and country code if the optional country code is not supplied.\n"
"    Sets the regulatory domain for the country code if it is supplied.\n"
"    A restart or reboot is required for changes to take effect.\n"
"\n"
"####\n"
"  Option: write_to_syslog\n"
"  Usage: mesh11sd write_to_syslog [string to log] [debuglevel (debug, info, warn, notice, err or emerg)]\n"
"    Writes to the syslog at the specified debuglevel\n"
"\n"
"####\n"
"  Option: write_log\n"
"  Usage: mesh11sd write_log [string to log] [debuglevel (debug, info, warn, notice, err or emerg)]\n"
"    Writes to the mesh11sd log at the specified debuglevel\n"
"\n"
"####\n"
"  Option: read_log\n"
"  Usage: mesh11sd read_log [ -f ]\n"
"    Displays the mesh11sd log\n"
"      If -f is specified, the existing log will be deleted and all subsequent logs will be followed on the display\n"
"\n"
"####\n"
"  Option: restore_all\n"
"  Usage: mesh11sd restore_all restore\n"
"    Restores wireless, dhcp, network, firewall and system to how they were before mesh11sd first ran,\n"
"    keeping the mesh11sd config, then reboots\n"
"\n"
"####\n"
"  Option: active_nodecount\n"
"  Usage: mesh11sd active_nodecount\n"
"    Returns the number of mesh paths currently known to this node\n"
"\n"
"####\n"
"  Option: get_node_type_code\n"
"  Usage: mesh11sd get_node_type_code\n"
"    Returns the node type code (MRP, MPE, CPE, MBP, TPE or N/A)\n"
"    Exit code 1 if the daemon has not yet determined the node state\n"
"\n"
"####\n"
"  Option: wifi_chipset_detect\n"
"  Usage: mesh11sd wifi_chipset_detect\n"
"    Detects the capabilities of each wireless phy and returns them in json format\n"
"    The result is also written to /tmp/mesh11sd/wifidetect\n"
"\n"
"####\n"
"  Option: watchdog_nonvolatile_log\n"
"  Usage: mesh11sd watchdog_nonvolatile_log [enable|disable] [commit]\n"
"    FOR DEBUGGING ONLY - this log wears flash memory and uses up storage space.\n"
"    Logs portal watchdog actions, network restarts, reboots, channel scans and other\n"
"    events to /mesh11sd_log/mesh11sd.log, so the reason a node rebooted survives the reboot.\n"
"    With no argument, shows whether the log is off, on until the next restart or reboot, or on and saved\n"
"    enable or disable takes effect within three checkintervals and lasts until\n"
"      mesh11sd restarts or the node reboots\n"
"    commit also saves the setting to the mesh11sd config, so it survives a reboot.\n"
"      Only this option is saved; other pending changes are left as they are.\n"
"\n"
"####\n"
"  Option: get_valid_channels\n"
"  Usage: mesh11sd get_valid_channels\n"
"    Returns a list of valid wireless channels for the current country code setting\n"
"    Note: DFS channels are not suitable for use in a mesh backhaul so are excluded.\n"
"\n"
"####\n"
"  For further documentation, see: https://github.com/openNDS/mesh11sd#readme\n"
"\n"
""
);
}



/* The value saved in /etc/config/mesh11sd, ignoring staged changes. */
static int watchdog_log_saved(void)
{
	FILE *f = fopen("/etc/config/mesh11sd", "r");
	char line[256];
	int saved = 0;

	while (f && fgets(line, sizeof(line), f)) {
		char val[16] = "";

		if (sscanf(line, " option watchdog_nonvolatile_log '%15[^']'", val) == 1 ||
		    sscanf(line, " option watchdog_nonvolatile_log %15s", val) == 1)
			saved = atoi(val);
	}
	if (f)
		fclose(f);
	return saved;
}

/*
 * mesh11sd watchdog_nonvolatile_log [enable|disable] [commit]
 * enable/disable stage the option; the daemon picks it up within three
 * checkintervals. commit saves only this option to flash.
 */
static int watchdog_log_cli(int argc, char **argv)
{
	const char *tuple = "mesh11sd.setup.watchdog_nonvolatile_log";
	char buf[16];
	int live = 0, saved, commit;
	const char *value;

	if (argc < 3) {
		if (mesh_uci_get(tuple, buf, sizeof(buf)) == 0)
			live = atoi(buf);
		saved = watchdog_log_saved();
		if (live <= 0)
			printf("disabled%s\n", saved > 0 ? " (enabled in saved config, active after restart or reboot)" : "");
		else if (saved > 0)
			printf("enabled (saved, survives reboot)\n");
		else
			printf("enabled (until mesh11sd restarts or the node reboots)\n");
		return 0;
	}

	if (!strcmp(argv[2], "enable"))
		value = "1";
	else if (!strcmp(argv[2], "disable"))
		value = "0";
	else {
		fprintf(stderr, "Usage: mesh11sd watchdog_nonvolatile_log [enable|disable] [commit]\n");
		return 1;
	}
	commit = (argc >= 4 && !strcmp(argv[3], "commit"));
	if (argc >= 4 && !commit) {
		fprintf(stderr, "Usage: mesh11sd watchdog_nonvolatile_log [enable|disable] [commit]\n");
		return 1;
	}

	if (commit) {
		if (mesh_uci_commit_option("mesh11sd", tuple, value) != 0)
			return 1;
	} else if (mesh_uci_set(tuple, value) != 0) {
		return 1;
	}
	printf("watchdog_nonvolatile_log %s%s, takes effect within three checkintervals\n",
	       value[0] == '1' ? "enabled" : "disabled",
	       commit ? " and saved" : " until mesh11sd restarts or the node reboots");
	if (value[0] == '1')
		printf("WARNING: this log wears flash memory. Disable it as soon as debugging is done.\n");
	return 0;
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

	if (!strcmp(cmd, "get_node_type_code")) {
		char code[16];
		FILE *f = fopen(MESH11SD_TMPDIR "/is_portal", "r");
		char line[256];

		/* v7: no is_portal yet means no code and exit 1 */
		if (!f)
			return 1;
		if (fgets(line, sizeof(line), f)) {
			char *p = strstr(line, "detected_state=\"");
			if (p)
				sscanf(p + 16, "%15[^\"]", ctx->detected_state);
		}
		fclose(f);
		mesh_get_node_type_code(ctx, code, sizeof(code));
		fputs(code, stdout);
		return 0;
	}

	if (!strcmp(cmd, "active_nodecount")) {
		char ifname[64] = "";
		static char dump[65536];
		int lines = 0;
		FILE *f = fopen(MESH11SD_TMPDIR "/active_mesh_ifname", "r");
		char line[128];

		if (f && fgets(line, sizeof(line), f)) {
			char *q = strstr(line, "active_mesh_ifname=\"");
			if (q)
				sscanf(q + 20, "%63[^\"]", ifname);
		}
		if (f)
			fclose(f);
		if (ifname[0]) {
			char *iw_argv[] = { "iw", "dev", ifname, "mpath", "dump", NULL };

			mesh_run_capture(iw_argv, dump, sizeof(dump));
			/* First line is the column header, as in v7 */
			for (char *c = dump; *c; c++)
				if (*c == '\n')
					lines++;
			if (dump[0] && dump[strlen(dump) - 1] != '\n')
				lines++;
			if (lines > 0)
				lines--;
		}
		printf("%d", lines);
		return 0;
	}

	if (!strcmp(cmd, "watchdog_nonvolatile_log"))
		return watchdog_log_cli(argc, argv);

	if (!strcmp(cmd, "wifi_chipset_detect")) {
		FILE *f;
		char line[512];

		if (mesh_wifi_chipset_detect(ctx) != 0)
			return 1;
		f = fopen(MESH11SD_TMPDIR "/wifidetect", "r");
		while (f && fgets(line, sizeof(line), f))
			fputs(line, stdout);
		if (f)
			fclose(f);
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
