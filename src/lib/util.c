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

static int mesh_sha256_hex_internal(const char *in, char *out, size_t outlen, int add_nl)
{
	char *argv[] = { "sha256sum", NULL };
	int pipefd[2];
	pid_t pid;
	int status;
	char buf[128];
	ssize_t n;
	size_t total = 0;
	size_t inlen;

	if (!in || !out || outlen < 65)
		return -1;
	inlen = strlen(in);
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
			if (inlen)
				write(pstdin[1], in, inlen);
			if (add_nl)
				write(pstdin[1], "\n", 1);
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

/* v7 printf "...' | sha256sum — no trailing newline */
int mesh_sha256_hex(const char *in, char *out, size_t outlen)
{
	return mesh_sha256_hex_internal(in, out, outlen, 0);
}

/* v7 echo "..." | sha256sum — trailing newline included in digest */
int mesh_sha256_hex_echo(const char *in, char *out, size_t outlen)
{
	return mesh_sha256_hex_internal(in, out, outlen, 1);
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

static int path_is_busybox(const char *path)
{
	char link[256];
	ssize_t n;

	if (!path)
		return 0;
	n = readlink(path, link, sizeof(link) - 1);
	if (n < 0)
		return 0;
	link[n] = '\0';
	return strstr(link, "busybox") != NULL;
}

/* Search a file (binary or text) for a literal string. No extra deps. */
static int file_has_string(const char *path, const char *needle)
{
	FILE *f;
	char buf[4096];
	size_t nlen, n, keep = 0, i;

	if (!path || !needle || !needle[0])
		return 0;
	nlen = strlen(needle);
	if (nlen >= sizeof(buf))
		return 0;
	f = fopen(path, "r");
	if (!f)
		return 0;
	while ((n = fread(buf + keep, 1, sizeof(buf) - 1 - keep, f)) > 0) {
		size_t total = keep + n;

		for (i = 0; i + nlen <= total; i++) {
			if (memcmp(buf + i, needle, nlen) == 0) {
				fclose(f);
				return 1;
			}
		}
		if (total >= nlen) {
			memmove(buf, buf + total - (nlen - 1), nlen - 1);
			keep = nlen - 1;
		} else {
			keep = total;
		}
	}
	fclose(f);
	return 0;
}

/* /sbin/ip is iproute2 (ip-full), not a BusyBox applet. Package is fallback. */
static int has_ip_full(void)
{
	static const char *const paths[] = {
		"/sbin/ip", "/usr/sbin/ip", "/usr/bin/ip", NULL
	};
	int i;

	for (i = 0; paths[i]; i++) {
		if (access(paths[i], X_OK) != 0)
			continue;
		if (path_is_busybox(paths[i]))
			continue;
		if (file_has_string(paths[i], "iproute2"))
			return 1;
	}
	return mesh_is_installed("ip-full") == 0;
}

/*
 * A mesh-capable wpad/hostapd/wpa_supplicant has ieee80211s and SAE
 * compiled in. Package list is the v7 fallback.
 */
static int has_mesh_wpad(void)
{
	static const char *const paths[] = {
		"/usr/sbin/wpad", "/usr/sbin/hostapd",
		"/usr/sbin/wpa_supplicant", NULL
	};
	static const char *const pkgs[] = {
		"wpad-mbedtls", "wpad-wolfssl", "wpad-openssl",
		"wpad-mesh-mbedtls", "wpad-mesh-wolfssl", "wpad-mesh-openssl",
		NULL
	};
	int i;

	for (i = 0; paths[i]; i++) {
		if (access(paths[i], X_OK) != 0)
			continue;
		if (file_has_string(paths[i], "ieee80211s") &&
		    file_has_string(paths[i], "SAE"))
			return 1;
	}
	return mesh_check_package_any(pkgs, NULL) == 0;
}

int mesh_check_requirements(struct mesh_ctx *ctx)
{
	/* Bit 0: ath10k-ct present (must be absent)
	 * Bit 1: ip-full missing
	 * Bit 2: mesh-capable wpad missing
	 */
	static int last_mask = -1;
	int mask = 0;

	if (mesh_is_installed("kmod-ath10k-ct") == 0)
		mask |= 1;
	if (!has_ip_full())
		mask |= 2;
	if (!has_mesh_wpad())
		mask |= 4;

	if (mask && mask != last_mask) {
		if (mask & 1)
			mesh_log_err(ctx, "Unable to autoconfigure. Please install ath10k NON ct drivers for mesh support");
		if (mask & 2)
			mesh_log_err(ctx, "Unable to autoconfigure. Please install package ip-full");
		if (mask & 4)
			mesh_log_err(ctx, "Unable to autoconfigure. Please install package wpad-mbedtls, wpad-wolfssl, wpad-openssl or an equivalent mesh only version");
	}
	last_mask = mask;
	return mask ? -1 : 0;
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

int mesh_generate_ipv4_base(struct mesh_ctx *ctx, const char *seed, char *out,
			    size_t outlen, const char *ignore_ip)
{
	char hash[72];
	int i;
	/* v7 skiplist="168 167 8 1 0" */
	const int skips[] = { 168, 167, 8, 1, 0, -1 };

	if (!seed || !out || outlen < 16)
		return -1;
	out[0] = '\0';
	/* v7: printf "$seed" | sha256sum — no trailing newline */
	if (mesh_sha256_hex(seed, hash, sizeof(hash)) != 0)
		return -1;
	/*
	 * v7:
	 *   for i in $(seq 9 1 64); do
	 *     cpipsubnet=$((0x$(printf "$seed" | sha256sum | awk '{printf "%s", $1}' \
	 *         | tail -c$i | head -c2)))
	 * tail -c$i on a 64-char digest starts at offset (64 - i), then head -c2
	 * takes hash[offset] and hash[offset+1].
	 */
	for (i = 9; i <= 64; i++) {
		char pair[3];
		unsigned subnet;
		int s, skip = 0, is_used = 0;
		char show[65536];
		size_t hlen = strlen(hash);
		size_t offset;

		if ((size_t)i > hlen)
			continue;
		offset = hlen - (size_t)i; /* == 64 - i for a full sha256 hex digest */
		pair[0] = hash[offset];
		pair[1] = hash[offset + 1];
		pair[2] = '\0';
		subnet = (unsigned)strtoul(pair, NULL, 16);
		/* v7: if [ $cpipsubnet -ge 255 ]; then continue; fi */
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
		/* Self is not "used elsewhere": ignore_ip is the address on the
		 * option we are about to set (lan or vtunlan). Without this,
		 * a second apply sees 192.168.111.1 as used and picks .102 —
		 * netifd then applies .102 (with brd) while restore_ipv4 adds
		 * .111 via ip addr add (no brd). */
		if (ignore_ip && ignore_ip[0] && strcmp(out, ignore_ip) == 0) {
			mesh_log_debug(ctx,
				       "Validated ipv4 address [ %s ] using seed value [ %s ] (i=%d pair=%s, already ours)",
				       out, seed, i, pair);
			return 0;
		}
		/* v7: is_used=$(uci show network | grep -c "$newipaddr") */
		{
			char *uargv[] = { "uci", "show", "network", NULL };
			if (mesh_run_capture(uargv, show, sizeof(show)) == 0) {
				const char *p = show;
				size_t nlen = strlen(out);
				while ((p = strstr(p, out)) != NULL) {
					is_used++;
					p += nlen;
				}
			}
		}
		if (is_used > 0)
			continue;
		mesh_log_debug(ctx,
			       "Validated ipv4 address [ %s ] using seed value [ %s ] (i=%d pair=%s)",
			       out, seed, i, pair);
		return 0;
	}
	return -1;
}

int mesh_apply_portal_ipv4(struct mesh_ctx *ctx)
{
	char seed[128];
	char newip[32];
	char setl[192];
	char cur[64] = "";
	char net[32];
	char batch[512];
	int applied = 0;

	/* v7 autostartup ~10925: when portal_use_default_ipv4=0, all nodes
	 * calculate the same address; portals keep proto=static and use it. */
	if (ctx->portal_use_default_ipv4 != 0) {
		mesh_log_debug(ctx, "portal ipv4: skipped (portal_use_default_ipv4=%d)",
			       ctx->portal_use_default_ipv4);
		return 0;
	}
	snprintf(net, sizeof(net), "%s",
		 ctx->auto_mesh_network[0] ? ctx->auto_mesh_network : "lan");
	if (ctx->portal_detect == 3) {
		if (!ctx->factory_mac[0])
			mesh_get_factory_mac(ctx, ctx->factory_mac, sizeof(ctx->factory_mac));
		snprintf(seed, sizeof(seed), "%s", ctx->factory_mac);
	} else {
		snprintf(seed, sizeof(seed), "%s", ctx->auto_mesh_id);
	}
	if (!seed[0]) {
		mesh_log_err(ctx, "portal ipv4: empty seed (auto_mesh_id/factory_mac)");
		return -1;
	}
	/* Current lan ipaddr (strip CIDR from stock list form) — ignore in is_used */
	{
		char key[96];
		char *slash;
		snprintf(key, sizeof(key), "network.%s.ipaddr", net);
		if (mesh_uci_get(key, cur, sizeof(cur)) == 0 && cur[0]) {
			slash = strchr(cur, '/');
			if (slash)
				*slash = '\0';
		} else {
			cur[0] = '\0';
		}
	}
	if (mesh_generate_ipv4_base(ctx, seed, newip, sizeof(newip),
				    cur[0] ? cur : NULL) != 0) {
		mesh_log_err(ctx, "portal ipv4: generate_ipv4_base failed");
		return -1;
	}
	/* Idempotent: already the stable portal address — do not rewrite UCI */
	if (cur[0] && strcmp(cur, newip) == 0) {
		mesh_log_debug(ctx, "portal ipv4: already [ %s ] on network.%s",
			       newip, net);
		return 0;
	}

	/* Atomic del+set: stock OpenWrt uses list ipaddr '192.168.1.1/24';
	 * v7 does set network.lan.ipaddr=... (no delete). del+set converts
	 * list→option cleanly so netifd sees a single non-CIDR address. */
	snprintf(batch, sizeof(batch),
		 "del network.%s.ipaddr\n"
		 "set network.%s.ipaddr='%s'\n"
		 "set network.%s.netmask='255.255.255.0'\n"
		 "set network.%s.ip6assign='64'\n"
		 "set network.wan6.reqprefix='auto'\n"
		 "set network.wan6.reqaddress='try'\n",
		 net, net, newip, net, net);
	{
		char *argv[] = { "uci", "batch", NULL };
		int pipefd[2];
		pid_t pid;
		int status;
		if (pipe(pipefd) == 0) {
			pid = fork();
			if (pid == 0) {
				int dn;
				close(pipefd[1]);
				dup2(pipefd[0], 0);
				close(pipefd[0]);
				dn = open("/dev/null", O_WRONLY);
				if (dn >= 0) {
					dup2(dn, 1);
					dup2(dn, 2);
					close(dn);
				}
				execvp("uci", argv);
				_exit(127);
			}
			if (pid > 0) {
				close(pipefd[0]);
				dprintf(pipefd[1], "%s", batch);
				close(pipefd[1]);
				if (waitpid(pid, &status, 0) >= 0 &&
				    WIFEXITED(status) && WEXITSTATUS(status) == 0)
					applied = 1;
			} else {
				close(pipefd[0]);
				close(pipefd[1]);
			}
		}
	}
	if (!applied) {
		/* fallback: individual ops */
		snprintf(setl, sizeof(setl), "del network.%s.ipaddr", net);
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl), "network.%s.ipaddr", net);
		if (mesh_uci_set(setl, newip) == 0)
			applied = 1;
		snprintf(setl, sizeof(setl), "network.%s.netmask", net);
		mesh_uci_set(setl, "255.255.255.0");
		snprintf(setl, sizeof(setl), "network.%s.ip6assign", net);
		mesh_uci_set(setl, "64");
		mesh_uci_set("network.wan6.reqprefix", "auto");
		mesh_uci_set("network.wan6.reqaddress", "try");
	}

	snprintf(setl, sizeof(setl), "network.%s.ipaddr", net);
	if (mesh_uci_get(setl, cur, sizeof(cur)) == 0)
		applied = (cur[0] && strncmp(cur, newip, strlen(newip)) == 0) ? 1 : 0;
	else
		applied = 0;

	mesh_log_notice(ctx,
			"portal ipv4: computed [ %s ] on network.%s applied=%s (uci now [ %s ])",
			newip, net, applied ? "yes" : "no",
			cur[0] ? cur : "-");
	return applied ? 0 : -1;
}

int mesh_sysfs_read_int(const char *path, int *out)
{
	FILE *f;
	int val;

	if (!path || !out)
		return -1;
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (fscanf(f, "%d", &val) != 1) {
		fclose(f);
		return -1;
	}
	fclose(f);
	*out = val;
	return 0;
}

int mesh_sysfs_write_int_if_changed(const char *path, int want)
{
	int cur;
	FILE *f;

	if (!path)
		return -1;
	if (mesh_sysfs_read_int(path, &cur) == 0 && cur == want)
		return 0;
	f = fopen(path, "w");
	if (!f)
		return -1;
	fprintf(f, "%d\n", want);
	fclose(f);
	return 1;
}
