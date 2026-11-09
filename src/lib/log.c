/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mesh11sd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int level_num(const char *debugtype, const char **stderr_flag)
{
	*stderr_flag = "";
	if (!debugtype)
		return 1;
	if (!strcmp(debugtype, "emerg") || !strcmp(debugtype, "err")) {
		*stderr_flag = "-s";
		return 0;
	}
	if (!strcmp(debugtype, "notice"))
		return 1;
	if (!strcmp(debugtype, "warn")) {
		*stderr_flag = "-s";
		return 1;
	}
	if (!strcmp(debugtype, "info"))
		return 2;
	if (!strcmp(debugtype, "debug"))
		return 3;
	return 1;
}

static int is_mountpoint(const char *path)
{
	FILE *f;
	char line[512];
	char dev[256], dir[256];
	int found = 0;

	f = fopen("/proc/mounts", "r");
	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%255s %255s", dev, dir) != 2)
			continue;
		if (!strcmp(dir, path)) {
			found = 1;
			break;
		}
	}
	fclose(f);
	return found;
}

void mesh_write_log(struct mesh_ctx *ctx, const char *debugtype, const char *msg)
{
	char logdir[192];
	char logfile[220];
	char cmd[1536];
	FILE *f;
	time_t now;
	char timestr[64];
	long available = 0, filesize = 1, sizeratio;
	FILE *df;

	if (!ctx || !msg)
		return;

	if (!is_mountpoint(ctx->log_mountpoint)) {
		snprintf(cmd, sizeof(cmd),
			 "logger -p daemon.err -s -t 'mesh11sd[%d]:' "
			 "'Log location is NOT a mountpoint - logs would fill storage space - logging disabled'",
			 (int)ctx->pid);
		system(cmd);
		return;
	}

	snprintf(logdir, sizeof(logdir), "%s/mesh11sd", ctx->log_mountpoint);
	snprintf(logfile, sizeof(logfile), "%s/%s", logdir, MESH11SD_LOG_NAME);
	mkdir(logdir, 0755);

	/* truncate if over max_log_entries */
	{
		FILE *lf = fopen(logfile, "r");
		long lines = 0;
		int c;
		if (lf) {
			while ((c = fgetc(lf)) != EOF)
				if (c == '\n')
					lines++;
			fclose(lf);
		}
		if (lines > ctx->max_log_entries) {
			snprintf(cmd, sizeof(cmd),
				 "mv '%s' '%s.cut' 2>/dev/null; "
				 "tail -n %d '%s.cut' 2>/dev/null >> '%s'; "
				 "rm -f '%s.cut'",
				 logfile, logfile, ctx->max_log_entries,
				 logfile, logfile, logfile);
			system(cmd);
		}
	}

	df = popen("df", "r");
	if (df) {
		char line[256];
		while (fgets(line, sizeof(line), df)) {
			char fs[64], mp[128];
			long blocks, used, avail;
			if (sscanf(line, "%63s %ld %ld %ld %*s %127s",
				   fs, &blocks, &used, &avail, mp) >= 5) {
				if (!strcmp(mp, ctx->log_mountpoint)) {
					available = avail;
					break;
				}
			}
		}
		pclose(df);
	}

	{
		struct stat st;
		if (stat(logfile, &st) == 0 && st.st_size > 0)
			filesize = (long)((st.st_size + 1023) / 1024);
		if (filesize < 1)
			filesize = 1;
	}

	sizeratio = available / filesize;
	if (sizeratio < 10) {
		snprintf(cmd, sizeof(cmd),
			 "logger -p daemon.err -s -t 'mesh11sd[%d]:' "
			 "'Log file too big, please archive contents and reduce max_log_entries'",
			 (int)ctx->pid);
		system(cmd);
		return;
	}

	now = time(NULL);
	strftime(timestr, sizeof(timestr), "%a %b %e %H:%M:%S %Z %Y", localtime(&now));

	f = fopen(logfile, "a");
	if (!f)
		return;
	fprintf(f, "%s daemon.%s mesh11sd[%d]: %s\n",
		timestr, debugtype ? debugtype : "notice", (int)ctx->pid, msg);
	fclose(f);
}

void mesh_write_to_syslog(struct mesh_ctx *ctx, const char *debugtype, const char *msg)
{
	const char *stderr_flag;
	int debugnum;
	char prefixed[640];
	char cmd[768];
	const char *dtype = debugtype ? debugtype : "notice";

	if (!ctx || !msg || !msg[0])
		return;
	if (ctx->last_syslogmessage[0] && !strcmp(msg, ctx->last_syslogmessage))
		return;
	if (ctx->mute)
		return;

	ctx->entrynum++;
	if ((long)ctx->entrynum < 0)
		ctx->entrynum = 1;

	snprintf(prefixed, sizeof(prefixed), "[%lu] %s", ctx->entrynum, msg);
	debugnum = level_num(dtype, &stderr_flag);
	if (!strcmp(dtype, "emerg") || !strcmp(dtype, "err") ||
	    !strcmp(dtype, "notice") || !strcmp(dtype, "warn") ||
	    !strcmp(dtype, "info") || !strcmp(dtype, "debug"))
		; /* keep */
	else
		dtype = "notice";

	if (ctx->debuglevel < debugnum)
		return;

	if (!strcmp(dtype, "emerg") || !strcmp(dtype, "err") || !strcmp(dtype, "notice")) {
		snprintf(cmd, sizeof(cmd),
			 "logger -p 'daemon.%s' %s -t 'mesh11sd[%d]' '%s'",
			 dtype, stderr_flag, (int)ctx->pid, prefixed);
		/* escape single quotes poorly — use safer write via popen later; for now replace */
		system(cmd);
	}

	mesh_write_log(ctx, dtype, prefixed);
	snprintf(ctx->last_syslogmessage, sizeof(ctx->last_syslogmessage), "%s", msg);
}

int mesh_read_log(struct mesh_ctx *ctx, int follow)
{
	char logfile[220];
	char cmd[320];
	FILE *f;
	char line[1024];

	if (!ctx)
		return 1;
	snprintf(logfile, sizeof(logfile), "%s/mesh11sd/%s",
		 ctx->log_mountpoint, MESH11SD_LOG_NAME);
	if (access(logfile, R_OK) != 0)
		return 1;

	if (!follow) {
		f = fopen(logfile, "r");
		if (!f)
			return 1;
		while (fgets(line, sizeof(line), f))
			fputs(line, stdout);
		fclose(f);
		return 0;
	}

	/* follow: print, truncate, then tail -F */
	f = fopen(logfile, "r");
	if (f) {
		while (fgets(line, sizeof(line), f))
			fputs(line, stdout);
		fclose(f);
	}
	unlink(logfile);
	f = fopen(logfile, "w");
	if (f)
		fclose(f);
	snprintf(cmd, sizeof(cmd), "tail -F '%s' 2>/dev/null", logfile);
	return system(cmd) == 0 ? 0 : 1;
}

#include <stdarg.h>

static void mesh_log_v(struct mesh_ctx *ctx, const char *dtype, const char *fmt, va_list ap)
{
	char msg[512];

	vsnprintf(msg, sizeof(msg), fmt, ap);
	mesh_write_to_syslog(ctx, dtype, msg);
}

void mesh_log_info(struct mesh_ctx *ctx, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	mesh_log_v(ctx, "info", fmt, ap);
	va_end(ap);
}

void mesh_log_debug(struct mesh_ctx *ctx, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	mesh_log_v(ctx, "debug", fmt, ap);
	va_end(ap);
}

void mesh_log_notice(struct mesh_ctx *ctx, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	mesh_log_v(ctx, "notice", fmt, ap);
	va_end(ap);
}

void mesh_log_warn(struct mesh_ctx *ctx, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	mesh_log_v(ctx, "warn", fmt, ap);
	va_end(ap);
}

void mesh_log_err(struct mesh_ctx *ctx, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	mesh_log_v(ctx, "err", fmt, ap);
	va_end(ap);
}
