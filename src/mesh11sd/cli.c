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
"  Option: read_log\n"
"  Usage: mesh11sd read_log [ -f ]\n"
"\n"
"####\n"
"  For further documentation, see: https://github.com/openNDS/mesh11sd#readme\n"
);
}

static int cmd_status(struct mesh_ctx *ctx)
{
	char procd[64] = "unknown";
	FILE *f;

	f = popen("/etc/init.d/mesh11sd status 2>/dev/null", "r");
	if (f) {
		if (!fgets(procd, sizeof(procd), f))
			snprintf(procd, sizeof(procd), "unknown");
		pclose(f);
		/* trim */
		{
			size_t n = strlen(procd);
			while (n > 0 && (procd[n - 1] == '\n' || procd[n - 1] == '\r'))
				procd[--n] = '\0';
		}
	}

	printf("{\n");
	printf("  \"setup\":{\n");
	printf("    \"version\":\"%s\",\n", MESH11SD_VERSION);
	printf("    \"enabled\":\"%d\",\n", ctx->enabled);
	printf("    \"procd_status\":\"%s\",\n", procd);
	printf("    \"debuglevel\":\"%d\",\n", ctx->debuglevel);
	printf("    \"commit_all\":\"%d\",\n", ctx->commit_all);
	printf("    \"port_note\":\"chunk1-minimal-status\"\n");
	printf("  }\n");
	printf("}\n");
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

	/* Commands below need setup (as v7 calls get_current_setup). */
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
		return cmd_status(ctx);

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

	if (!strcmp(cmd, "daemon"))
		return mesh_daemon_run(ctx);

	fprintf(stderr, "Unrecognised command - For help, try mesh11sd --help \n");
	return 1;
}
