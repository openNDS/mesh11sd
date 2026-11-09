/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mesh11sd.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

int mesh_is_hex(const char *s)
{
	size_t i;

	if (!s || !s[0])
		return 0;
	for (i = 0; s[i]; i++) {
		if (!isxdigit((unsigned char)s[i]))
			return 0;
	}
	return 1;
}

int mesh_str_to_hex(const char *in, char *out, size_t outlen)
{
	size_t i, n;

	if (!in || !out || outlen == 0)
		return -1;
	n = strlen(in);
	if (outlen < n * 2 + 1)
		return -1;
	for (i = 0; i < n; i++)
		sprintf(out + i * 2, "%02x", (unsigned char)in[i]);
	out[n * 2] = '\0';
	return 0;
}

int mesh_hex_to_str(const char *in, char *out, size_t outlen)
{
	size_t i, n;
	unsigned int v;

	if (!in || !out || outlen == 0)
		return -1;
	if (!mesh_is_hex(in))
		return -1;
	n = strlen(in);
	if (n % 2)
		return -1;
	if (outlen < n / 2 + 1)
		return -1;
	for (i = 0; i < n; i += 2) {
		if (sscanf(in + i, "%2x", &v) != 1)
			return -1;
		out[i / 2] = (char)v;
	}
	out[n / 2] = '\0';
	return 0;
}

int mesh_is_ipv4addr_valid(const char *s, char *canon, size_t canonlen)
{
	unsigned a, b, c, d;
	char tmp[32];
	char junk;

	if (!s || !s[0])
		return 0;
	if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &junk) != 4)
		return 0;
	if (a > 254 || b > 254 || c > 254 || d > 254)
		return 0;
	snprintf(tmp, sizeof(tmp), "%u.%u.%u.%u", a, b, c, d);
	if (strcmp(tmp, s) != 0)
		return 0;
	if (canon && canonlen)
		snprintf(canon, canonlen, "%s", tmp);
	return 1;
}
