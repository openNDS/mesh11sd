/**
 * @file mrp.h
 * @brief Explicit IPv4-only MRP profile and owned DNS/firewall types.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#ifndef MESH_MRP_H
#define MESH_MRP_H
#include "services.h"
/* IPv4-only is an explicit experimental profile, NEVER an inferred GUA/RA
 * policy. Unsupported IPv6 profiles fail before network or service effects. */
int mesh_mrp_configure(int root, struct mesh_peer *peer, struct mesh_native_config *config,
					   enum mesh_mrp_profile profile);
int mesh_mrp_firewall_payload(struct blob_buf *buffer, const struct mesh_native_config *config,
							  const struct mesh_plan *plan);
int mesh_mrp_firewall_verify(struct mesh_peer *peer, bool absent);
int mesh_mrp_verify(struct mesh_peer *peer);
int mesh_mrp_dns_guard(struct mesh_services *services);
int mesh_mrp_dns_text(const struct mesh_services *services, bool portal, char *text, size_t capacity);
int mesh_mrp_dns_profile(struct mesh_services *services, bool portal);
int mesh_mrp_live_reload(void);
int mesh_mrp_live_chain(const char *chain, struct mesh_runtime_data *reply);
int mesh_mrp_live_probe(const char *bridge, const char *uplink, uint32_t gateway,
						uint8_t prefix, uint32_t dns_pid);
int mesh_mrp_udp67_entry(const char *line, uint64_t *inode);
int mesh_mrp_dns_response(const unsigned char *data, size_t size, uint16_t id, uint32_t address);
#endif
