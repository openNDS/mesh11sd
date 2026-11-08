/**
 * @file test_wan.c
 * @brief Host fixtures for committed WAN parsing and live DNS qualification.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#define MESH_SERVICES_FIXTURE_ONLY
#include "test_services.c"
#include "wan.h"
#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

static const char wan_network[] =
	"# PUBLIC-TEST-ONLY network evidence\nconfig interface 'lan'\n option device 'br-lan'\n option proto 'static'\n"
	"config interface 'wan'\n option device 'wan'\n option proto 'dhcp'\n";
struct wan_fake {
	struct service_fake base;
	int root;
	int probe_result;
	unsigned int probes;
	bool down;
	bool pending;
	bool wrong_device;
	bool wrong_l3;
	bool no_default;
	bool no_dns;
	bool local_dns;
	bool malformed_dns;
	bool ipv6_dns;
	bool extra_dns;
	bool source_overlap;
	bool malformed;
	bool mutate;
};
static int wan_call(void *context, const char *object, const char *method,
					struct blob_attr *request, struct mesh_runtime_data *reply)
{
	struct wan_fake *fake=context;
	struct mesh_message message;
	void *array;
	void *table;
	char local[32];
	if (strcmp(object,"network.interface.wan")!=0 || strcmp(method,"status")!=0) {
		return service_call(context,object,method,request,reply);
	}
	must(mesh_message_init(&message));
	must(blobmsg_add_u8(&message.blob,"up",!fake->down));
	must(blobmsg_add_u8(&message.blob,"pending",fake->pending));
	must(blobmsg_add_string(&message.blob,"proto","dhcp"));
	must(blobmsg_add_string(&message.blob,"device",fake->wrong_device?"br-lan":"wan"));
	must(blobmsg_add_string(&message.blob,"l3_device",fake->wrong_l3?"otherwan":"wan"));
	array=blobmsg_open_array(&message.blob,"ipv4-address");
	table=blobmsg_open_table(&message.blob,NULL);
	if (fake->source_overlap) {
		struct in_addr address={.s_addr=htonl(fake->base.peer.config->config.portal_ipv4.address)};
		must(inet_ntop(AF_INET,&address,local,sizeof(local))==NULL?-1:0);
	}
	must(blobmsg_add_string(&message.blob,"address",fake->source_overlap?local:"198.51.100.2"));
	if (fake->malformed) {
		must(blobmsg_add_string(&message.blob,"mask","not-an-integer"));
	} else {
		must(blobmsg_add_u32(&message.blob,"mask",24));
	}
	blobmsg_close_table(&message.blob,table);
	blobmsg_close_array(&message.blob,array);
	array=blobmsg_open_array(&message.blob,"route");
	if (!fake->no_default) {
		table=blobmsg_open_table(&message.blob,NULL);
		must(blobmsg_add_string(&message.blob,"target","0.0.0.0"));
		must(blobmsg_add_u32(&message.blob,"mask",0));
		blobmsg_close_table(&message.blob,table);
	}
	blobmsg_close_array(&message.blob,array);
	array=blobmsg_open_array(&message.blob,"dns-server");
	if (!fake->no_dns) {
		must(blobmsg_add_string(&message.blob,NULL,fake->local_dns ? "127.0.0.1" : fake->malformed_dns ? "not-a-DNS-address" : fake->ipv6_dns ? "2001:db8::53" : "198.51.100.53"));
		if (fake->extra_dns) {
			must(blobmsg_add_string(&message.blob,NULL,"198.51.100.54"));
		}
	}
	blobmsg_close_array(&message.blob,array);
	must(mesh_message_copy(&message,reply));
	mesh_message_clear(&message);
	return 0;
}
static int qualified_probe(void *context, const struct mesh_wan_evidence *evidence, uint32_t server)
{
	struct wan_fake *fake=context;
	fake->probes++;
	CHECK("injected native probe receives ONLY committed WAN device and WAN source, never mesh source",
		  strcmp(evidence->device,"wan")==0 && evidence->source==0xc6336402 && evidence->prefix==24);
	CHECK("no arbitrary default target: probe target comes ONLY from configured-WAN DNS evidence",
		  server==0xc6336435 || server==0xc6336436);
	if (fake->mutate) {
		text(fake->root,"etc/config/network","# PUBLIC-TEST-ONLY concurrent committed change\n");
	}
	return fake->probe_result;
}
static void codec_tests(void)
{
	unsigned char packet[4096]={0};
	unsigned char good[4096];
	size_t length;
	size_t i;
	uint32_t random=0x12345678;
	CHECK("actual configured-resolver query is root NS IN with a correlated identifier", mesh_wan_dns_request(packet,sizeof(packet),&length,0x1234)==0 && length==17 && packet[0]==0x12 && packet[1]==0x34 && packet[12]==0 && packet[14]==2 && packet[16]==1);
	packet[2]=0x81;
	packet[3]=0x80;
	packet[7]=1;
	packet[17]=0xc0;
	packet[18]=12;
	packet[20]=2;
	packet[22]=1;
	packet[26]=60;
	packet[28]=4;
	packet[29]=2;
	packet[30]='n';
	packet[31]='s';
	packet[32]=0;
	CHECK("bounded DNS parser accepts a syntactically valid correlated root NS response", mesh_wan_dns_response(packet,33,0x1234)==0);
	CHECK("wrong DNS transaction cannot qualify WAN", mesh_wan_dns_response(packet,33,0x1235)!=0);
	memcpy(good,packet,sizeof(packet));
	packet[3]|=2;
	CHECK("SERVFAIL is not a successful WAN-path probe", mesh_wan_dns_response(packet,33,0x1234)!=0);
	memcpy(packet,good,sizeof(packet));
	packet[2]|=2;
	CHECK("truncated DNS over UDP cannot qualify WAN", mesh_wan_dns_response(packet,33,0x1234)!=0);
	memcpy(packet,good,sizeof(packet));
	packet[14]=1;
	CHECK("answer to a different query cannot qualify WAN", mesh_wan_dns_response(packet,33,0x1234)!=0);
	memcpy(packet,good,sizeof(packet));
	packet[28]=2;
	packet[29]=0xc0;
	packet[30]=29;
	CHECK("self-referential compressed DNS names fail boundedly", mesh_wan_dns_response(packet,31,0x1234)!=0);
	for (i=0;i<33;i++) {
		if (mesh_wan_dns_response(good,i,0x1234)==0) {
			break;
		}
	}
	CHECK("every truncated positive DNS fixture is rejected", i==33);
	for (i=0;i<20000;i++) {
		memcpy(packet,good,33);
		random=random*1664525U+1013904223U;
		packet[random%33]^=(unsigned char)(random>>24);
		mesh_wan_dns_response(packet,(random>>8)%34,0x1234);
	}
	CHECK("20000 bounded malformed DNS response mutations complete",true);
	{
		struct mesh_wan_evidence invalid = { .prefix = 24, .dns_count = 1, .dns = {0xc6336435} };
		memset(invalid.device,'x',sizeof(invalid.device));
		CHECK("unterminated native device input fails before any interface/socket lookup", mesh_wan_native_probe(NULL,&invalid,0xc6336435)<0);
	}
	{
		union { struct nlmsghdr align; unsigned char bytes[128]; } request;
		struct rtmsg *route;
		CHECK("actual route GET requests bind destination, source and output interface", mesh_wan_route_request(request.bytes,sizeof(request),&length,9,0xc6336402,0xc6336435)==0 && request.align.nlmsg_type==RTM_GETROUTE && request.align.nlmsg_flags==NLM_F_REQUEST);
		route=NLMSG_DATA(&request.align);
		route->rtm_type=RTN_UNICAST;
		CHECK("native route response accepts only exact requested WAN/source/target", mesh_wan_route_decode(route,length-NLMSG_HDRLEN,9,0xc6336402,0xc6336435)==0);
		CHECK("route through another portal/interface is NOT own-WAN health", mesh_wan_route_decode(route,length-NLMSG_HDRLEN,10,0xc6336402,0xc6336435)!=0);
		CHECK("route with a different source cannot qualify WAN", mesh_wan_route_decode(route,length-NLMSG_HDRLEN,9,0xc6336403,0xc6336435)!=0);
		CHECK("route to a different DNS target cannot qualify WAN", mesh_wan_route_decode(route,length-NLMSG_HDRLEN,9,0xc6336402,0xc6336436)!=0);
		route->rtm_type=RTN_BLACKHOLE;
		CHECK("blackhole is not a healthy route merely because WAN is UP", mesh_wan_route_decode(route,length-NLMSG_HDRLEN,9,0xc6336402,0xc6336435)!=0);
		route->rtm_type=RTN_UNICAST;
		CHECK("unaligned native route data is rejected before typed access", mesh_wan_route_decode((unsigned char *)route+1,length-NLMSG_HDRLEN-1,9,0xc6336402,0xc6336435)!=0);
		CHECK("truncated native route evidence fails", mesh_wan_route_decode(route,length-NLMSG_HDRLEN-1,9,0xc6336402,0xc6336435)!=0);
		CHECK("invalid route inputs fail before any kernel query", mesh_wan_route_request(request.bytes,sizeof(request),&length,0,0xc6336402,0xc6336435)!=0);
	}
}
int main(int argc,char **argv)
{
	struct mesh_native_config config={0};
	struct mesh_facts facts;
	struct mesh_plan plan;
	struct mesh_peer peer;
	struct wan_fake fake={.base={.peer.config=&config,.dns_pid=100,.foreign_pid=999,.lan_up=true}};
	struct mesh_peer_transport transport_io=transport(&fake.base.peer);
	struct mesh_discovery_ops discovery={.context=&peer,.radio_config=mesh_peer_radio,.bridge=bridge,.role=role};
	struct mesh_wan_io io={.context=&fake,.probe=qualified_probe};
	struct mesh_wan_result result;
	unsigned int before;
	int root;
	int dir;
	if (argc!=2) {
		return 2;
	}
	setvbuf(stdout,NULL,_IONBF,0);
	{
		enum mesh_role selected;
		enum mesh_role another;
		enum mesh_wan_health health;
		for (health = MESH_WAN_ABSENT; health < MESH_WAN_QUALIFIED; health++) {
			CHECK("automatic boot with absent/partial/unreachable own WAN chooses only peer", mesh_wan_boot_role(health,true,false,&selected)==0 && selected==MESH_ROLE_PEER);
		}
		CHECK("automatic boot may independently qualify concurrent local MRPs", mesh_wan_boot_role(MESH_WAN_QUALIFIED,true,false,&selected)==0 && mesh_wan_boot_role(MESH_WAN_QUALIFIED,true,false,&another)==0 && selected==MESH_ROLE_MRP && another==MESH_ROLE_MRP);
		CHECK("malformed WAN evidence cannot silently choose an automatic role", mesh_wan_boot_role(MESH_WAN_ERROR,true,false,&selected)!=0);
		CHECK("explicit safe peer override requires no guessed WAN classification", mesh_wan_boot_role(MESH_WAN_ERROR,false,false,&selected)==0 && selected==MESH_ROLE_PEER);
		CHECK("explicit MRP does not override failed own-WAN qualification", mesh_wan_boot_role(MESH_WAN_UNREACHABLE,false,true,&selected)!=0);
		CHECK("conflicting startup switches fail explicitly", mesh_wan_boot_role(MESH_WAN_QUALIFIED,true,true,&selected)!=0);
		CHECK("active peer never speculatively promotes because a probe later becomes healthy", mesh_wan_role_guard(MESH_ROLE_PEER,MESH_WAN_QUALIFIED)==0);
		CHECK("active qualified MRP can continue without a role flip or renewal", mesh_wan_role_guard(MESH_ROLE_MRP,MESH_WAN_QUALIFIED)==0);
		CHECK("own-WAN loss requires ordered MRP withdrawal, not an unqualified portal advertisement", mesh_wan_role_guard(MESH_ROLE_MRP,MESH_WAN_UNREACHABLE)==1);
		CHECK("unsupported MBP has no fabricated WAN transition behavior", mesh_wan_role_guard(MESH_ROLE_MBP,MESH_WAN_QUALIFIED)<0);
		CHECK("committed forced MRP override is never silently converted to peer", mesh_wan_configured_role(0,MESH_ROLE_PEER)<0 && mesh_wan_configured_role(0,MESH_ROLE_MRP)==0);
		CHECK("existing automatic planner modes accept only qualified role evidence", mesh_wan_configured_role(1,MESH_ROLE_PEER)==0 && mesh_wan_configured_role(2,MESH_ROLE_MRP)==0);
		CHECK("unsupported committed MBP role modes fail before owned network setup", mesh_wan_configured_role(4,MESH_ROLE_PEER)<0);
	}
	codec_tests();
	root=open(argv[1],O_RDONLY|O_DIRECTORY|O_CLOEXEC);
	if (root<0) {
		return 2;
	}
	services_fixture(root);
	fake.root=root;
	text(root,"etc/config/network",wan_network);
	transport_io.invoke=wan_call;
	dir=directory(root,"wan-state");
	must(mesh_peer_open(&peer,dir,&transport_io));
	must(mesh_native_plan(root,&discovery,&config,&facts,&plan)==MESH_OK?0:-1);
	CHECK("healthy committed direct WAN plus qualified controlled probe is eligible MRP evidence", mesh_wan_assess(root,&peer,&config,&io,&result)==0 && result.health==MESH_WAN_QUALIFIED);
	fake.down=true;
	before=fake.probes;
	CHECK("WAN down selects safe peer posture without probing arbitrary targets", mesh_wan_assess(root,&peer,&config,&io,&result)==0 && result.health==MESH_WAN_ABSENT && fake.probes==before);
	fake.down=false;
	fake.pending=true;
	CHECK("UP but pending is partial evidence, never a local portal", mesh_wan_assess(root,&peer,&config,&io,&result)==0 && result.health==MESH_WAN_PARTIAL && fake.probes==before);
	fake.pending=false;
	fake.no_default=true;
	CHECK("WAN address without own default-route evidence stays peer", mesh_wan_assess(root,&peer,&config,&io,&result)==0 && result.health==MESH_WAN_PARTIAL && fake.probes==before);
	fake.no_default=false;
	fake.no_dns=true;
	CHECK("no configured WAN DNS means no invented public probe endpoint", mesh_wan_assess(root,&peer,&config,&io,&result)==0 && result.health==MESH_WAN_PARTIAL && fake.probes==before);
	fake.no_dns=false;
	fake.local_dns=true;
	CHECK("local loopback DNS cannot falsely prove own-WAN health", mesh_wan_assess(root,&peer,&config,&io,&result)==0 && result.health==MESH_WAN_PARTIAL && fake.probes==before);
	fake.local_dns=false;
	fake.malformed_dns=true;
	CHECK("malformed DNS text is not silently treated as unsupported IPv6", mesh_wan_assess(root,&peer,&config,&io,&result)<0 && result.health==MESH_WAN_ERROR && fake.probes==before);
	fake.malformed_dns=false;
	fake.ipv6_dns=true;
	CHECK("IPv6-only WAN DNS is explicitly partial, never a guessed IPv4 portal", mesh_wan_assess(root,&peer,&config,&io,&result)==0 && result.health==MESH_WAN_PARTIAL && fake.probes==before);
	fake.ipv6_dns=false;
	fake.wrong_device=true;
	CHECK("foreign current device cannot override committed WAN ownership", mesh_wan_assess(root,&peer,&config,&io,&result)==0 && result.health==MESH_WAN_PARTIAL && fake.probes==before);
	fake.wrong_device=false;
	fake.wrong_l3=true;
	CHECK("unsupported PPP/other L3 mapping stays explicitly partial", mesh_wan_assess(root,&peer,&config,&io,&result)==0 && result.health==MESH_WAN_PARTIAL && fake.probes==before);
	fake.wrong_l3=false;
	fake.source_overlap=true;
	CHECK("mesh/LAN source prefix cannot masquerade as WAN", mesh_wan_assess(root,&peer,&config,&io,&result)==0 && result.health==MESH_WAN_PARTIAL && fake.probes==before);
	fake.source_overlap=false;
	fake.probe_result=1;
	CHECK("interface UP is insufficient when controlled configured-DNS probe fails", mesh_wan_assess(root,&peer,&config,&io,&result)==0 && result.health==MESH_WAN_UNREACHABLE);
	fake.extra_dns=true;
	before=fake.probes;
	CHECK("all supplied DNS candidates are boundedly tried, no arbitrary fallback", mesh_wan_assess(root,&peer,&config,&io,&result)==0 && result.health==MESH_WAN_UNREACHABLE && fake.probes==before+2);
	fake.extra_dns=false;
	fake.probe_result=-1;
	CHECK("malformed native route/system evidence is explicit failure, not fabricated healthy WAN", mesh_wan_assess(root,&peer,&config,&io,&result)<0 && result.health==MESH_WAN_ERROR);
	fake.probe_result=0;
	CHECK("restored confirmed own-WAN evidence can qualify again independently", mesh_wan_assess(root,&peer,&config,&io,&result)==0 && result.health==MESH_WAN_QUALIFIED);
	fake.down=true;
	CHECK("flap DOWN is observed without retaining stale healthy role evidence", mesh_wan_assess(root,&peer,&config,&io,&result)==0 && result.health==MESH_WAN_ABSENT);
	fake.down=false;
	CHECK("reentry requires a new complete controlled qualification", mesh_wan_assess(root,&peer,&config,&io,&result)==0 && result.health==MESH_WAN_QUALIFIED);
	fake.malformed=true;
	CHECK("malformed netifd address evidence is explicit failure", mesh_wan_assess(root,&peer,&config,&io,&result)<0 && result.health==MESH_WAN_ERROR);
	fake.malformed=false;
	fake.mutate=true;
	CHECK("committed mapping changed during probe cannot be applied as qualified", mesh_wan_assess(root,&peer,&config,&io,&result)<0 && result.health==MESH_WAN_ERROR);
	fake.mutate=false;
	text(root,"etc/config/network","# PUBLIC-TEST-ONLY absent WAN\nconfig interface 'lan'\n option device 'br-lan'\n option proto 'static'\n");
	mesh_native_config_clear(&config);
	must(mesh_native_plan(root,&discovery,&config,&facts,&plan)==MESH_OK?0:-1);
	before=fake.probes;
	CHECK("boot without committed WAN is a peer, not a fabricated local portal", mesh_wan_assess(root,&peer,&config,&io,&result)==0 && result.health==MESH_WAN_ABSENT && fake.probes==before);
	CHECK("public health status explicitly avoids claiming Internet verification", strcmp(mesh_wan_health_name(MESH_WAN_QUALIFIED),"configured_dns_qualified")==0);
	{
		static const char *const endings[] = {
			" option disabled '1'\n", " option auto '0'\n", " option proto 'pppoe'\n",
			" list disabled '1'\n", " option auto 'invalid'\n"
		};
		static const enum mesh_wan_health expected[] = {
			MESH_WAN_ABSENT, MESH_WAN_ABSENT, MESH_WAN_PARTIAL, MESH_WAN_ERROR, MESH_WAN_ERROR
		};
		char committed_text[1024];
		size_t n;
		for (n = 0; n < sizeof(endings)/sizeof(endings[0]); n++) {
			snprintf(committed_text,sizeof(committed_text),"%s%s",wan_network,endings[n]);
			text(root,"etc/config/network",committed_text);
			mesh_native_config_clear(&config);
			must(mesh_native_plan(root,&discovery,&config,&facts,&plan)==MESH_OK?0:-1);
			before = fake.probes;
			(void)mesh_wan_assess(root,&peer,&config,&io,&result);
			CHECK("committed disable/unsupported/malformed mapping never probes or invents a portal", result.health == expected[n] && fake.probes == before);
		}
	}
	mesh_native_config_clear(&config);
	mesh_peer_close(&peer);
	close(dir);
	close(root);
	printf("%u WAN checks; %u failures\n",checks,failures);
	return failures==0?0:1;
}
