/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mesh11sd.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int mesh_run_capture(char *const argv[], char *out, size_t outlen)
{
	int pipefd[2];
	pid_t pid;
	ssize_t n;
	size_t total = 0;
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
		int devnull = open("/dev/null", O_WRONLY);
		close(pipefd[0]);
		if (dup2(pipefd[1], STDOUT_FILENO) < 0)
			_exit(127);
		close(pipefd[1]);
		if (devnull >= 0) {
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}
		execvp(argv[0], argv);
		_exit(127);
	}
	close(pipefd[1]);
	if (out && outlen) {
		out[0] = '\0';
		while (total + 1 < outlen &&
		       (n = read(pipefd[0], out + total, outlen - 1 - total)) > 0)
			total += (size_t)n;
		out[total] = '\0';
		while (total > 0 && (out[total - 1] == '\n' || out[total - 1] == '\r'))
			out[--total] = '\0';
	} else {
		char sink[256];
		while (read(pipefd[0], sink, sizeof(sink)) > 0)
			;
	}
	close(pipefd[0]);
	if (waitpid(pid, &status, 0) < 0)
		return -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;
	return 0;
}

int mesh_sha256_hex(const char *in, char *out, size_t outlen)
{
	char *argv[] = { "sha256sum", NULL };
	int pipefd[2];
	pid_t pid;
	int status;
	char buf[128];
	ssize_t n;
	size_t total = 0;

	if (!in || !out || outlen < 65)
		return -1;
	if (pipe(pipefd) < 0)
		return -1;
	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (pid == 0) {
		int pstdin[2];
		if (pipe(pstdin) < 0)
			_exit(127);
		pid_t writer = fork();
		if (writer == 0) {
			close(pstdin[0]);
			close(pipefd[0]);
			close(pipefd[1]);
			write(pstdin[1], in, strlen(in));
			close(pstdin[1]);
			_exit(0);
		}
		close(pstdin[1]);
		dup2(pstdin[0], STDIN_FILENO);
		close(pstdin[0]);
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		close(pipefd[1]);
		int dn = open("/dev/null", O_WRONLY);
		if (dn >= 0) {
			dup2(dn, STDERR_FILENO);
			close(dn);
		}
		execvp("sha256sum", argv);
		_exit(127);
	}
	close(pipefd[1]);
	buf[0] = '\0';
	while (total + 1 < sizeof(buf) &&
	       (n = read(pipefd[0], buf + total, sizeof(buf) - 1 - total)) > 0)
		total += (size_t)n;
	buf[total] = '\0';
	close(pipefd[0]);
	waitpid(pid, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;
	/* first 64 hex chars */
	for (n = 0; n < 64 && buf[n]; n++) {
		if (!((buf[n] >= '0' && buf[n] <= '9') ||
		      (buf[n] >= 'a' && buf[n] <= 'f')))
			break;
		out[n] = buf[n];
	}
	if (n != 64)
		return -1;
	out[64] = '\0';
	return 0;
}

int mesh_is_installed(const char *package)
{
	char out[512];
	char *apk_argv[] = { "apk", "list", "-I", (char *)package, NULL };
	char *opkg_argv[] = { "opkg", "list-installed", NULL };

	if (!package || !package[0])
		return 2;
	if (mesh_run_capture(apk_argv, out, sizeof(out)) == 0) {
		if (strstr(out, package))
			return 0;
		return 1;
	}
	if (mesh_run_capture(opkg_argv, out, sizeof(out)) == 0) {
		char needle[128];
		snprintf(needle, sizeof(needle), "%s ", package);
		if (strstr(out, needle) || strstr(out, package))
			return 0;
		return 1;
	}
	return 127;
}

int mesh_check_package_any(const char *const *names, int *installed_flags)
{
	int i, any = 1;

	for (i = 0; names[i]; i++) {
		int rc = mesh_is_installed(names[i]);
		if (installed_flags)
			installed_flags[i] = rc;
		if (rc == 0)
			any = 0;
	}
	return any;
}

int mesh_file_write(const char *path, const char *content)
{
	FILE *f = fopen(path, "w");
	if (!f)
		return -1;
	fputs(content ? content : "", f);
	fclose(f);
	return 0;
}

int mesh_mkdir_p(const char *path)
{
	char tmp[256];
	size_t len, i;

	if (!path || !path[0])
		return -1;
	snprintf(tmp, sizeof(tmp), "%s", path);
	len = strlen(tmp);
	for (i = 1; i < len; i++) {
		if (tmp[i] == '/') {
			tmp[i] = '\0';
			mkdir(tmp, 0755);
			tmp[i] = '/';
		}
	}
	return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

int mesh_generate_ipv4_base(struct mesh_ctx *ctx, const char *seed, char *out, size_t outlen)
{
	char hash[72];
	int i;
	const int skips[] = { 168, 167, 8, 1, 0, -1 };

	if (!seed || !out || outlen < 16)
		return -1;
	out[0] = '\0';
	if (mesh_sha256_hex(seed, hash, sizeof(hash)) != 0)
		return -1;
	/* v7: for i in seq 9 1 64; take last i chars of digest, first 2 as hex subnet */
	for (i = 9; i <= 64; i++) {
		char pair[3];
		unsigned subnet;
		int s, skip = 0;
		char show[4096];
		size_t hlen = strlen(hash);
		size_t start;

		if ((size_t)i > hlen)
			continue;
		start = hlen - (size_t)i;
		pair[0] = hash[start];
		pair[1] = hash[start + 1];
		pair[2] = '\0';
		subnet = (unsigned)strtoul(pair, NULL, 16);
		if (subnet >= 255)
			continue;
		for (s = 0; skips[s] >= 0; s++) {
			if ((int)subnet == skips[s]) {
				skip = 1;
				break;
			}
		}
		if (skip)
			continue;
		snprintf(out, outlen, "192.168.%u.1", subnet);
		/* v7: is_used=$(uci show network | grep -c "$newipaddr") */
		{
			char *uargv[] = { "uci", "show", "network", NULL };
			if (mesh_run_capture(uargv, show, sizeof(show)) == 0 &&
			    strstr(show, out))
				continue;
		}
		mesh_log_debug(ctx, "Validated ipv4 address [ %s ] using seed value [ %s ]",
			       out, seed);
		return 0;
	}
	return -1;
}

int mesh_apply_portal_ipv4(struct mesh_ctx *ctx)
{
	char seed[128];
	char newip[32];
	char setl[160];

	/* v7 autostartup: when portal_use_default_ipv4=0, all nodes calculate
	 * the same address; portals keep proto=static and use it. */
	if (ctx->portal_use_default_ipv4 != 0)
		return 0;
	if (ctx->portal_detect == 3) {
		if (!ctx->factory_mac[0])
			mesh_get_factory_mac(ctx, ctx->factory_mac, sizeof(ctx->factory_mac));
		snprintf(seed, sizeof(seed), "%s", ctx->factory_mac);
	} else {
		snprintf(seed, sizeof(seed), "%s", ctx->auto_mesh_id);
	}
	if (!seed[0] || mesh_generate_ipv4_base(ctx, seed, newip, sizeof(newip)) != 0)
		return -1;
	snprintf(setl, sizeof(setl), "set network.lan.ipaddr='%s'", newip);
	mesh_uci_batch_line(setl);
	mesh_uci_batch_line("set network.lan.netmask='255.255.255.0'");
	mesh_uci_batch_line("set network.lan.ip6assign='64'");
	mesh_uci_batch_line("set network.wan6.reqprefix='auto'");
	mesh_uci_batch_line("set network.wan6.reqaddress='try'");
	mesh_log_notice(ctx, "portal ipv4 [ %s ] (seeded from mesh config)", newip);
	return 0;
}
