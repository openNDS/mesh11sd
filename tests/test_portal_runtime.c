/**
 * @file test_portal_runtime.c
 * @brief Host fixtures for portal selection, LLDP, neighbor pin, and renew.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#define MESH_SERVICES_FIXTURE_ONLY
#include "test_services.c"
#include "portal_runtime.h"
#include <arpa/inet.h>
#include <linux/genetlink.h>
#include <linux/neighbour.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <linux/rtnetlink.h>

struct portal_fake {
	struct service_fake base;
	struct mesh_peer *owner;
	struct mesh_neighbor neighbor;
	unsigned int writes;
	unsigned int renews;
	unsigned int probes;
	unsigned int measurements;
	uint32_t packets[2];
	bool failed_a;
	bool failed_b;
	bool bad_path;
	bool client_deferred;
	bool renew_timeout;
	bool write_failure;
	bool uncertain_next_set;
	bool age_restored_neighbor;
	uint32_t bridge_index;
	bool old_bridge_gone;
	unsigned int probe_elapsed_ms;
	bool cancel_after_probe;
};
static int portal_call(void *context, const char *object, const char *method,
						struct blob_attr *request, struct mesh_runtime_data *reply)
{
	struct portal_fake *fake = context;
	struct mesh_message message;
	if (strcmp(object, "network.interface.mesh11sd_v8_client") != 0) {
		return service_call(context, object, method, request, reply);
	}
	if (strcmp(method, "renew") == 0) {
		fake->renews++;
		CHECK("actual renewal RPC is empty and scoped ONLY to owned DHCP alias", blob_len(request) == 0);
		return fake->renew_timeout ? 7 : 0;
	}
	if (strcmp(method, "status") != 0) {
		return 3;
	}
	must(mesh_message_init(&message));
	must(blobmsg_add_string(&message.blob, "proto", "dhcp"));
	must(blobmsg_add_string(&message.blob, "device", "br-lan"));
	must(blobmsg_add_u8(&message.blob, "autostart", true));
	must(blobmsg_add_u8(&message.blob, "up", !fake->client_deferred));
	must(blobmsg_add_u8(&message.blob, "pending", false));
	must(mesh_message_copy(&message, reply));
	mesh_message_clear(&message);
	return 0;
}
static int sample(void *context, uint32_t index, const uint8_t destination[6],
				   struct mesh_path_sample *path, struct mesh_station_sample *station)
{
	struct portal_fake *fake = context;
	unsigned int slot = destination[5] == 0x62 ? 0 : 1;
	fake->measurements++;
	memset(path, 0, sizeof(*path));
	memset(station, 0, sizeof(*station));
	path->ifindex = fake->bad_path ? 999 : index;
	memcpy(path->destination, destination, 6);
	memcpy(path->next_hop, destination, 6);
	path->active = true;
	path->hops = 1;
	station->ifindex = index;
	memcpy(station->mac, destination, 6);
	station->established = true;
	station->have_counters = true;
	station->have_tx_rate = true;
	station->have_retries = true;
	station->tx_rate = slot == 0 ? 540 : 1080;
	fake->packets[slot] += 100;
	station->tx_packets = fake->packets[slot];
	station->tx_retries = fake->packets[slot] / 10;
	return 0;
}
static int probe(void *context, const struct mesh_portal_advert *advert)
{
	struct portal_fake *fake = context;
	fake->probes++;
	fake->base.peer.clock += fake->probe_elapsed_ms;
	if (fake->cancel_after_probe && fake->owner->cancelled != NULL) {
		*fake->owner->cancelled = 1;
	}
	return (advert->station[5] == 0x62 ? fake->failed_a : fake->failed_b) ? -1 : 0;
}
static int get(void *context, uint32_t index, uint32_t gateway, struct mesh_neighbor *out)
{
	struct portal_fake *fake = context;
	if (index != fake->bridge_index || gateway != fake->owner->config->config.portal_ipv4.address) {
		return fake->old_bridge_gone ? 1 : -1;
	}
	*out = fake->neighbor;
	return 0;
}
static int set(void *context, uint32_t index, uint32_t gateway, const struct mesh_neighbor *next)
{
	struct portal_fake *fake = context;
	if (index != fake->bridge_index || gateway != fake->owner->config->config.portal_ipv4.address || fake->write_failure) {
		return -1;
	}
	fake->writes++;
	fake->neighbor = *next;
	if (fake->age_restored_neighbor && next->exists && next->state != NUD_PERMANENT) {
		fake->neighbor.state = NUD_REACHABLE;
	}
	if (fake->uncertain_next_set) {
		fake->uncertain_next_set = false;
		return -1;
	}
	return 0;
}
static int ready(void *context)
{
	struct portal_fake *fake = context;
	return mesh_portal_live_client_ready(fake->owner);
}
static int renew(void *context)
{
	struct portal_fake *fake = context;
	return mesh_portal_live_renew(fake->owner);
}
static void advertisement(struct mesh_portal_advert *out, const struct mesh_native_config *config, unsigned int node)
{
	struct mesh_identity identity;
	uint8_t factory[6] = {2, 0, 0, 0, 0, (uint8_t)(0x42 + node)};
	uint8_t bridge_mac[6] = {2, 0, 0, 0, 0, (uint8_t)(0x52 + node)};
	memset(out, 0, sizeof(*out));
	must(mesh_identity_build(config->group, factory, bridge_mac, &identity));
	must(inet_pton(AF_INET6, identity.portal_address, out->id) == 1 ? 0 : -1);
	memcpy(out->factory, factory, 6);
	memcpy(out->bridge, bridge_mac, 6);
	memcpy(out->station, factory, 6);
	out->station[5] = (uint8_t)(0x62 + node);
	out->gateway = config->config.portal_ipv4.address;
	out->role = MESH_ROLE_MRP;
	out->ttl = MESH_PORTAL_TTL;
	out->linklocal[0] = 0xfe;
	out->linklocal[1] = 0x80;
	out->linklocal[15] = (uint8_t)(0x42 + node);
}
static void hear(struct mesh_portal_runtime *runtime, const struct mesh_portal_advert *advert, const uint8_t local[16], uint64_t now)
{
	uint8_t frame[MESH_PORTAL_FRAME_MAX];
	size_t length;
	must(mesh_portal_frame_encode(advert, frame, sizeof(frame), &length));
	must(mesh_portal_hear(runtime, frame, length, local, now));
}
static void airtime_tests(void)
{
	struct mesh_station_sample station = { .mac = {2,0,0,0,0,1}, .ifindex = 20, .tx_rate = 540,
		.rx_rate = 1080, .tx_packets = 100, .tx_retries = 30, .have_retries = true,
		.have_counters = true, .have_tx_rate = true, .have_rx_rate = true, .have_signal = true,
		.signal_average = -40, .established = true };
	struct mesh_airtime_window window = {0};
	struct mesh_airtime_result result = {0};
	CHECK("counter-window baseline never invents a successful metric", mesh_calc_airtime_metric(&window,&station,1,true,-72,1000,&result) == 1);
	station.tx_packets += 100;
	station.tx_retries += 20;
	CHECK("named v7 54Mbps zero-loss vector equals152, not kernel8192 offset", mesh_calc_airtime_metric(&window,&station,1,true,-72,6000,&result) == 0 && result.next_hop_metric == 152 && result.adjusted_metric == 152 && result.retry_delta == 20);
	station.tx_packets += 100;
	station.tx_failed += 10;
	CHECK("named v7 10percent-loss vector uses DELTAS and equals168", mesh_calc_airtime_metric(&window,&station,1,true,-72,11000,&result) == 0 && result.fail_percent == 10 && result.next_hop_metric == 168);
	station.tx_packets += 100;
	CHECK("named hop2 exponent1.6 vector exposes raw152 and adjusted461", mesh_calc_airtime_metric(&window,&station,2,true,-72,16000,&result) == 0 && result.next_hop_metric == 152 && result.adjusted_metric == 461);
	station.tx_packets += 100;
	CHECK("named hop3 vector rounds to882", mesh_calc_airtime_metric(&window,&station,3,true,-72,21000,&result) == 0 && result.adjusted_metric == 882);
	station.tx_rate = 10;
	station.tx_packets += 100;
	CHECK("strong RX fallback replaces reported1Mbps with real108Mbps", mesh_calc_airtime_metric(&window,&station,1,true,-72,26000,&result) == 0 && result.used_rx_fallback && result.next_hop_metric == 76);
	station.tx_packets += 100;
	CHECK("unset RSSI threshold is deliberate conservative20Mbps, no fabricated strength", mesh_calc_airtime_metric(&window,&station,1,false,0,31000,&result) == 0 && result.used_conservative_rate && result.next_hop_metric == 410);
	station.signal_average = -90;
	station.tx_packets += 100;
	CHECK("weak signal affects fallback rate only, never drops established peer", mesh_calc_airtime_metric(&window,&station,1,true,-72,36000,&result) == 0 && result.used_conservative_rate && station.established);
	station.tx_rate = 540;
	station.tx_packets += 100;
	station.tx_failed += 100;
	CHECK("named90percent-loss clamp vector equals1503", mesh_calc_airtime_metric(&window,&station,1,true,-72,41000,&result) == 0 && result.fail_percent == 90 && result.next_hop_metric == 1503);
	CHECK("duplicate timestamp cannot reuse or manufacture another counter window", mesh_calc_airtime_metric(&window,&station,1,true,-72,41000,&result) < 0);
	station.tx_packets = 1;
	CHECK("counter reset rebases instead of wrapping to artificial loss", mesh_calc_airtime_metric(&window,&station,1,true,-72,46000,&result) == 1);
	station.tx_packets += 100;
	CHECK("stale sample interval rebases without old lifetime penalty", mesh_calc_airtime_metric(&window,&station,1,true,-72,80000,&result) == 1);
	station.have_counters = false;
	CHECK("missing station loss evidence fails clearly", mesh_calc_airtime_metric(&window,&station,1,true,-72,85000,&result) < 0);
	station.have_counters = true;
	station.tx_rate = 0;
	station.rx_rate = 0;
	CHECK("zero-valued rates are unavailable evidence, not a made-up20Mbps success", mesh_calc_airtime_metric(&window,&station,1,true,-72,85000,&result) < 0);
	station.tx_rate = 540;
	station.established = false;
	CHECK("blocked plink cannot produce a usable portal metric", mesh_calc_airtime_metric(&window,&station,1,true,-72,85000,&result) < 0);
}
struct native_message {
	struct genlmsghdr generic;
	uint8_t bytes[1024];
};
static size_t portal_nla(struct native_message *message, size_t offset, uint16_t type, const void *value, size_t length)
{
	struct nlattr *attr = (struct nlattr *)((uint8_t *)message + offset);
	attr->nla_type = type;
	attr->nla_len = (uint16_t)(NLA_HDRLEN + length);
	if (length != 0) {
		memcpy((uint8_t *)attr + NLA_HDRLEN, value, length);
	}
	return offset + NLA_ALIGN(attr->nla_len);
}
static void native_tests(void)
{
	struct native_message message = {0};
	struct mesh_station_sample station;
	struct mesh_path_sample path;
	uint32_t index = 20;
	uint32_t rate = 540;
	uint32_t packets = 100;
	uint32_t failed = 10;
	uint32_t retries = 20;
	uint32_t raw_offset = 8195;
	uint8_t mac[6] = {2,0,0,0,0,1};
	uint8_t established = NL80211_PLINK_ESTAB;
	uint8_t active = NL80211_MPATH_FLAG_ACTIVE;
	uint8_t hops = 2;
	int8_t signal = -50;
	size_t offset = GENL_HDRLEN;
	size_t nested;
	size_t rate_nested;
	uint32_t random = 0x31415926;
	unsigned int i;
	message.generic.cmd = NL80211_CMD_NEW_STATION;
	offset = portal_nla(&message,offset,NL80211_ATTR_IFINDEX,&index,4);
	offset = portal_nla(&message,offset,NL80211_ATTR_MAC,mac,6);
	nested = offset;
	offset = portal_nla(&message,offset,NL80211_ATTR_STA_INFO | NLA_F_NESTED,NULL,0);
	offset = portal_nla(&message,offset,NL80211_STA_INFO_PLINK_STATE,&established,1);
	offset = portal_nla(&message,offset,NL80211_STA_INFO_TX_PACKETS,&packets,4);
	offset = portal_nla(&message,offset,NL80211_STA_INFO_TX_FAILED,&failed,4);
	offset = portal_nla(&message,offset,NL80211_STA_INFO_TX_RETRIES,&retries,4);
	offset = portal_nla(&message,offset,NL80211_STA_INFO_SIGNAL_AVG,&signal,1);
	rate_nested = offset;
	offset = portal_nla(&message,offset,NL80211_STA_INFO_TX_BITRATE | NLA_F_NESTED,NULL,0);
	offset = portal_nla(&message,offset,NL80211_RATE_INFO_BITRATE32,&rate,4);
	((struct nlattr *)((uint8_t *)&message + rate_nested))->nla_len = (uint16_t)(offset-rate_nested);
	((struct nlattr *)((uint8_t *)&message + nested))->nla_len = (uint16_t)(offset-nested);
	CHECK("actual nested nl80211 station shape decodes rates retry/loss and signed RSSI", mesh_decode_station(&message,offset,&station) == 0 && station.tx_rate == 540 && station.tx_retries == 20 && station.signal_average == -50 && station.established);
	{
		struct native_message duplicate = message;
		size_t duplicate_length = portal_nla(&duplicate,offset,NL80211_ATTR_IFINDEX,&index,4);
		CHECK("duplicate station identity evidence is rejected", mesh_decode_station(&duplicate,duplicate_length,&station)!=0);
		duplicate=message;
		duplicate.generic.cmd=NL80211_CMD_GET_STATION;
		CHECK("wrong generic-netlink response command is not silently consumed", mesh_decode_station(&duplicate,offset,&station)!=0);
		duplicate=message;
		{
			uint16_t legacy_rate=540;
			struct nlattr *rate_attr=(struct nlattr *)((uint8_t *)&duplicate+rate_nested+NLA_HDRLEN);
			rate_attr->nla_type=NL80211_RATE_INFO_BITRATE;
			rate_attr->nla_len=NLA_HDRLEN+sizeof(legacy_rate);
			memcpy((uint8_t *)rate_attr+NLA_HDRLEN,&legacy_rate,sizeof(legacy_rate));
		}
		CHECK("native16-bit legacy bitrate schema remains supported alongside bitrate32", mesh_decode_station(&duplicate,offset,&station)==0 && station.tx_rate==540);
	}
	CHECK("unaligned generic-netlink data fails before any typed header access", mesh_decode_station((uint8_t *)&message+1,offset-1,&station)!=0);
	CHECK("truncated native station reply is rejected", mesh_decode_station(&message,offset-1,&station) != 0);
	for (i = 0; i < 10000; i++) {
		struct native_message corrupt = message;
		random = random * 1664525U + 1013904223U;
		((uint8_t *)&corrupt)[random % offset] ^= (uint8_t)(random >> 24);
		mesh_decode_station(&corrupt,(random >> 8) % (offset+1),&station);
	}
	CHECK("10000 bounded mutated native station messages complete", true);
	memset(&message,0,sizeof(message));
	message.generic.cmd = NL80211_CMD_NEW_MPATH;
	offset = portal_nla(&message,GENL_HDRLEN,NL80211_ATTR_IFINDEX,&index,4);
	offset = portal_nla(&message,offset,NL80211_ATTR_MAC,mac,6);
	offset = portal_nla(&message,offset,NL80211_ATTR_MPATH_NEXT_HOP,mac,6);
	nested = offset;
	offset = portal_nla(&message,offset,NL80211_ATTR_MPATH_INFO | NLA_F_NESTED,NULL,0);
	offset = portal_nla(&message,offset,NL80211_MPATH_INFO_FLAGS,&active,1);
	offset = portal_nla(&message,offset,NL80211_MPATH_INFO_HOP_COUNT,&hops,1);
	offset = portal_nla(&message,offset,NL80211_MPATH_INFO_METRIC,&raw_offset,4);
	((struct nlattr *)((uint8_t *)&message + nested))->nla_len = (uint16_t)(offset-nested);
	CHECK("native mpath decode retains next-hop/hops and deliberately ignores bad raw metric", mesh_decode_path(&message,offset,&path) == 0 && path.hops == 2 && path.active && memcmp(path.next_hop,mac,6) == 0);
	CHECK("truncated path evidence cannot masquerade as complete", mesh_decode_path(&message,offset-1,&path) != 0);
	{
		union { struct nlmsghdr align; uint8_t bytes[256]; } request;
		struct mesh_neighbor wanted = { .exists = true, .mac = {2,0,0,0,0,2}, .state = NUD_PERMANENT, .type = RTN_UNICAST };
		struct mesh_neighbor decoded;
		size_t length;
		CHECK("actual owned ARP add request uses native acknowledged create/replace", mesh_portal_neighbor_request(request.bytes,sizeof(request),&length,10,0xc0000201,&wanted) == 0 && request.align.nlmsg_type == RTM_NEWNEIGH && (request.align.nlmsg_flags & (NLM_F_ACK|NLM_F_CREATE|NLM_F_REPLACE)) == (NLM_F_ACK|NLM_F_CREATE|NLM_F_REPLACE));
		CHECK("real rtnetlink neighbor payload decodes exact gateway interface and permanent MAC", mesh_portal_neighbor_decode(NLMSG_DATA(&request.align),length-NLMSG_HDRLEN,10,0xc0000201,&decoded) == 0 && decoded.state == NUD_PERMANENT && memcmp(decoded.mac,wanted.mac,6) == 0);
		CHECK("unaligned native neighbor payload fails before typed ndmsg access", mesh_portal_neighbor_decode((uint8_t *)NLMSG_DATA(&request.align)+2,length-NLMSG_HDRLEN-2,10,0xc0000201,&decoded)!=0);
		CHECK("foreign neighbor interface cannot be adopted", mesh_portal_neighbor_decode(NLMSG_DATA(&request.align),length-NLMSG_HDRLEN,11,0xc0000201,&decoded) != 0);
		wanted.exists = false;
		CHECK("owned ARP removal is scoped RTM_DELNEIGH, not table flush", mesh_portal_neighbor_request(request.bytes,sizeof(request),&length,10,0xc0000201,&wanted) == 0 && request.align.nlmsg_type == RTM_DELNEIGH);
		CHECK("invalid native ARP interface rejected before any socket", mesh_portal_native_get(NULL,0,0xc0000201,&decoded) != 0);
	}
}
static uint16_t echo_checksum(uint8_t *packet)
{
	uint32_t sum = 24 + 58;
	unsigned int i;
	for (i=22;i<78;i+=2) {
		sum += (unsigned int)packet[i]*256 + packet[i+1];
	}
	while (sum>>16) {
		sum = (sum&65535)+(sum>>16);
	}
	return (uint16_t)~sum;
}
static void codec_tests(const struct mesh_portal_advert *a, const struct mesh_portal_advert *b, const uint8_t local[16])
{
	uint8_t frame[MESH_PORTAL_FRAME_MAX];
	uint8_t nonce[16] = {1,2,3,4,5,6,7,8,9};
	struct mesh_portal_advert decoded;
	size_t length;
	size_t i;
	uint16_t sum;
	uint32_t random = 42;
	struct mesh_portal_runtime cache = { .gateway = a->gateway };
	struct mesh_portal_advert changed;
	uint8_t foreign_group[16];
	CHECK("portal LLDP encodes without mesh IDs keys or ferry fields", mesh_portal_frame_encode(a,frame,sizeof(frame),&length) == 0 && memmem(frame,length,"fside",5) == NULL);
	CHECK("portal LLDP distinguishes factory bridge and station identities", mesh_portal_frame_decode(frame,length,local,&decoded) == 0 && memcmp(decoded.factory,a->factory,6) == 0 && memcmp(decoded.bridge,a->bridge,6) == 0 && memcmp(decoded.station,a->station,6) == 0 && memcmp(frame+6,a->bridge,6) == 0);
	for (i=0;i<length;i++) {
		if (mesh_portal_frame_decode(frame,i,local,&decoded) == 0) {
			break;
		}
	}
	CHECK("every truncated LLDP announcement is rejected", i == length);
	frame[0] ^= 1;
	CHECK("wrong LLDP destination is rejected", mesh_portal_frame_decode(frame,length,local,&decoded) != 0);
	frame[0] ^= 1;
	frame[6] ^= 2;
	CHECK("announced bridge MAC must match actual Ethernet source", mesh_portal_frame_decode(frame,length,local,&decoded) != 0);
	frame[6] ^= 2;
	frame[14] ^= 6;
	CHECK("LLDP mandatory chassis/port/TTL ordering is enforced", mesh_portal_frame_decode(frame,length,local,&decoded) != 0);
	frame[14] ^= 6;
	memcpy(foreign_group,local,16);
	foreign_group[1] ^= 1;
	CHECK("foreign shared48 portal announcements cannot enter this mesh group", mesh_portal_frame_decode(frame,length,foreign_group,&decoded) != 0);
	CHECK("valid announcements enter the bounded cache", mesh_portal_hear(&cache,frame,length,local,20)==0);
	CHECK("same-identity timestamps cannot run backwards", mesh_portal_hear(&cache,frame,length,local,19)!=0);
	changed=*a;
	changed.factory[5]^=2;
	must(mesh_portal_frame_encode(&changed,frame,sizeof(frame),&length));
	CHECK("same ULA advertised by different factory identity is an explicit conflict", mesh_portal_hear(&cache,frame,length,local,21)!=0);
	changed=*a;
	changed.gateway^=1;
	must(mesh_portal_frame_encode(&changed,frame,sizeof(frame),&length));
	CHECK("valid LLDP from a different IPv4 pool is not usable evidence", mesh_portal_hear(&cache,frame,length,local,21)!=0);
	changed=*a;
	changed.ttl=181;
	CHECK("unbounded remote advertisement lifetime is rejected at construction", mesh_portal_frame_encode(&changed,frame,sizeof(frame),&length)!=0);
	changed=*a;
	changed.role=MESH_ROLE_PEER;
	CHECK("peer cannot falsely announce itself as a portal", mesh_portal_frame_encode(&changed,frame,sizeof(frame),&length)!=0);
	changed=*a;
	changed.ttl=0;
	must(mesh_portal_frame_encode(&changed,frame,sizeof(frame),&length));
	CHECK("zero TTL is a real withdrawal, not an immortal cache entry", mesh_portal_hear(&cache,frame,length,local,22)==0);
	for (i=0;i<MESH_PORTALS_MAX;i++) {
		if (cache.records[i].used) {
			break;
		}
	}
	CHECK("withdrawal leaves no eligible cached advertisement", i==MESH_PORTALS_MAX);
	must(mesh_portal_frame_encode(a,frame,sizeof(frame),&length));
	for (i=0;i<10000;i++) {
		uint8_t corrupted[MESH_PORTAL_FRAME_MAX];
		memcpy(corrupted,frame,length);
		random=random*1664525U+1013904223U;
		corrupted[random%length]^=(uint8_t)(random>>24);
		mesh_portal_frame_decode(corrupted,(random>>8)%(length+1),local,&decoded);
	}
	CHECK("10000 bounded malformed portal LLDP frames complete", true);
	CHECK("raw selected-mesh ICMPv6 request binds actual bridge MAC and observed linklocal", mesh_portal_echo_build(a,b,nonce,frame,sizeof(frame),&length) == 0 && length == 78 && memcmp(frame,b->bridge,6) == 0 && memcmp(frame+6,a->bridge,6) == 0 && frame[54] == 128 && echo_checksum(frame) == 0);
	must(mesh_portal_echo_build(b,a,nonce,frame,sizeof(frame),&length));
	frame[54] = 129;
	frame[56] = 0;
	frame[57] = 0;
	sum = echo_checksum(frame);
	frame[56] = (uint8_t)(sum>>8);
	frame[57] = (uint8_t)sum;
	CHECK("matching bound-path echo reply proves correlated remote L2 identity", mesh_portal_echo_reply(a,b,nonce,frame,length) == 0);
	frame[62] ^= 1;
	CHECK("wrong nonce or checksum is not a successful portal probe", mesh_portal_echo_reply(a,b,nonce,frame,length) != 0);
	CHECK("truncated probe packet cannot prove reachability", mesh_portal_echo_reply(a,b,nonce,frame,77) != 0);
}
int main(int argc,char **argv)
{
	struct mesh_native_config config = {0};
	struct mesh_facts facts;
	struct mesh_plan plan;
	struct mesh_peer peer;
	struct portal_fake fake = { .base = { .peer.config=&config, .dns_pid=100, .foreign_pid=999, .lan_up=true } };
	struct mesh_peer_transport transport_io = transport(&fake.base.peer);
	struct mesh_discovery_ops discovery = { .context=&peer, .radio_config=mesh_peer_radio, .bridge=bridge, .role=role };
	struct mesh_portal_io io = { .context=&fake, .measure=sample, .probe=probe, .neighbor_get=get,
		.neighbor_set=set, .client_ready=ready, .renew=renew };
	struct mesh_portal_runtime runtime;
	struct mesh_portal_advert a;
	struct mesh_portal_advert b;
	struct mesh_neighbor original = { .exists=true, .mac={2,0,0,0,0,9}, .state=NUD_STALE, .type=RTN_UNICAST };
	uint8_t local[16];
	char summary[512];
	unsigned int renew_count;
	uint64_t second;
	int root;
	int dir;
	struct stat metadata;
	if (argc!=2) {
		return 2;
	}
	setvbuf(stdout,NULL,_IONBF,0);
	airtime_tests();
	native_tests();
	root=open(argv[1],O_RDONLY|O_DIRECTORY|O_CLOEXEC);
	if (root<0) {
		return 2;
	}
	services_fixture(root);
	fake.base.root=root;
	fake.owner=&peer;
	fake.bridge_index=10;
	transport_io.invoke=portal_call;
	dir=directory(root,"portal-state");
	CHECK("portal runtime uses the existing exclusive owner session", mesh_peer_open(&peer,dir,&transport_io)==0);
	CHECK("native planner still supplies agreed ULA and actual bridge identity", mesh_native_plan(root,&discovery,&config,&facts,&plan)==MESH_OK);
	peer.plan=&plan;
	peer.config=&config;
	must(inet_pton(AF_INET6,plan.identity.portal_address,local)==1?0:-1);
	advertisement(&a,&config,0);
	advertisement(&b,&config,1);
	codec_tests(&a,&b,local);
	fake.neighbor=original;
	CHECK("unowned/prepared LAN-client mode is rejected before any ARP or renewal effect", mesh_portal_runtime_open(&runtime,&peer,&io,10,20,config.config.portal_ipv4.address)!=0 && fake.writes==0 && fake.renews==0);
	config.manage_services=true;
	CHECK("portal runtime opens without network effects", mesh_portal_runtime_open(&runtime,&peer,&io,10,20,config.config.portal_ipv4.address)==0 && fake.writes==0 && fake.renews==0);
	hear(&runtime,&a,local,0);
	CHECK("first live evidence window does not fabricate metric or renew", mesh_portal_round(&runtime,&plan,0)==0 && !runtime.selection.selected && fake.renews==0);
	hear(&runtime,&a,local,5);
	CHECK("measured portal installs owned ARP then actually invokes netifd renew", mesh_portal_round(&runtime,&plan,5000)==0 && runtime.selection.selected && fake.renews==1 && fake.neighbor.state==NUD_PERMANENT && memcmp(fake.neighbor.mac,a.bridge,6)==0);
	CHECK("portal metadata is fixed-size private and contains public identities only", fstatat(dir,"portal.meta",&metadata,0)==0 && (metadata.st_mode&0777)==0600 && metadata.st_size==96);
	CHECK("duplicate round is rejected before repeat probes or renewal", mesh_portal_round(&runtime,&plan,5000)!=0 && fake.renews==1);
	for (second=10;second<=70;second+=5) {
		hear(&runtime,&a,local,second);
		hear(&runtime,&b,local,second);
		must(mesh_portal_round(&runtime,&plan,second*1000) < 0 ? -1 : 0);
	}
	CHECK("better portal must satisfy actual60second continuous hold", memcmp(runtime.selection.selected_id,a.id,16)==0 && fake.renews==1);
	hear(&runtime,&a,local,75);
	hear(&runtime,&b,local,75);
	CHECK("20percent-margin challenger switches after60seconds with one renewal", mesh_portal_round(&runtime,&plan,75000)==0 && memcmp(runtime.selection.selected_id,b.id,16)==0 && fake.renews==2 && memcmp(fake.neighbor.mac,b.bridge,6)==0);
	b.role=MESH_ROLE_MBP;
	hear(&runtime,&b,local,80);
	hear(&runtime,&a,local,80);
	CHECK("remote MRP to MBP role attribute alone never renews", mesh_portal_round(&runtime,&plan,80000)==0 && fake.renews==2);
	fake.failed_b=true;
	for (second=85;second<=90;second+=5) {
		hear(&runtime,&a,local,second);
		hear(&runtime,&b,local,second);
		must(mesh_portal_round(&runtime,&plan,second*1000) < 0 ? -1 : 0);
	}
	CHECK("two failed actual probes retain selected identity and pin", memcmp(runtime.selection.selected_id,b.id,16)==0 && fake.renews==2);
	hear(&runtime,&a,local,95);
	hear(&runtime,&b,local,95);
	CHECK("third failed probe immediately chooses healthy alternative without fresh hold", mesh_portal_round(&runtime,&plan,95000)==0 && memcmp(runtime.selection.selected_id,a.id,16)==0 && fake.renews==3);
	CHECK("status is numeric redacted evidence, not a credential payload", mesh_portal_runtime_status(&runtime,summary,sizeof(summary))>0 && strstr(summary,config.credential)==NULL && strstr(summary,config.group)==NULL && strstr(summary,"pin_owned=1")!=NULL);
	fake.neighbor.mac[5]^=1;
	CHECK("external ARP mutation is detected even before probe selection", mesh_portal_round(&runtime,&plan,100000)!=0);
	CHECK("cleanup preserves foreign neighbor replacement as explicit conflict", mesh_portal_runtime_stop(&runtime)!=0);
	fake.neighbor=runtime.installed;
	CHECK("clean stop restores original dynamic neighbor, not an ARP-table flush", mesh_portal_runtime_stop(&runtime)==0 && fake.neighbor.state==original.state && memcmp(fake.neighbor.mac,original.mac,6)==0);
	renew_count=fake.renews;
	config.config.portal_ipv4.address^=0x100;
	CHECK("cleanly released history permits an explicitly changed common gateway without network writes", mesh_portal_runtime_open(&runtime,&peer,&io,10,20,config.config.portal_ipv4.address)==0 && runtime.phase==0);
	config.config.portal_ipv4.address^=0x100;
	must(mesh_portal_runtime_open(&runtime,&peer,&io,10,20,config.config.portal_ipv4.address));
	fake.bridge_index=11;
	CHECK("clean restart may recreate an owned bridge with a new kernel ifindex", mesh_portal_runtime_open(&runtime,&peer,&io,11,20,config.config.portal_ipv4.address)==0 && runtime.bridge_index==11 && runtime.phase==0);
	fake.bridge_index=10;
	CHECK("restart reopens public identity metadata without repeated renewal", mesh_portal_runtime_open(&runtime,&peer,&io,10,20,config.config.portal_ipv4.address)==0);
	hear(&runtime,&a,local,105);
	must(mesh_portal_round(&runtime,&plan,105000) < 0 ? -1 : 0);
	hear(&runtime,&a,local,110);
	CHECK("same-identity restart repins but does not send an extra renew", mesh_portal_round(&runtime,&plan,110000)==0 && fake.renews==renew_count && runtime.phase==2);
	CHECK("crash recovery restores only the recorded owned ARP entry", mesh_portal_runtime_open(&runtime,&peer,&io,10,20,config.config.portal_ipv4.address)==0 && fake.neighbor.state==original.state);
	CHECK("restart metadata does not contain a new role-flip renewal", fake.renews==renew_count);
	hear(&runtime,&a,local,111);
	must(mesh_portal_round(&runtime,&plan,111000) < 0 ? -1 : 0);
	hear(&runtime,&a,local,113);
	must(mesh_portal_round(&runtime,&plan,113000) < 0 ? -1 : 0);
	fake.bridge_index=11;
	fake.old_bridge_gone=true;
	fake.neighbor=(struct mesh_neighbor){0};
	CHECK("crash recovery after actual old-device ENODEV does not write to a recycled ifindex", mesh_portal_runtime_open(&runtime,&peer,&io,11,20,config.config.portal_ipv4.address)==0 && runtime.phase==0 && !fake.neighbor.exists);
	fake.old_bridge_gone=false;
	fake.bridge_index=10;
	fake.neighbor=original;
	must(mesh_portal_runtime_open(&runtime,&peer,&io,10,20,config.config.portal_ipv4.address));
	hear(&runtime,&a,local,115);
	must(mesh_portal_round(&runtime,&plan,115000) < 0 ? -1 : 0);
	hear(&runtime,&a,local,120);
	must(mesh_portal_round(&runtime,&plan,120000) < 0 ? -1 : 0);
	for (second=135;second<=145;second+=5) {
		must(mesh_portal_round(&runtime,&plan,second*1000) < 0 ? -1 : 0);
	}
	CHECK("expired announcements cannot remain selected despite unrelated echo success", !runtime.selection.selected && runtime.phase==0 && fake.renews==renew_count);
	fake.client_deferred=true;
	fake.failed_b=false;
	b.role=MESH_ROLE_MRP;
	hear(&runtime,&b,local,150);
	must(mesh_portal_round(&runtime,&plan,150000) < 0 ? -1 : 0);
	hear(&runtime,&b,local,155);
	CHECK("unstarted netifd client defers notification without inventing delivery", mesh_portal_round(&runtime,&plan,155000)==1 && fake.renews==renew_count);
	fake.client_deferred=false;
	fake.renew_timeout=true;
	hear(&runtime,&b,local,160);
	CHECK("actual renewal timeout is persisted as uncertain, never fake lease success", mesh_portal_round(&runtime,&plan,160000)==2 && fake.renews==renew_count+1 && runtime.renew_uncertain);
	hear(&runtime,&b,local,165);
	CHECK("uncertain renewal is NOT resent every polling cycle", mesh_portal_round(&runtime,&plan,165000)==2 && fake.renews==renew_count+1);
	CHECK("uncertain-intent crash recovery never silently repeats external request", mesh_portal_runtime_open(&runtime,&peer,&io,10,20,config.config.portal_ipv4.address)==0 && runtime.renew_uncertain && fake.renews==renew_count+1);
	fake.renew_timeout=false;
	CHECK("stop without an active owned pin is idempotent", mesh_portal_runtime_stop(&runtime)==0);
	text(dir,"portal.pending","PUBLIC-TEST-ONLY unknown interrupted write");
	CHECK("unknown interrupted portal metadata blocks without changing networking", mesh_portal_runtime_open(&runtime,&peer,&io,10,20,config.config.portal_ipv4.address)!=0);
	unlinkat(dir,"portal.pending",0);
	text(dir,"portal.meta","PUBLIC-TEST-ONLY malformed metadata");
	CHECK("malformed portal state is never interpreted as ownership", mesh_portal_runtime_open(&runtime,&peer,&io,10,20,config.config.portal_ipv4.address)!=0);
	unlinkat(dir,"portal.meta",0);
	fake.neighbor=original;
	CHECK("fresh phase-one failure fixture opens", mesh_portal_runtime_open(&runtime,&peer,&io,10,20,config.config.portal_ipv4.address)==0);
	hear(&runtime,&a,local,170);
	must(mesh_portal_round(&runtime,&plan,170000) < 0 ? -1 : 0);
	fake.write_failure=true;
	hear(&runtime,&a,local,175);
	CHECK("rejected kernel pin write leaves durable pre-mutation phase-one intent", mesh_portal_round(&runtime,&plan,175000)<0 && runtime.phase==1 && fake.neighbor.state==original.state);
	fake.write_failure=false;
	CHECK("crash before pin write recovers without claiming original dynamic neighbor", mesh_portal_runtime_open(&runtime,&peer,&io,10,20,config.config.portal_ipv4.address)==0 && runtime.phase==0 && fake.neighbor.state==original.state);
	hear(&runtime,&a,local,180);
	must(mesh_portal_round(&runtime,&plan,180000) < 0 ? -1 : 0);
	fake.uncertain_next_set=true;
	hear(&runtime,&a,local,185);
	CHECK("ACK loss after real pin mutation is not fake success", mesh_portal_round(&runtime,&plan,185000)<0 && runtime.phase==1 && fake.neighbor.state==NUD_PERMANENT);
	CHECK("phase-one crash AFTER pin mutation restores original dynamic neighbor", mesh_portal_runtime_open(&runtime,&peer,&io,10,20,config.config.portal_ipv4.address)==0 && runtime.phase==0 && fake.neighbor.state==original.state);
	hear(&runtime,&a,local,190);
	must(mesh_portal_round(&runtime,&plan,190000) < 0 ? -1 : 0);
	hear(&runtime,&a,local,195);
	must(mesh_portal_round(&runtime,&plan,195000) < 0 ? -1 : 0);
	fake.uncertain_next_set=true;
	CHECK("restore mutation is also journaled BEFORE a possible ACK-loss crash", mesh_portal_runtime_stop(&runtime)<0 && runtime.phase==3 && fake.neighbor.state==original.state);
	{
		unsigned int writes=fake.writes;
		CHECK("crash AFTER successful restoration recognizes previous identity without another write", mesh_portal_runtime_open(&runtime,&peer,&io,10,20,config.config.portal_ipv4.address)==0 && runtime.phase==0 && fake.writes==writes);
	}
	hear(&runtime,&a,local,200);
	must(mesh_portal_round(&runtime,&plan,200000) < 0 ? -1 : 0);
	hear(&runtime,&a,local,205);
	must(mesh_portal_round(&runtime,&plan,205000) < 0 ? -1 : 0);
	fake.age_restored_neighbor=true;
	CHECK("normal kernel dynamic-neighbor aging after restore ACK is not a false ownership conflict", mesh_portal_runtime_stop(&runtime)==0 && fake.neighbor.state==NUD_REACHABLE);
	fake.age_restored_neighbor=false;
	{
		unsigned int renews=fake.renews;
		struct mesh_plan portal_plan=plan;
		portal_plan.role=MESH_ROLE_MRP;
		hear(&runtime,&b,local,210);
		CHECK("local MRP remains independent of another restored MRP and never renews static gateway", mesh_portal_round(&runtime,&portal_plan,210000)==0 && !runtime.selection.selected && fake.renews==renews);
		CHECK("MRP observation has monotonic completed-round semantics too", mesh_portal_round(&runtime,&portal_plan,210000)<0);
	}
	{
		volatile sig_atomic_t cancel=1;
		unsigned int probes=fake.probes;
		peer.cancelled=&cancel;
		CHECK("signal cancellation stops probing before additional native effects", mesh_portal_round(&runtime,&plan,215000)<0 && fake.probes==probes);
		peer.cancelled=NULL;
	}
	unlinkat(dir,"portal.meta",0);
	fake.neighbor=(struct mesh_neighbor){.exists=true,.mac={2,0,0,0,0,8},.state=NUD_PERMANENT,.type=RTN_UNICAST};
	CHECK("fresh runtime can inspect but not claim an existing configured static neighbor", mesh_portal_runtime_open(&runtime,&peer,&io,10,20,config.config.portal_ipv4.address)==0);
	hear(&runtime,&a,local,220);
	must(mesh_portal_round(&runtime,&plan,220000) < 0 ? -1 : 0);
	hear(&runtime,&a,local,225);
	CHECK("foreign static gateway ARP override is preserved, not replaced", mesh_portal_round(&runtime,&plan,225000)!=0 && fake.neighbor.mac[5]==8);
	unlinkat(dir,"portal.meta",0);
	fake.neighbor=original;
	must(mesh_portal_runtime_open(&runtime,&peer,&io,10,20,config.config.portal_ipv4.address));
	hear(&runtime,&a,local,230);
	must(mesh_portal_round(&runtime,&plan,230000) < 0 ? -1 : 0);
	hear(&runtime,&a,local,235);
	must(mesh_portal_round(&runtime,&plan,235000) < 0 ? -1 : 0);
	fake.bad_path=true;
	hear(&runtime,&a,local,240);
	{
		unsigned int renewed=fake.renews;
		CHECK("successful echo with unavailable native metric is NOT a failed probe or made-up cost", mesh_portal_round(&runtime,&plan,240000)==1 && runtime.selection.failed_probes==0 && fake.renews==renewed);
		hear(&runtime,&a,local,245);
		CHECK("second missing metric still defers and retains owned pin", mesh_portal_round(&runtime,&plan,245000)==1 && runtime.selection.failed_probes==0 && runtime.phase==2);
		hear(&runtime,&a,local,250);
		CHECK("three successful probes with missing telemetry cannot trigger a false failover", mesh_portal_round(&runtime,&plan,250000)==1 && runtime.selection.selected && runtime.selection.failed_probes==0 && fake.renews==renewed);
		CHECK("status distinguishes an old retained metric from currently available evidence", mesh_portal_runtime_status(&runtime,summary,sizeof(summary))>0 && strstr(summary,"selected_metric_available=0")!=NULL && strstr(summary,"last_metric=")!=NULL);
	}
	fake.bad_path=false;
	hear(&runtime,&a,local,255);
	CHECK("fresh matching native path recovers without a spurious identity renewal", mesh_portal_round(&runtime,&plan,255000)==0 && runtime.selection.failed_probes==0);
	fake.neighbor=(struct mesh_neighbor){0};
	CHECK("a kernel-flushed owned ARP entry is not recreated during compensation", mesh_portal_runtime_stop(&runtime)==0 && !fake.neighbor.exists && runtime.phase==0);
	{
		uint8_t invalid[96]={0};
		int journal=openat(dir,"portal.meta",O_WRONLY|O_TRUNC|O_CLOEXEC|O_NOFOLLOW);
		memcpy(invalid,"M11PORT1",8);
		invalid[8]=2;
		invalid[15]=10;
		invalid[16]=(uint8_t)(config.config.portal_ipv4.address>>24);
		invalid[17]=(uint8_t)(config.config.portal_ipv4.address>>16);
		invalid[18]=(uint8_t)(config.config.portal_ipv4.address>>8);
		invalid[19]=(uint8_t)config.config.portal_ipv4.address;
		must(journal<0 || write(journal,invalid,sizeof(invalid))!=sizeof(invalid) ? -1 : 0);
		close(journal);
		CHECK("phase-two metadata without an owned permanent pin cannot authorize recovery writes", mesh_portal_runtime_open(&runtime,&peer,&io,10,20,config.config.portal_ipv4.address)!=0);
	}
	unlinkat(dir,"portal.meta",0);
	fake.neighbor=original;
	must(mesh_portal_runtime_open(&runtime,&peer,&io,10,20,config.config.portal_ipv4.address));
	hear(&runtime,&a,local,260);
	fake.probe_elapsed_ms=20000;
	CHECK("announcement expiring DURING a slow native probe cannot win afterward", mesh_portal_round(&runtime,&plan,260000)==0 && !runtime.selection.selected && runtime.phase==0);
	fake.probe_elapsed_ms=0;
	{
		struct mesh_portal_runtime unopened={0};
		CHECK("missing runtime ownership fails clearly without NULL dereference", mesh_portal_round(&unopened,&plan,300000)<0);
	}
	hear(&runtime,&a,local,300);
	must(mesh_portal_round(&runtime,&plan,300000) < 0 ? -1 : 0);
	{
		volatile sig_atomic_t cancel=0;
		unsigned int writes=fake.writes;
		unsigned int renews=fake.renews;
		peer.cancelled=&cancel;
		fake.cancel_after_probe=true;
		hear(&runtime,&a,local,305);
		CHECK("signal DURING a native probe prevents subsequent pin or renewal effects", mesh_portal_round(&runtime,&plan,305000)<0 && cancel && fake.writes==writes && fake.renews==renews);
		peer.cancelled=NULL;
		fake.cancel_after_probe=false;
	}
	hear(&runtime,&a,local,310);
	must(mesh_portal_round(&runtime,&plan,310000) < 0 ? -1 : 0);
	must(mesh_portal_runtime_stop(&runtime));
	{
		unsigned int writes=fake.writes;
		struct mesh_identity changed;
		must(mesh_identity_build("PUBLIC-TEST-ONLY-NEW-MESH-GROUP",plan.identity.factory_mac,plan.identity.bridge_mac,&changed));
		plan.identity=changed;
		CHECK("cleanly released foreign-group history is safely reset, not an autoconfig startup blocker", mesh_portal_runtime_open(&runtime,&peer,&io,10,20,config.config.portal_ipv4.address)==0 && !runtime.selection.selected && runtime.phase==0 && fake.writes==writes);
	}
	mesh_native_config_clear(&config);
	mesh_peer_close(&peer);
	close(dir);
	close(root);
	printf("%u portal-runtime checks; %u failures\n",checks,failures);
	return failures==0?0:1;
}
