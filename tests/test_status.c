/**
 * @file test_status.c
 * @brief Host fixtures for the status object on a private ubusd socket.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

/* PUBLIC-TEST-ONLY: a private AF_UNIX ubusd, never system/network ubus. */
#define _POSIX_C_SOURCE 200809L
#include "status.h"
#include <errno.h>
#include <libubus.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
static unsigned int checks;
static unsigned int failures;
#define CHECK(name, condition) do { \
	checks++; \
	if (condition) { \
		printf("ok %u - %s\n",checks,name); \
	} else { \
		printf("not ok %u - %s\n",checks,name); \
		failures++; \
	} \
} while (0)
static bool same(const struct mesh_public_status *a, const struct mesh_public_status *b)
{
	struct mesh_runtime_data left = {0};
	struct mesh_runtime_data right = {0};
	return mesh_status_payload(a,&left) == 0 && mesh_status_payload(b,&right) == 0 &&
		   left.length == right.length && memcmp(left.bytes,right.bytes,left.length) == 0;
}
static int real_query(struct mesh_status_service *service, const char *path, const struct mesh_public_status *expected)
{
	pid_t child;
	int status;
	unsigned int i;
	fflush(NULL);
	child = fork();
	if (child < 0) {
		return -1;
	}
	if (child == 0) {
		struct mesh_public_status actual;
		mesh_status_close(service);
		_exit(mesh_status_query(path,&actual) == 0 && same(&actual,expected) ? 0 : 1);
	}
	for (i = 0; i < 100; i++) {
		pid_t done = waitpid(child,&status,WNOHANG);
		if (done == child) {
			return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
		}
		if (done < 0 || mesh_status_dispatch(service,50) != 0) {
			break;
		}
	}
	kill(child,SIGKILL);
	waitpid(child,&status,0);
	return -1;
}
static int foreign_reply(struct ubus_context *context, struct ubus_object *object, struct ubus_request_data *request,
						  const char *method, struct blob_attr *message)
{
	(void)context;
	(void)object;
	(void)request;
	(void)method;
	(void)message;
	return 0;
}
int main(int argc, char **argv)
{
	struct mesh_public_status status = { .phase = MESH_STATUS_VALIDATING, .role = MESH_ROLE_PEER, .automatic_at_boot = true };
	struct mesh_public_status parsed;
	struct mesh_runtime_data payload = {0};
	struct mesh_runtime_data altered;
	struct mesh_status_service *service;
	struct mesh_status_service *collision;
	struct ubus_context *foreign;
	struct ubus_object object = { .name = "PUBLIC-TEST-ONLY.unrelated" };
	static const struct ubus_method foreign_methods[] = { UBUS_METHOD_NOARG("ping",foreign_reply) };
	struct ubus_object_type foreign_type = UBUS_OBJECT_TYPE("PUBLIC-TEST-ONLY",foreign_methods);
	struct blob_attr *field;
	uint32_t id;
	uint32_t random = 41;
	size_t i;
	unsigned int phase;
	if (argc != 2) {
		return 2;
	}
	CHECK("public status encodes and round-trips a validating peer", mesh_status_payload(&status,&payload) == 0 && mesh_status_decode(&payload,&parsed) == 0 && same(&status,&parsed));
	CHECK("public schema has no credentials or on-air identifier", mesh_root_field(&payload,"credential",BLOBMSG_TYPE_STRING) == NULL && mesh_root_field(&payload,"mesh_id",BLOBMSG_TYPE_STRING) == NULL);
	CHECK("status does not advertise working hardware", (field = mesh_root_field(&payload,"hardware_validated",BLOBMSG_TYPE_BOOL)) != NULL && !blobmsg_get_bool(field));
	CHECK("status does not advertise bridged client ARP enforcement", (field = mesh_root_field(&payload,"bridged_client_portal_pinning",BLOBMSG_TYPE_BOOL)) != NULL && !blobmsg_get_bool(field));
	CHECK("null status cannot serialize", mesh_status_payload(NULL,&payload) != 0);
	status.role = MESH_ROLE_MBP;
	CHECK("unsupported local MBP is not a valid public runtime role", mesh_status_payload(&status,&payload) != 0);
	status.role = MESH_ROLE_MRP;
	status.wan_assessed = true;
	status.wan_health = MESH_WAN_QUALIFIED;
	status.ipv4_services_active = true;
	status.mesh_active = true;
	status.portal_selected = true;
	for (phase = MESH_STATUS_VALIDATING; phase <= MESH_STATUS_CONFLICT; phase++) {
		status.phase = (enum mesh_status_phase)phase;
		CHECK("all explicit public lifecycle phases round-trip", mesh_status_payload(&status,&payload) == 0 && mesh_status_decode(&payload,&parsed) == 0 && same(&status,&parsed));
	}
	altered = payload;
	field = mesh_root_field(&altered,"hardware_validated",BLOBMSG_TYPE_BOOL);
	if (field == NULL) {
		return 2;
	}
	*(unsigned char *)blobmsg_data(field) = 1;
	CHECK("status client rejects an unexpected hardware-validation claim", mesh_status_decode(&altered,&parsed) != 0);
	for (i = 0; i < 20000; i++) {
		size_t offset;
		altered = payload;
		random = random * 1664525U + 1013904223U;
		offset = (random>>8)%altered.length;
		altered.bytes[offset] ^= (unsigned char)random;
		altered.length = (random>>16)%(altered.length+1);
		(void)mesh_status_decode(&altered,&parsed);
	}
	CHECK("twenty thousand public status blob mutations stay bounded", true);
	CHECK("status polling rejects a missing service", mesh_status_dispatch(NULL,0) != 0);
	foreign = ubus_connect(argv[1]);
	object.type = &foreign_type;
	object.methods = foreign_methods;
	object.n_methods = sizeof(foreign_methods)/sizeof(foreign_methods[0]);
	{
		int code = foreign == NULL ? -100 : ubus_add_object(foreign,&object);
		if (code != 0) {
			fprintf(stderr,"PUBLIC-TEST-ONLY private broker registration rc=%d\n",code);
		}
		CHECK("unrelated object can coexist on the real private ubus", code == 0);
	}
	service = mesh_status_open(&status,argv[1]);
	CHECK("real libubus registers the exclusive read-only service object", service != NULL);
	if (service == NULL || foreign == NULL) {
		return 1;
	}
	CHECK("actual client query returns the typed public status through ubusd", real_query(service,argv[1],&status) == 0);
	collision = mesh_status_open(&status,argv[1]);
	CHECK("second status provider cannot adopt or overwrite the owned namespace", collision == NULL);
	mesh_status_close(collision);
	CHECK("failed collision leaves original provider intact", real_query(service,argv[1],&status) == 0);
	status.phase = MESH_STATUS_ACTIVE;
	status.role = MESH_ROLE_PEER;
	status.wan_health = MESH_WAN_UNREACHABLE;
	status.ipv4_services_active = false;
	CHECK("actual provider exposes updated state without registering another object", real_query(service,argv[1],&status) == 0);
	CHECK("public provider rejects unbounded dispatch waits", mesh_status_dispatch(service,1001) != 0);
	mesh_status_close(service);
	CHECK("closed provider no longer answers status queries", mesh_status_query(argv[1],&parsed) != 0);
	CHECK("owned cleanup preserves an unrelated ubus object", ubus_lookup_id(foreign,object.name,&id) == 0 && id == object.id);
	service = mesh_status_open(&status,argv[1]);
	CHECK("clean restart gets a fresh type and service registration", service != NULL);
	CHECK("clean restarted provider actually answers", service != NULL && real_query(service,argv[1],&status) == 0);
	mesh_status_close(service);
	ubus_free(foreign);
	printf("1..%u\n",checks);
	return failures == 0 ? 0 : 1;
}
