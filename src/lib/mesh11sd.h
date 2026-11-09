/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MESH11SD_H
#define MESH11SD_H

#include <stddef.h>
#include <sys/types.h>
#include <stdint.h>

#ifndef MESH11SD_VERSION
#define MESH11SD_VERSION "8.0.0_alpha1"
#endif

#define MESH11SD_TMPDIR "/tmp/mesh11sd"
#define MESH11SD_CONFIG "mesh11sd"
#define MESH11SD_LOG_NAME "mesh11sd_log.log"

struct mesh_ctx {
	int debuglevel;		/* 0..3, from setup.debuglevel (default 3) */
	int enabled;		/* setup.enabled (default 1) */
	int commit_all;		/* setup.commit_all (default 0) = uncommitted */
	int checkinterval;	/* seconds */
	int max_log_entries;
	int mute;		/* 1 = CLI (muted syslog volume), 0 = daemon */
	unsigned long entrynum;
	char log_mountpoint[128];
	char last_syslogmessage[512];
	pid_t pid;
};

/* uci_ops: all UCI reads/writes. Stage 1 matches v7 uncommitted default.
 * When commit_all==1, apply paths may commit (daemon chunks); CLI
 * commit_changes/commit_all/revert_all land in port chunk 9. */
int mesh_uci_get(const char *tuple, char *buf, size_t buflen);
int mesh_uci_set(const char *tuple, const char *value); /* uncommitted */
int mesh_uci_commit(const char *package);
int mesh_uci_revert(const char *package);
int mesh_uci_changes(const char *package, char *buf, size_t buflen);

void mesh_ctx_init(struct mesh_ctx *ctx, int is_daemon);
int mesh_ctx_load_setup(struct mesh_ctx *ctx);

void mesh_write_to_syslog(struct mesh_ctx *ctx, const char *debugtype, const char *msg);
void mesh_write_log(struct mesh_ctx *ctx, const char *debugtype, const char *msg);
int mesh_read_log(struct mesh_ctx *ctx, int follow);

int mesh_is_hex(const char *s);
int mesh_str_to_hex(const char *in, char *out, size_t outlen);
int mesh_hex_to_str(const char *in, char *out, size_t outlen);
int mesh_is_ipv4addr_valid(const char *s, char *canon, size_t canonlen);

int mesh_get_factory_mac(struct mesh_ctx *ctx, char *mac_out, size_t mac_len);

int mesh_cli_dispatch(struct mesh_ctx *ctx, int argc, char **argv);
int mesh_daemon_run(struct mesh_ctx *ctx);

#endif
