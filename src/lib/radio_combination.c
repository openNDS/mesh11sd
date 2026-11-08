/**
 * @file radio_combination.c
 * @brief Prove a desired AP plus one mesh fits the radio limits.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#include "platform.h"
#include <linux/nl80211.h>

static uint32_t gcd(uint32_t a, uint32_t b)
{
	uint32_t next;
	while (b != 0) {
		next = a % b;
		a = b;
		b = next;
	}
	return a;
}

bool mesh_combination_allows(const struct mesh_radio *radio, unsigned int aps,
							 const uint32_t *beacons, size_t beacon_count)
{
	const struct mesh_combination *combination;
	const struct mesh_iflimit *limit;
	uint64_t covered;
	uint32_t count;
	uint32_t common;
	size_t i;
	size_t j;
	bool valid;

	if (radio == NULL || !radio->mesh_supported || aps > 16 || beacons == NULL ||
		beacon_count != (size_t)aps + 1 || radio->combination_count > MESH_COMBINATIONS_MAX) {
		return false;
	}
	common = beacons[0];
	if (common == 0) {
		return false;
	}
	for (i = 1; i < beacon_count; i++) {
		if (beacons[i] == 0) {
			return false;
		}
		common = gcd(common, beacons[i]);
	}
	if (aps == 0) {
		return true;
	}
	for (i = 0; i < radio->combination_count; i++) {
		combination = &radio->combinations[i];
		if (combination->maximum < aps + 1 || combination->channels < 1 ||
			combination->limit_count == 0 || combination->limit_count > MESH_LIMITS_MAX ||
			(combination->beacon_min_gcd != 0 && common < combination->beacon_min_gcd)) {
			continue;
		}
		/* STA_AP_BI_MATCH applies to station+AP, not our AP+mesh-only request.
		 * The kernel's BI_MIN_GCD applies to all beaconing interfaces. */
		covered = 0;
		valid = true;
		for (j = 0; j < combination->limit_count; j++) {
			limit = &combination->limits[j];
			count = 0;
			if (limit->types & (UINT64_C(1) << NL80211_IFTYPE_AP)) {
				count += aps;
			}
			if (limit->types & (UINT64_C(1) << NL80211_IFTYPE_MESH_POINT)) {
				count++;
			}
			if (count > limit->maximum || (covered & limit->types) != 0) {
				valid = false;
				break;
			}
			covered |= limit->types;
		}
		if (valid && (covered & (UINT64_C(1) << NL80211_IFTYPE_AP)) &&
			(covered & (UINT64_C(1) << NL80211_IFTYPE_MESH_POINT))) {
			return true;
		}
	}
	return false;
}
