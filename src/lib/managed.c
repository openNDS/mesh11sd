/* SPDX-License-Identifier: GPL-2.0-or-later
 * Keeping LuCI usable (docs/LUCI-DESIGN.md, decisions 1 to 4).
 *
 * With auto_config=1 and portal_detect 0 or 1, the portal configuration
 * is committed before portal detection runs, and a copy of each
 * committed file is kept as /etc/config/<pkg>_mesh11sd_portal. A peer
 * is an uncommitted overlay on top of it.
 *
 * Each loop pass then:
 *  - rebuilds and commits the portal configuration if the mesh11sd
 *    config itself changed, or a copy is missing (decisions 3 and 4);
 *  - compares the options mesh11sd manages in the committed files with
 *    the copies and puts back any that were changed by LuCI, uci or a
 *    direct edit (decisions 1 and 2);
 *  - on a peer, re-applies the peer overlay if it has gone.
 *
 * Committed values are read from a renamed copy of the file, because
 * uci always applies the uncommitted changes in /tmp/.uci to a package
 * of the same name. Reverts are made in that copy and moved back into
 * /etc/config, so the peer overlay is never committed.
 */
#include "mesh11sd.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PORTAL_SUFFIX	"_mesh11sd_portal"
#define WORKDIR		MESH11SD_TMPDIR "/managed"
#define WORK_DELTA	WORKDIR "/delta"
#define SETTLE_SECS	120	/* leave a freshly changed file alone */
#define RPCD_SNAPSHOT	"/var/run/rpcd/snapshot-files"
#define RPCD_PENDING	300	/* LuCI rollback window plus margin */
#define MAX_ENTRIES	600
#define MAX_BACKOFF	64
#define SHOW_LEN	32768

struct uci_entry {
	char sect[64];
	char opt[64];		/* empty: section type line */
	char val[256];		/* as printed by uci show (quoted) */
};

/* Static to keep large tables off the stack. */
static struct uci_entry portal_tab[MAX_ENTRIES];
static struct uci_entry live_tab[MAX_ENTRIES];
static char show_buf[SHOW_LEN];

/* Options reverted once already this run; a second drift only warns. */
static char reverted[MAX_BACKOFF][160];
static int reverted_count;
static char warned[MAX_BACKOFF][160];
static int warned_count;

static int portal_ready;

static const char *const MANAGED_PKGS[] = {
	"wireless", "network", "dhcp", "firewall", NULL
};

int mesh_managed_mode(struct mesh_ctx *ctx)
{
	if (ctx->auto_config != 1 || ctx->commit_all)
		return 0;
	if (access(MESH11SD_TMPDIR "/auto_config_test", F_OK) == 0)
		return 0;
	return ctx->portal_detect == 0 || ctx->portal_detect == 1;
}

/* ---- small file helpers ---- */

static int copy_file(const char *src, const char *dst)
{
	char buf[4096];
	struct stat st;
	FILE *in, *out;
	size_t n;
	int rc = 0;

	in = fopen(src, "r");
	if (!in)
		return -1;
	out = fopen(dst, "w");
	if (!out) {
		fclose(in);
		return -1;
	}
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		if (fwrite(buf, 1, n, out) != n) {
			rc = -1;
			break;
		}
	}
	if (ferror(in))
		rc = -1;
	fclose(in);
	if (fclose(out) != 0)
		rc = -1;
	if (rc == 0 && stat(src, &st) == 0)
		chmod(dst, st.st_mode & 0777);
	return rc;
}

/* 1 if both files exist and have the same contents */
static int files_equal(const char *a, const char *b)
{
	char ba[4096], bb[4096];
	FILE *fa, *fb;
	size_t na, nb;
	int same = 1;

	fa = fopen(a, "r");
	if (!fa)
		return 0;
	fb = fopen(b, "r");
	if (!fb) {
		fclose(fa);
		return 0;
	}
	do {
		na = fread(ba, 1, sizeof(ba), fa);
		nb = fread(bb, 1, sizeof(bb), fb);
		if (na != nb || memcmp(ba, bb, na) != 0) {
			same = 0;
			break;
		}
	} while (na > 0);
	fclose(fa);
	fclose(fb);
	return same;
}

static long file_age(const char *path)
{
	struct stat st;

	if (stat(path, &st) != 0)
		return -1;
	return (long)(time(NULL) - st.st_mtime);
}

static void run_quiet(char *const argv[])
{
	mesh_run_capture(argv, NULL, 0);
}

/* ---- LuCI apply/rollback detection ----
 * rpcd copies each file it is about to commit into
 * /var/run/rpcd/snapshot-files and clears the directory on confirm or
 * rollback. An apply without rollback leaves the files behind, so only
 * recent files count as pending. */
static int luci_apply_pending(void)
{
	DIR *d = opendir(RPCD_SNAPSHOT);
	struct dirent *e;
	int pending = 0;

	if (!d)
		return 0;
	while ((e = readdir(d)) != NULL) {
		char path[320];
		long age;

		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", RPCD_SNAPSHOT, e->d_name);
		age = file_age(path);
		if (age >= 0 && age < RPCD_PENDING) {
			pending = 1;
			break;
		}
	}
	closedir(d);
	return pending;
}

/* ---- portal copies ---- */

static void portal_copy_path(const char *pkg, char *out, size_t len)
{
	snprintf(out, len, "/etc/config/%s" PORTAL_SUFFIX, pkg);
}

int mesh_write_portal_copies(struct mesh_ctx *ctx, const char *const *pkgs)
{
	int i, rc = 0;

	for (i = 0; pkgs[i]; i++) {
		char src[96], dst[128];

		snprintf(src, sizeof(src), "/etc/config/%s", pkgs[i]);
		portal_copy_path(pkgs[i], dst, sizeof(dst));
		if (access(src, R_OK) != 0)
			continue;
		if (copy_file(src, dst) != 0) {
			mesh_log_err(ctx, "Unable to write %s", dst);
			rc = -1;
		}
	}
	return rc;
}

static int portal_copies_present(void)
{
	int i;
	char path[128];

	portal_copy_path("mesh11sd", path, sizeof(path));
	if (access(path, R_OK) != 0)
		return 0;
	for (i = 0; MANAGED_PKGS[i]; i++) {
		char live[96];

		snprintf(live, sizeof(live), "/etc/config/%s", MANAGED_PKGS[i]);
		portal_copy_path(MANAGED_PKGS[i], path, sizeof(path));
		if (access(live, R_OK) == 0 && access(path, R_OK) != 0)
			return 0;
	}
	return 1;
}

/* Next line that is not last_role (mesh11sd's own role note). */
static char *next_config_line(char *buf, int len, FILE *f)
{
	while (fgets(buf, len, f))
		if (!strstr(buf, "option last_role"))
			return buf;
	return NULL;
}

static int mesh11sd_config_changed(void)
{
	char path[128], la[512], lb[512];
	FILE *fa, *fb;
	char *a, *b;
	int changed = 0;

	portal_copy_path("mesh11sd", path, sizeof(path));
	fa = fopen("/etc/config/mesh11sd", "r");
	fb = fopen(path, "r");
	if (!fa || !fb) {
		if (fa)
			fclose(fa);
		if (fb)
			fclose(fb);
		return 1;
	}
	do {
		a = next_config_line(la, sizeof(la), fa);
		b = next_config_line(lb, sizeof(lb), fb);
		if (!a != !b || (a && strcmp(a, b))) {
			changed = 1;
			break;
		}
	} while (a);
	fclose(fa);
	fclose(fb);
	return changed;
}

/* ---- which options mesh11sd owns (docs/UCI-OWNERSHIP.md) ---- */

static int in_list(const char *opt, const char *const *list)
{
	int i;

	for (i = 0; list[i]; i++)
		if (!strcmp(opt, list[i]))
			return 1;
	return 0;
}

static int is_managed(struct mesh_ctx *ctx, const char *pkg, const char *sect,
		      const char *type, const char *mode, const char *opt)
{
	static const char *const radio[] = {
		"disabled", "country", "channel", "htmode", "noscan",
		"txpower", "beacon_int", NULL
	};
	static const char *const mesh_if[] = {
		"device", "mode", "network", "ifname", "mesh_id", "key",
		"encryption", "disabled", "mesh_rssi_threshold",
		"dtim_period", NULL
	};
	static const char *const ap_if[] = {
		"disabled", "ssid", "encryption", "key", NULL
	};
	static const char *const lan_if[] = {
		"ipaddr", "netmask", "proto", "ip6assign", "ip6hint", NULL
	};
	static const char *const lan_dhcp[] = {
		"dhcpv4", "dhcpv6", "ra", "ignore", "force", "ra_default",
		"ra_flags", NULL
	};
	static const char *const ra_timers[] = {
		"ra_lifetime", "ra_mininterval", "ra_maxinterval", NULL
	};
	static const char *const dnsmasq[] = {
		"authoritative", "rebind_protection", NULL
	};
	const char *net = ctx->auto_mesh_network;

	/* MAC addresses come from the v7 MAC walk and fixup on every
	 * start and are never committed, so never compare or restore them. */
	if (!strcmp(opt, "macaddr"))
		return 0;

	if (!strcmp(pkg, "wireless")) {
		if (!strcmp(type, "wifi-device"))
			return in_list(opt, radio);
		if (strcmp(type, "wifi-iface") != 0)
			return 0;
		if (!strcmp(mode, "mesh"))
			return in_list(opt, mesh_if);
		if (!strncmp(sect, "vxradio", 7))
			return 1;
		if (!strcmp(mode, "ap"))
			return in_list(opt, ap_if);
		return 0;
	}
	if (!strcmp(pkg, "network")) {
		if (!strcmp(sect, net))
			return in_list(opt, lan_if);
		if (!strcmp(sect, "globals"))
			return !strcmp(opt, "ula_prefix");
		return !strcmp(sect, "vtunnel") || !strcmp(sect, "vtunlan");
	}
	if (!strcmp(pkg, "dhcp")) {
		if (!strcmp(sect, "odhcpd"))
			return !strcmp(opt, "loglevel");
		if (!strcmp(type, "dnsmasq"))
			return in_list(opt, dnsmasq);
		if (!strcmp(sect, "vtunlan"))
			return 1;
		if (!strcmp(sect, net) && in_list(opt, lan_dhcp))
			return 1;
		return !strcmp(sect, "lan") && in_list(opt, ra_timers);
	}
	if (!strcmp(pkg, "firewall")) {
		return !strcmp(sect, "vtunlan") || !strcmp(sect, "vxlan") ||
		       !strcmp(sect, "vxlan_fwd") || !strcmp(sect, "lan_tun") ||
		       !strncmp(sect, "dhcptun", 7) || !strncmp(sect, "dnstun", 6);
	}
	return 0;
}

/* Never log the value of a key or password. */
static int is_secret(const char *opt)
{
	return strstr(opt, "key") != NULL || strstr(opt, "password") != NULL;
}

/* ---- reading uci show output ---- */

/* Parse "name.sect=type" and "name.sect.opt='val'" lines. */
static int parse_show(char *text, struct uci_entry *tab)
{
	char *p = text, *line;
	int n = 0;

	while ((line = strsep(&p, "\n")) != NULL) {
		char *dot, *eq, *odot;

		if (!line[0] || n >= MAX_ENTRIES)
			continue;
		dot = strchr(line, '.');
		eq = strchr(line, '=');
		if (!dot || !eq || eq < dot)
			continue;
		*eq = '\0';
		odot = strchr(dot + 1, '.');
		if (odot) {
			*odot = '\0';
			snprintf(tab[n].opt, sizeof(tab[n].opt), "%s", odot + 1);
		} else {
			tab[n].opt[0] = '\0';
		}
		snprintf(tab[n].sect, sizeof(tab[n].sect), "%s", dot + 1);
		snprintf(tab[n].val, sizeof(tab[n].val), "%s", eq + 1);
		n++;
	}
	return n;
}

/* uci show <name> from confdir, without uncommitted changes as long as
 * no delta of that name exists. Returns entry count or -1. */
static int load_show(const char *confdir, const char *name,
		     struct uci_entry *tab)
{
	char *argv[] = { "uci", "-q", "-c", (char *)confdir, "show",
			 (char *)name, NULL };

	if (mesh_run_capture(argv, show_buf, sizeof(show_buf)) != 0)
		return -1;
	if (strlen(show_buf) >= sizeof(show_buf) - 1)
		return -1;	/* truncated: do not act on half a file */
	return parse_show(show_buf, tab);
}

static const struct uci_entry *find(const struct uci_entry *tab, int n,
				    const char *sect, const char *opt)
{
	int i;

	for (i = 0; i < n; i++)
		if (!strcmp(tab[i].sect, sect) && !strcmp(tab[i].opt, opt))
			return &tab[i];
	return NULL;
}

/* Copy a value out of uci show quoting: 'a'\''b' -> a'b */
static void unquote(const char *in, char *out, size_t len)
{
	size_t o = 0;
	int quoted = 0;

	for (; *in && o + 1 < len; in++) {
		if (*in == '\'') {
			quoted = !quoted;
			continue;
		}
		if (!quoted && *in == '\\' && in[1]) {
			in++;
			out[o++] = *in;
			continue;
		}
		out[o++] = *in;
	}
	out[o] = '\0';
}

/* Next list item from a uci show list value ('a' 'b'). */
static const char *next_item(const char *p, char *item, size_t len)
{
	char raw[256];
	size_t r = 0;
	int quoted = 0;

	while (*p == ' ')
		p++;
	if (!*p)
		return NULL;
	for (; *p && (quoted || *p != ' '); p++) {
		if (*p == '\'')
			quoted = !quoted;
		if (r + 1 < sizeof(raw))
			raw[r++] = *p;
	}
	raw[r] = '\0';
	unquote(raw, item, len);
	return p;
}

static int is_list_value(const char *val)
{
	char item[256];
	const char *p = next_item(val, item, sizeof(item));

	return p && next_item(p, item, sizeof(item)) != NULL;
}

/* ---- back off ---- */

static int key_in(char keys[][160], int count, const char *key)
{
	int i;

	for (i = 0; i < count; i++)
		if (!strcmp(keys[i], key))
			return 1;
	return 0;
}

static void key_add(char keys[][160], int *count, const char *key)
{
	if (*count < MAX_BACKOFF && !key_in(keys, *count, key))
		snprintf(keys[(*count)++], 160, "%s", key);
}

static void backoff_reset(void)
{
	reverted_count = 0;
	warned_count = 0;
}

/* ---- the drift check for one package ---- */

static int uci_work(char *a1, char *a2)
{
	char *argv[] = { "uci", "-q", "-c", WORKDIR, "-t", WORK_DELTA,
			 a1, a2, NULL };

	return mesh_run_capture(argv, NULL, 0);
}

static void restore_option(const char *work, const struct uci_entry *e)
{
	char tuple[192], arg[512], item[256];
	const char *p;

	snprintf(tuple, sizeof(tuple), "%s.%s.%s", work, e->sect, e->opt);
	if (!is_list_value(e->val)) {
		unquote(e->val, item, sizeof(item));
		snprintf(arg, sizeof(arg), "%s=%s", tuple, item);
		uci_work("set", arg);
		return;
	}
	uci_work("delete", tuple);
	for (p = e->val; (p = next_item(p, item, sizeof(item))) != NULL;) {
		snprintf(arg, sizeof(arg), "%s=%s", tuple, item);
		uci_work("add_list", arg);
	}
}

static void reload_package(struct mesh_ctx *ctx, const char *pkg)
{
	mesh_log_info(ctx, "Reloading %s after restoring managed options", pkg);
	if (!strcmp(pkg, "wireless")) {
		char *argv[] = { "/sbin/wifi", "reload", NULL };
		run_quiet(argv);
	} else if (!strcmp(pkg, "network")) {
		char *argv[] = { "/sbin/service", "network", "reload", NULL };
		run_quiet(argv);
		mesh_network_mark_applied();
	} else if (!strcmp(pkg, "dhcp")) {
		char *dns[] = { "/sbin/service", "dnsmasq", "reload", NULL };
		char *odhcpd[] = { "/sbin/service", "odhcpd", "reload", NULL };
		run_quiet(dns);
		run_quiet(odhcpd);
	} else if (!strcmp(pkg, "firewall")) {
		char *argv[] = { "/sbin/service", "firewall", "reload", NULL };
		run_quiet(argv);
	}
}

/* Returns 1 if the package was changed and reloaded. */
static int check_package(struct mesh_ctx *ctx, const char *pkg)
{
	char live[96], copy[128], work[64], workpath[160], tmp[128];
	int np, nl, i, restored = 0;

	snprintf(live, sizeof(live), "/etc/config/%s", pkg);
	portal_copy_path(pkg, copy, sizeof(copy));
	if (access(copy, R_OK) != 0 || access(live, R_OK) != 0)
		return 0;
	if (files_equal(live, copy))
		return 0;
	if (file_age(live) < SETTLE_SECS) {
		mesh_log_debug(ctx, "%s changed recently - checking later", live);
		return 0;
	}

	snprintf(work, sizeof(work), "live_%s", pkg);
	snprintf(workpath, sizeof(workpath), WORKDIR "/%s", work);
	mesh_mkdir_p(WORK_DELTA);
	if (copy_file(live, workpath) != 0)
		return 0;

	snprintf(tmp, sizeof(tmp), "%s" PORTAL_SUFFIX, pkg);
	np = load_show("/etc/config", tmp, portal_tab);
	nl = load_show(WORKDIR, work, live_tab);
	if (np < 0 || nl < 0) {
		mesh_log_warn(ctx, "Unable to read %s or its portal copy", live);
		return 0;
	}

	for (i = 0; i < np; i++) {
		const struct uci_entry *e = &portal_tab[i];
		const struct uci_entry *type, *mode, *now, *live_type;
		char key[160], modeval[32] = "";

		if (!e->opt[0])
			continue;
		type = find(portal_tab, np, e->sect, "");
		mode = find(portal_tab, np, e->sect, "mode");
		if (mode)
			unquote(mode->val, modeval, sizeof(modeval));
		if (!is_managed(ctx, pkg, e->sect, type ? type->val : "",
				modeval, e->opt))
			continue;

		/* The section must still exist as the same kind of section.
		 * If not, an anonymous index has probably shifted: leave it. */
		live_type = find(live_tab, nl, e->sect, "");
		if (!live_type || !type || strcmp(live_type->val, type->val)) {
			snprintf(key, sizeof(key), "%s.%s", pkg, e->sect);
			if (!key_in(warned, warned_count, key)) {
				mesh_log_warn(ctx, "Managed section %s is missing or changed - not restoring it", key);
				key_add(warned, &warned_count, key);
			}
			continue;
		}
		if (e->sect[0] == '@' && mode) {
			const struct uci_entry *lmode =
				find(live_tab, nl, e->sect, "mode");
			if (!lmode || strcmp(lmode->val, mode->val))
				continue;
		}

		now = find(live_tab, nl, e->sect, e->opt);
		if (now && !strcmp(now->val, e->val))
			continue;

		snprintf(key, sizeof(key), "%s.%s.%s", pkg, e->sect, e->opt);
		if (key_in(reverted, reverted_count, key)) {
			if (!key_in(warned, warned_count, key)) {
				mesh_log_warn(ctx, "%s was changed again outside mesh11sd - not restoring it again. Set it in the mesh11sd config instead", key);
				key_add(warned, &warned_count, key);
			}
			continue;
		}

		restore_option(work, e);
		key_add(reverted, &reverted_count, key);
		restored++;
		if (is_secret(e->opt)) {
			mesh_log_notice(ctx, "%s restored", key);
		} else {
			char val[256];

			unquote(e->val, val, sizeof(val));
			mesh_log_notice(ctx, "%s restored to [ %s ]", key, val);
		}
	}

	if (!restored)
		return 0;

	if (uci_work("commit", work) != 0) {
		mesh_log_err(ctx, "Unable to restore managed options in %s", live);
		return 0;
	}
	snprintf(tmp, sizeof(tmp), "/etc/config/.%s.mesh11sd", pkg);
	if (copy_file(workpath, tmp) != 0 || rename(tmp, live) != 0) {
		unlink(tmp);
		mesh_log_err(ctx, "Unable to write %s", live);
		return 0;
	}
	reload_package(ctx, pkg);
	return 1;
}

/* ---- MAC addresses set by mesh11sd (v7 clean sheet) ----
 *
 * v7 numbers the AP and bridge MACs afresh on every start (MAC walk
 * and fixup) and never commits them with auto_config=1. A committed
 * one would stop the fixup and fight the walk, so they are kept out
 * of the committed files and the portal copies.
 *
 * A MAC is mesh11sd's own if it is the factory MAC (mesh vif) or the
 * locally administered factory MAC with any 4th octet (indexed MAC).
 */
static int mesh_is_own_mac(struct mesh_ctx *ctx, const char *mac)
{
	unsigned m[6], f[6];
	int i;

	if (!ctx->factory_mac[0])
		mesh_get_factory_mac(ctx, ctx->factory_mac, sizeof(ctx->factory_mac));
	if (!ctx->factory_mac[0])
		return 0;
	if (sscanf(mac, "%x:%x:%x:%x:%x:%x",
		   &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6)
		return 0;
	if (sscanf(ctx->factory_mac, "%x:%x:%x:%x:%x:%x",
		   &f[0], &f[1], &f[2], &f[3], &f[4], &f[5]) != 6)
		return 0;
	for (i = 0; i < 6 && m[i] == f[i]; i++)
		;
	if (i == 6)
		return 1;	/* factory MAC */
	f[0] |= 0x02;
	return m[0] == f[0] && m[1] == f[1] && m[2] == f[2] &&
	       m[4] == f[4] && m[5] == f[5];
}

/* Find "<conf>.<sect>.macaddr" options holding one of our MACs. */
int mesh_own_macs_find(struct mesh_ctx *ctx, const char *conf,
		       struct mesh_own_mac *tab, int max)
{
	char *argv[] = { "uci", "-q", "show", (char *)conf, NULL };
	char *p, *line;
	int n = 0;

	if (mesh_run_capture(argv, show_buf, sizeof(show_buf)) != 0)
		return 0;
	p = show_buf;
	while ((line = strsep(&p, "\n")) != NULL && n < max) {
		char *sect, *opt, *eq, val[64];
		size_t clen = strlen(conf);

		if (strncmp(line, conf, clen) || line[clen] != '.')
			continue;
		sect = line + clen + 1;
		eq = strchr(sect, '=');
		opt = strchr(sect, '.');
		if (!eq || !opt || opt > eq)
			continue;
		if ((size_t)(eq - opt - 1) != strlen("macaddr") ||
		    strncmp(opt + 1, "macaddr", 7))
			continue;
		unquote(eq + 1, val, sizeof(val));
		if (!mesh_is_own_mac(ctx, val))
			continue;
		*opt = '\0';
		snprintf(tab[n].sect, sizeof(tab[n].sect), "%s", sect);
		snprintf(tab[n].val, sizeof(tab[n].val), "%s", val);
		n++;
	}
	return n;
}

/* Delete the options from the committed file. Only call this when the
 * package has no other uncommitted changes. */
void mesh_own_macs_strip(const char *conf, const struct mesh_own_mac *tab, int n)
{
	char line[192];
	int i;

	if (n <= 0)
		return;
	for (i = 0; i < n; i++) {
		snprintf(line, sizeof(line), "delete %s.%s.macaddr", conf, tab[i].sect);
		mesh_uci_batch_line(line);
	}
	mesh_uci_commit(conf);
}

/* Put the options back as uncommitted changes. */
void mesh_own_macs_set(const char *conf, const struct mesh_own_mac *tab, int n)
{
	char line[192];
	int i;

	for (i = 0; i < n; i++) {
		snprintf(line, sizeof(line), "set %s.%s.macaddr='%s'",
			 conf, tab[i].sect, tab[i].val);
		mesh_uci_batch_line(line);
	}
}

/* Daemon startup, after the uncommitted changes have been reverted:
 * remove our MACs from the committed files and the portal copies, so
 * the walk and the fixup start from a clean sheet as in v7. */
void mesh_clear_own_macaddrs(struct mesh_ctx *ctx)
{
	static const char *const confs[] = {
		"wireless", "network",
		"wireless" PORTAL_SUFFIX, "network" PORTAL_SUFFIX, NULL
	};
	struct mesh_own_mac tab[MESH_OWN_MACS_MAX];
	int i, n;

	if (ctx->auto_config != 1 || ctx->commit_all)
		return;
	for (i = 0; confs[i]; i++) {
		char path[128];

		snprintf(path, sizeof(path), "/etc/config/%s", confs[i]);
		if (access(path, R_OK) != 0)
			continue;
		n = mesh_own_macs_find(ctx, confs[i], tab, MESH_OWN_MACS_MAX);
		if (n <= 0)
			continue;
		mesh_own_macs_strip(confs[i], tab, n);
		mesh_log_notice(ctx, "Removed %d committed mesh11sd MAC address(es) from %s", n, path);
	}
}

/* ---- portal base settings, also used at daemon startup ---- */

/* v7 autostartup: dhcp and network role settings for the configured
 * portal_detect mode, applied as uncommitted changes. */
void mesh_apply_autostartup_uci(struct mesh_ctx *ctx)
{
	char setl[192];

	if (ctx->portal_detect == 0 || ctx->portal_detect == 4)
		mesh_set_ula_prefix(ctx, "set");
	if (ctx->portal_detect == 5) {
		snprintf(setl, sizeof(setl),
			 "set dhcp.%s.dhcpv4='disabled'", ctx->auto_mesh_network);
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl),
			 "set dhcp.%s.dhcpv6='disabled'", ctx->auto_mesh_network);
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl),
			 "set dhcp.%s.ra='disabled'", ctx->auto_mesh_network);
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl),
			 "set dhcp.%s.ignore='1'", ctx->auto_mesh_network);
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl),
			 "set network.%s.proto='dhcp'", ctx->auto_mesh_network);
		mesh_uci_batch_line(setl);
	} else if (ctx->portal_detect != 3 && ctx->portal_detect != 4) {
		snprintf(setl, sizeof(setl),
			 "set dhcp.%s.dhcpv4='server'", ctx->auto_mesh_network);
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl),
			 "set dhcp.%s.dhcpv6='server'", ctx->auto_mesh_network);
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl),
			 "set dhcp.%s.ra='server'", ctx->auto_mesh_network);
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl),
			 "set dhcp.%s.ignore='0'", ctx->auto_mesh_network);
		mesh_uci_batch_line(setl);
		snprintf(setl, sizeof(setl),
			 "set network.%s.proto='static'", ctx->auto_mesh_network);
		mesh_uci_batch_line(setl);
	}
	/* v7 ~10925: deterministic lan IPv4 when portal_use_default_ipv4=0.
	 * All nodes calculate it; portals (proto=static) apply/use it. */
	if (ctx->portal_detect != 5)
		mesh_apply_portal_ipv4(ctx);
	if (ctx->portal_detect != 3) {
		snprintf(setl, sizeof(setl),
			 "set network.%s.ip6assign='64'", ctx->auto_mesh_network);
		mesh_uci_batch_line(setl);
	}
	mesh_uci_batch_line("set dhcp.lan.ra_lifetime='3600'");
	mesh_uci_batch_line("set dhcp.lan.ra_mininterval='20'");
	mesh_uci_batch_line("set dhcp.lan.ra_maxinterval='60'");
	/* CPE: br-wan, wan dhcp, lan server, NAT/NAT66 (mode 3 only) */
	mesh_cpe_stage_network(ctx);
}

/* ---- portal configuration build (decisions 3 and 4) ---- */

static void rebuild_portal(struct mesh_ctx *ctx, const char *reason,
			   int in_loop, int auto_config_done)
{
	int i;

	mesh_log_notice(ctx, "Portal configuration: %s - building and committing it", reason);

	if (in_loop) {
		mesh_ctx_load_setup(ctx);
		if (!mesh_managed_mode(ctx)) {
			const char *const mine[] = { "mesh11sd", NULL };

			mesh_log_notice(ctx, "auto_config or portal_detect changed - restart mesh11sd to apply");
			mesh_write_portal_copies(ctx, mine);
			return;
		}
		/* Drop every uncommitted overlay (peer or portal runtime),
		 * then rebuild the portal base from the mesh11sd config. */
		for (i = 0; MANAGED_PKGS[i]; i++)
			mesh_uci_revert(MANAGED_PKGS[i]);
		mesh_uci_revert("system");
		mesh_apply_autostartup_uci(ctx);
	}
	if (!auto_config_done && mesh_auto_config(ctx) != 0) {
		mesh_log_err(ctx, "Portal configuration not committed: auto config failed");
		return;
	}
	mesh_commit_portal_config(ctx);
	backoff_reset();

	if (!in_loop && auto_config_done)
		return;		/* the caller has just reloaded */

	if (in_loop && !strcmp(ctx->detected_state, "peer")) {
		mesh_peer_reapply(ctx);
	} else if (in_loop && !strcmp(ctx->detected_state, "portal")) {
		mesh_portal_reapply(ctx);
	} else {
		char *nr[] = { "/sbin/service", "network", "reload", NULL };
		char *dns[] = { "/sbin/service", "dnsmasq", "restart", NULL };
		char *fw[] = { "/sbin/service", "firewall", "reload", NULL };

		run_quiet(nr);
		mesh_network_mark_applied();
		run_quiet(dns);
		run_quiet(fw);
	}
}

/* On a peer the role settings are uncommitted. If an apply, a revert or
 * a commit dropped them, put them back. */
static void check_peer_overlay(struct mesh_ctx *ctx)
{
	char key[96], proto[32] = "";

	if (strcmp(ctx->detected_state, "peer") || strcmp(ctx->last_state, "peer"))
		return;
	snprintf(key, sizeof(key), "network.%s.proto", ctx->auto_mesh_network);
	mesh_uci_get(key, proto, sizeof(proto));
	if (!strcmp(proto, "dhcp"))
		return;
	mesh_log_notice(ctx, "Peer settings have gone (proto [ %s ]) - re-applying them", proto);
	mesh_peer_reapply(ctx);
}

/* Called every loop pass, before portal detection, when
 * mesh_managed_mode() is true. auto_config_done: mesh_auto_config ran
 * (and the network was reloaded) earlier in this pass. */
int mesh_managed_pass(struct mesh_ctx *ctx, int auto_config_done)
{
	int i, settled;

	mesh_mkdir_p(WORKDIR);

	if (!portal_ready) {
		portal_ready = 1;
		if (!portal_copies_present()) {
			rebuild_portal(ctx, "no portal copy found (new or upgraded node)",
				       0, auto_config_done);
			return 0;
		}
		if (mesh11sd_config_changed()) {
			rebuild_portal(ctx, "mesh11sd config changed", 0,
				       auto_config_done);
			return 0;
		}
		/* fall through: restore anything changed while stopped */
	} else {
		if (!portal_copies_present()) {
			rebuild_portal(ctx, "portal copy missing or unreadable", 1, 0);
			return 0;
		}
		settled = file_age("/etc/config/mesh11sd") >= SETTLE_SECS;
		if (mesh11sd_config_changed() && settled && !luci_apply_pending()) {
			rebuild_portal(ctx, "mesh11sd config changed", 1, 0);
			return 0;
		}
	}

	if (luci_apply_pending()) {
		mesh_log_debug(ctx, "LuCI apply pending - skipping managed option check");
		return 0;
	}
	for (i = 0; MANAGED_PKGS[i]; i++)
		check_package(ctx, MANAGED_PKGS[i]);
	check_peer_overlay(ctx);
	return 0;
}
