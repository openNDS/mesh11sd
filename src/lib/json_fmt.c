/* SPDX-License-Identifier: GPL-2.0-or-later
 * JSON emit helpers matching Spod's apmond style (json_str / json_field / json_array).
 * Reuse for status and any other CLI JSON — do not invent a second formatter.
 */
#include "mesh11sd.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* apmond json_str: escape \ and ", strip control characters */
void mesh_json_str(const char *in, char *out, size_t outlen)
{
	size_t o = 0;

	if (!out || outlen == 0)
		return;
	out[0] = '\0';
	if (!in)
		return;
	for (; *in && o + 1 < outlen; in++) {
		unsigned char c = (unsigned char)*in;
		if (c < 0x20)
			continue;
		if (c == '\\' || c == '"') {
			if (o + 2 >= outlen)
				break;
			out[o++] = '\\';
			out[o++] = (char)c;
		} else {
			out[o++] = (char)c;
		}
	}
	out[o] = '\0';
}

void mesh_json_field(const char *indent, const char *key, const char *value, int last)
{
	char esc[512];

	mesh_json_str(value ? value : "", esc, sizeof(esc));
	if (last)
		printf("%s\"%s\": \"%s\"\n", indent, key, esc);
	else
		printf("%s\"%s\": \"%s\",\n", indent, key, esc);
}

void mesh_json_field_int(const char *indent, const char *key, long value, int last)
{
	if (last)
		printf("%s\"%s\": \"%ld\"\n", indent, key, value);
	else
		printf("%s\"%s\": \"%ld\",\n", indent, key, value);
}
