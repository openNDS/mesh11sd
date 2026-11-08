/**
 * @file portal_runtime.h
 * @brief Portal runtime observation, pin, and renew types.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#ifndef MESH_PORTAL_RUNTIME_H
#define MESH_PORTAL_RUNTIME_H
#include "peer.h"
#include "airtime.h"

#define MESH_PORTAL_FRAME_MAX 600
#define MESH_PORTAL_HELLO_SECONDS 5
#define MESH_PORTAL_TTL 15
struct mesh_portal_advert {
	uint8_t id[16];
	uint8_t station[6];
	uint8_t factory[6];
	uint8_t bridge[6];
	uint8_t linklocal[16];
	uint32_t gateway;
	enum mesh_role role;
	unsigned int ttl;
};
struct mesh_portal_record {
	struct mesh_portal_advert advert;
	struct mesh_airtime_window window;
	struct mesh_airtime_result metric;
	uint8_t next_hop[6];
	uint64_t seen_seconds;
	bool used;
	bool metric_valid;
	bool probe_valid;
};
struct mesh_neighbor {
	bool exists;
	uint8_t mac[6];
	uint16_t state;
	uint8_t flags;
	uint8_t type;
};
struct mesh_portal_io {
	void *context;
	int (*measure)(void *, uint32_t, const uint8_t[6], struct mesh_path_sample *, struct mesh_station_sample *);
	int (*probe)(void *, const struct mesh_portal_advert *);
	/* 0 = complete lookup (including absent neighbor), 1 = device ENODEV,
	 * negative = unavailable/malformed. Device absence never authorizes writes. */
	int (*neighbor_get)(void *, uint32_t, uint32_t, struct mesh_neighbor *);
	int (*neighbor_set)(void *, uint32_t, uint32_t, const struct mesh_neighbor *);
	int (*client_ready)(void *);
	int (*renew)(void *);
};
struct mesh_portal_runtime {
	struct mesh_peer *peer;
	struct mesh_portal_io io;
	struct mesh_portal_record records[MESH_PORTALS_MAX];
	struct mesh_portal_state selection;
	struct mesh_neighbor previous;
	struct mesh_neighbor installed;
	struct mesh_neighbor pending;
	struct mesh_file_stamp stamp;
	uint8_t notified[16];
	uint32_t gateway;
	uint32_t bridge_index;
	uint32_t mesh_index;
	uint64_t rounds;
	int directory;
	bool pin_active;
	bool renew_uncertain;
	bool journal_exists;
	uint8_t phase;
};
int mesh_portal_frame_encode(const struct mesh_portal_advert *, void *, size_t, size_t *);
int mesh_portal_frame_decode(const void *, size_t, const uint8_t[16], struct mesh_portal_advert *);
int mesh_portal_hear(struct mesh_portal_runtime *, const void *, size_t, const uint8_t[16], uint64_t);
int mesh_portal_runtime_open(struct mesh_portal_runtime *, struct mesh_peer *, const struct mesh_portal_io *,
							 uint32_t, uint32_t, uint32_t);
int mesh_portal_round(struct mesh_portal_runtime *, const struct mesh_plan *, uint64_t);
int mesh_portal_runtime_stop(struct mesh_portal_runtime *);
int mesh_portal_journal_load(struct mesh_portal_runtime *);
int mesh_portal_journal_store(struct mesh_portal_runtime *);
int mesh_portal_runtime_status(const struct mesh_portal_runtime *, char *, size_t);

/* All live operations are reachable only through the existing explicit OpenWrt
 * opt-in entrypoints. Tests inject transports; no host network changes. */
int mesh_portal_neighbor_decode(const void *, size_t, uint32_t, uint32_t, struct mesh_neighbor *);
int mesh_portal_neighbor_request(void *, size_t, size_t *, uint32_t, uint32_t, const struct mesh_neighbor *);
int mesh_portal_native_get(void *, uint32_t, uint32_t, struct mesh_neighbor *);
int mesh_portal_native_set(void *, uint32_t, uint32_t, const struct mesh_neighbor *);
int mesh_portal_live_probe(void *, const struct mesh_portal_advert *);
int mesh_portal_local_advert(struct mesh_peer *, struct mesh_portal_advert *);
int mesh_portal_lldp_open(uint32_t);
int mesh_portal_lldp_send(int, uint32_t, const struct mesh_portal_advert *);
int mesh_portal_lldp_receive(int, uint32_t, struct mesh_portal_runtime *, const uint8_t[16], uint64_t);
int mesh_portal_live_measure(void *, uint32_t, const uint8_t[6], struct mesh_path_sample *, struct mesh_station_sample *);
int mesh_portal_live_client_ready(void *);
int mesh_portal_live_renew(void *);
int mesh_portal_echo_build(const struct mesh_portal_advert *, const struct mesh_portal_advert *,
						   const uint8_t[16], void *, size_t, size_t *);
int mesh_portal_echo_reply(const struct mesh_portal_advert *, const struct mesh_portal_advert *,
						   const uint8_t[16], const void *, size_t);
#endif
