/**
 * @file airtime.h
 * @brief Airtime sample and metric types used by portal selection.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#ifndef MESH_AIRTIME_H
#define MESH_AIRTIME_H
#include "mesh_config.h"

/* Rates use nl80211's 100 kbit/s units, never expected-throughput or kernel
 * airtime metrics. Counters are samples, not lifetime loss percentages. */
struct mesh_station_sample {
	uint8_t mac[6];
	uint32_t ifindex;
	uint32_t tx_rate;
	uint32_t rx_rate;
	uint32_t tx_packets;
	uint32_t tx_failed;
	uint32_t tx_retries;
	int signal_average;
	bool established;
	bool have_tx_rate;
	bool have_rx_rate;
	bool have_counters;
	bool have_retries;
	bool have_signal;
};
struct mesh_path_sample {
	uint8_t destination[6];
	uint8_t next_hop[6];
	uint32_t ifindex;
	uint32_t changes;
	uint8_t hops;
	bool active;
};
struct mesh_airtime_window {
	struct mesh_station_sample previous;
	uint64_t sampled_ms;
	bool initialized;
};
struct mesh_airtime_result {
	uint64_t next_hop_metric;
	uint64_t adjusted_metric;
	uint32_t packet_delta;
	uint32_t failed_delta;
	uint32_t retry_delta;
	uint8_t hops;
	bool have_retry_delta;
	unsigned int fail_percent;
	unsigned int chosen_mbps;
	bool used_rx_fallback;
	bool used_conservative_rate;
};
/* Returns 0 valid metric, 1 baseline/reset/stale interval, -1 invalid evidence.
 * RSSI affects only the fallback-rate workaround, NEVER live peer teardown. */
int mesh_calc_airtime_metric(struct mesh_airtime_window *window,
							 const struct mesh_station_sample *sample,
							 uint8_t hops, bool threshold_set, int threshold,
							 uint64_t now_ms, struct mesh_airtime_result *result);
int mesh_decode_station(const void *data, size_t length, struct mesh_station_sample *out);
int mesh_decode_path(const void *data, size_t length, struct mesh_path_sample *out);
int mesh_probe_path_station(uint32_t ifindex, const uint8_t destination[6],
							struct mesh_path_sample *path, struct mesh_station_sample *station);
#endif
