/**
 * @file airtime.c
 * @brief Integer airtime metric from rate, loss, and retry deltas with the hop adjustment.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#include "airtime.h"
#include <math.h>
#include <string.h>

int mesh_calc_airtime_metric(struct mesh_airtime_window *window,
							 const struct mesh_station_sample *sample,
							 uint8_t hops, bool threshold_set, int threshold,
							 uint64_t now_ms, struct mesh_airtime_result *result)
{
	struct mesh_airtime_result next = {0};
	uint64_t elapsed;
	uint64_t tx_time;
	uint64_t retransmit;
	uint64_t error;
	double adjusted;
	bool baseline;
	if (window == NULL || sample == NULL || result == NULL || !mesh_mac_valid(sample->mac) ||
		sample->ifindex == 0 || !sample->established || !sample->have_counters || hops == 0 ||
		((!sample->have_tx_rate || sample->tx_rate == 0) &&
		 (!sample->have_rx_rate || sample->rx_rate == 0)) ||
		(threshold_set && (threshold >= 0 || threshold < -127))) {
		return -1;
	}
	if (window->initialized && now_ms <= window->sampled_ms) {
		return -1;
	}
	elapsed = window->initialized ? now_ms - window->sampled_ms : 0;
	baseline = !window->initialized || window->previous.ifindex != sample->ifindex ||
			   memcmp(window->previous.mac, sample->mac, 6) != 0 || elapsed > 30000 ||
			   sample->tx_packets < window->previous.tx_packets || sample->tx_failed < window->previous.tx_failed ||
			   (sample->have_retries && window->previous.have_retries && sample->tx_retries < window->previous.tx_retries);
	if (!baseline && elapsed < 1000) {
		return 1;
	}
	if (!baseline) {
		next.packet_delta = sample->tx_packets - window->previous.tx_packets;
		next.failed_delta = sample->tx_failed - window->previous.tx_failed;
		if (sample->have_retries && window->previous.have_retries) {
			next.retry_delta = sample->tx_retries - window->previous.tx_retries;
			next.have_retry_delta = true;
		}
	}
	next.hops = hops;
	window->previous = *sample;
	window->sampled_ms = now_ms;
	window->initialized = true;
	if (baseline) {
		return 1;
	}
	/* Exact integer arithmetic from v7 src/mesh11sd:6064-6104, with the closed
	 * review's counter-window improvement and explicit strong-RX condition. */
	if (sample->have_tx_rate && sample->tx_rate >= 240) {
		next.chosen_mbps = sample->tx_rate / 10;
	} else if (sample->have_rx_rate && sample->rx_rate >= 240 && threshold_set &&
			   sample->have_signal && sample->signal_average > threshold) {
		next.chosen_mbps = sample->rx_rate / 10;
		next.used_rx_fallback = true;
	} else {
		next.chosen_mbps = 20;
		next.used_conservative_rate = true;
	}
	if (next.packet_delta > 20) {
		error = (uint64_t)next.failed_delta * 100 / next.packet_delta;
		next.fail_percent = error > 90 ? 90 : (unsigned int)error;
	}
	if (next.fail_percent > 90) {
		next.fail_percent = 90;
	}
	error = next.fail_percent * 256 / 100;
	tx_time = 256 + UINT64_C(20971520) / (next.chosen_mbps * UINT64_C(10));
	retransmit = 65536 / (256 - error);
	next.next_hop_metric = tx_time * retransmit / 65536;
	adjusted = (double)next.next_hop_metric * pow((double)hops, 1.6);
	if (!isfinite(adjusted) || adjusted < 0.5 || adjusted > (double)UINT32_MAX) {
		return -1;
	}
	next.adjusted_metric = (uint64_t)floor(adjusted + 0.5);
	*result = next;
	return 0;
}
