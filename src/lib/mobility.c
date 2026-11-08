/**
 * @file mobility.c
 * @brief Map mobility levels 0-5 to the v8 rootmode policy.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#include "native.h"
#include <string.h>

/* Closed v8 item 15 (Rob, 2026-11-05): per-node relative mobility, root 4
 * for portals/moving peers and 0 for static peers; NEVER change beacon/DTIM.
 * Remaining mesh timings retain the reference values pending test-mesh tuning.
 * Legacy level 6 voice/PA is not a mobility level in v8. */
enum mesh_error mesh_mobility_read(unsigned int level, bool portal,
								  bool beacon_workaround, struct mesh_mobility *out)
{
	static const struct mesh_mobility profiles[] = {
		{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, false },
		{ 1, 0, 0, 4, -68, 2000, 500, 1500, 16, 4, true },
		{ 2, 0, 0, 4, -72, 1500, 400, 1100, 32, 3, true },
		{ 3, 0, 0, 4, -75, 1000, 350, 700, 64, 2, true },
		{ 4, 0, 0, 4, -78, 500, 300, 450, 96, 1, true },
		{ 5, 0, 0, 4, -81, 300, 250, 400, 96, 1, true },
	};
	struct mesh_mobility result;

	if (out == NULL || level >= sizeof(profiles) / sizeof(profiles[0])) {
		return MESH_E_INPUT;
	}
	result = profiles[level];
	if (portal) {
		result.rootmode = 4;
	}
	/* Kept in the input ABI for this scratch milestone, not applied policy. */
	(void)beacon_workaround;
	*out = result;
	return MESH_OK;
}
