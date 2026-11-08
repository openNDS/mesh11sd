/**
 * @file status.c
 * @brief Publish and print the read-only mesh11sd.v8 status object.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#define _POSIX_C_SOURCE 200809L
#include "status.h"
#include <errno.h>
#include <libubus.h>
#include <libubox/blobmsg_json.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mesh_status_service {
	struct ubus_context *bus;
	struct ubus_object object;
	struct ubus_object_type type;
	const struct mesh_public_status *status;
};
static const char *const phases[] = {"validating", "starting", "active", "withdrawing", "conflict"};
static bool valid(const struct mesh_public_status *status)
{
	return status != NULL && status->phase >= MESH_STATUS_VALIDATING && status->phase <= MESH_STATUS_CONFLICT &&
		   (status->role == MESH_ROLE_PEER || status->role == MESH_ROLE_MRP) &&
		   status->wan_health >= MESH_WAN_ABSENT && status->wan_health <= MESH_WAN_ERROR;
}
int mesh_status_payload(const struct mesh_public_status *status, struct mesh_runtime_data *out)
{
	struct mesh_message message;
	int failed;
	if (!valid(status) || out == NULL || mesh_message_init(&message) != 0) {
		return -1;
	}
	failed = blobmsg_add_string(&message.blob,"version","v8-experimental-scratch") ||
		blobmsg_add_string(&message.blob,"state",phases[status->phase]) ||
		blobmsg_add_string(&message.blob,"role",status->role == MESH_ROLE_MRP ? "mrp" : "peer") ||
		blobmsg_add_string(&message.blob,"startup_policy",status->automatic_at_boot ? "auto-at-boot" : "fixed-role") ||
		blobmsg_add_string(&message.blob,"wan_health",mesh_wan_health_name(status->wan_health)) ||
		blobmsg_add_u8(&message.blob,"wan_assessed",status->wan_assessed) ||
		blobmsg_add_u8(&message.blob,"mesh_active",status->mesh_active) ||
		blobmsg_add_u8(&message.blob,"ipv4_services_active",status->ipv4_services_active) ||
		blobmsg_add_u8(&message.blob,"portal_selected",status->portal_selected) ||
		blobmsg_add_u8(&message.blob,"internet_verified",false) ||
		blobmsg_add_u8(&message.blob,"loop_protection_active",false) ||
		blobmsg_add_u8(&message.blob,"bridged_client_portal_pinning",false) ||
		blobmsg_add_u8(&message.blob,"ipv6_ra_supported",false) ||
		blobmsg_add_u8(&message.blob,"hardware_validated",false);
	if (failed == 0) {
		failed = mesh_message_copy(&message,out);
	}
	mesh_message_clear(&message);
	return failed == 0 ? 0 : -1;
}
static int boolean(const struct mesh_runtime_data *data, const char *name, bool *out)
{
	struct blob_attr *field = mesh_root_field(data,name,BLOBMSG_TYPE_BOOL);
	if (field == NULL) {
		return -1;
	}
	*out = blobmsg_get_bool(field);
	return 0;
}
int mesh_status_decode(const struct mesh_runtime_data *data, struct mesh_public_status *out)
{
	struct mesh_public_status status = {0};
	struct blob_attr *field;
	bool value;
	size_t i;
	static const char *const unsupported[] = {"internet_verified", "loop_protection_active", "bridged_client_portal_pinning", "ipv6_ra_supported", "hardware_validated"};
	if (data == NULL || out == NULL || !mesh_reply_valid(data)) {
		return -1;
	}
	field = mesh_root_field(data,"version",BLOBMSG_TYPE_STRING);
	if (field == NULL || strcmp(blobmsg_get_string(field),"v8-experimental-scratch") != 0) {
		return -1;
	}
	field = mesh_root_field(data,"state",BLOBMSG_TYPE_STRING);
	if (field == NULL) {
		return -1;
	}
	for (i = 0; i < sizeof(phases)/sizeof(phases[0]); i++) {
		if (strcmp(blobmsg_get_string(field),phases[i]) == 0) {
			break;
		}
	}
	if (i == sizeof(phases)/sizeof(phases[0])) {
		return -1;
	}
	status.phase = (enum mesh_status_phase)i;
	field = mesh_root_field(data,"role",BLOBMSG_TYPE_STRING);
	if (field == NULL || (strcmp(blobmsg_get_string(field),"mrp") != 0 && strcmp(blobmsg_get_string(field),"peer") != 0)) {
		return -1;
	}
	status.role = strcmp(blobmsg_get_string(field),"mrp") == 0 ? MESH_ROLE_MRP : MESH_ROLE_PEER;
	field = mesh_root_field(data,"startup_policy",BLOBMSG_TYPE_STRING);
	if (field == NULL || (strcmp(blobmsg_get_string(field),"auto-at-boot") != 0 && strcmp(blobmsg_get_string(field),"fixed-role") != 0)) {
		return -1;
	}
	status.automatic_at_boot = strcmp(blobmsg_get_string(field),"auto-at-boot") == 0;
	field = mesh_root_field(data,"wan_health",BLOBMSG_TYPE_STRING);
	if (field == NULL) {
		return -1;
	}
	for (i = MESH_WAN_ABSENT; i <= MESH_WAN_ERROR; i++) {
		if (strcmp(blobmsg_get_string(field),mesh_wan_health_name((enum mesh_wan_health)i)) == 0) {
			break;
		}
	}
	if (i > MESH_WAN_ERROR) {
		return -1;
	}
	status.wan_health = (enum mesh_wan_health)i;
	if (boolean(data,"wan_assessed",&status.wan_assessed) != 0 ||
		boolean(data,"mesh_active",&status.mesh_active) != 0 ||
		boolean(data,"ipv4_services_active",&status.ipv4_services_active) != 0 ||
		boolean(data,"portal_selected",&status.portal_selected) != 0) {
		return -1;
	}
	for (i = 0; i < sizeof(unsupported)/sizeof(unsupported[0]); i++) {
		if (boolean(data,unsupported[i],&value) != 0 || value) {
			return -1;
		}
	}
	*out = status;
	return 0;
}
static int status_reply(struct ubus_context *bus, struct ubus_object *object, struct ubus_request_data *request,
						const char *method, struct blob_attr *arguments)
{
	struct mesh_status_service *service = container_of(object,struct mesh_status_service,object);
	struct mesh_runtime_data response = {0};
	int result;
	(void)method;
	(void)arguments;
	if (mesh_status_payload(service->status,&response) != 0) {
		return UBUS_STATUS_UNKNOWN_ERROR;
	}
	result = ubus_send_reply(bus,request,(struct blob_attr *)response.bytes);
	mesh_runtime_data_clear(&response);
	return result;
}
static const struct ubus_method methods[] = { UBUS_METHOD_NOARG("status",status_reply) };
static const struct ubus_object_type type = UBUS_OBJECT_TYPE("mesh11sd.v8",methods);
struct mesh_status_service *mesh_status_open(const struct mesh_public_status *status, const char *socket_path)
{
	struct mesh_status_service *service;
	if (!valid(status) || (service = calloc(1,sizeof(*service))) == NULL) {
		return NULL;
	}
	service->status = status;
	service->bus = ubus_connect(socket_path);
	service->object.name = "mesh11sd.v8";
	service->type = type;
	service->object.type = &service->type;
	service->object.methods = methods;
	service->object.n_methods = sizeof(methods)/sizeof(methods[0]);
	if (service->bus == NULL || ubus_add_object(service->bus,&service->object) != 0) {
		mesh_status_close(service);
		return NULL;
	}
	return service;
}
int mesh_status_dispatch(struct mesh_status_service *service, unsigned int milliseconds)
{
	struct pollfd event;
	int result;
	if (service == NULL || service->bus == NULL || service->bus->sock.eof || milliseconds > 1000) {
		return -1;
	}
	event = (struct pollfd){ .fd = service->bus->sock.fd, .events = POLLIN };
	result = poll(&event,1,(int)milliseconds);
	if (result < 0) {
		return errno == EINTR ? 0 : -1;
	}
	if ((event.revents & (POLLERR|POLLHUP|POLLNVAL)) != 0) {
		return -1;
	}
	if ((event.revents & POLLIN) != 0) {
		ubus_handle_event(service->bus);
	}
	return service->bus->sock.eof ? -1 : 0;
}
void mesh_status_close(struct mesh_status_service *service)
{
	if (service == NULL) {
		return;
	}
	if (service->bus != NULL) {
		/* Closing this connection releases only its own object. */
		ubus_free(service->bus);
	}
	free(service);
}
struct query_result {
	struct mesh_public_status status;
	bool received;
};
static void query_reply(struct ubus_request *request, int type_id, struct blob_attr *message)
{
	struct query_result *result = request->priv;
	struct mesh_runtime_data reply = {0};
	(void)type_id;
	if (message != NULL && blob_raw_len(message) <= 4096) {
		reply.length = blob_raw_len(message);
		memcpy(reply.bytes,message,reply.length);
		if (mesh_status_decode(&reply,&result->status) == 0) {
			result->received = true;
		}
	}
	mesh_runtime_data_clear(&reply);
}
int mesh_status_query(const char *socket_path, struct mesh_public_status *status)
{
	struct ubus_context *bus;
	struct query_result result = {0};
	struct blob_buf arguments = {0};
	uint32_t id;
	int outcome = -1;
	if (status == NULL || (bus = ubus_connect(socket_path)) == NULL) {
		return -1;
	}
	blob_buf_init(&arguments,0);
	if (ubus_lookup_id(bus,"mesh11sd.v8",&id) == 0 &&
		ubus_invoke(bus,id,"status",arguments.head,query_reply,&result,2000) == 0 && result.received) {
		*status = result.status;
		outcome = 0;
	}
	blob_buf_free(&arguments);
	ubus_free(bus);
	return outcome;
}
int mesh_status_print(const struct mesh_public_status *status)
{
	struct mesh_runtime_data payload = {0};
	char *json;
	int result;
	if (mesh_status_payload(status,&payload) != 0) {
		return -1;
	}
	json = blobmsg_format_json((struct blob_attr *)payload.bytes,true);
	mesh_runtime_data_clear(&payload);
	if (json == NULL) {
		return -1;
	}
	result = puts(json);
	free(json);
	return result < 0 ? -1 : 0;
}
