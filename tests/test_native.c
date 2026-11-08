/**
 * @file test_native.c
 * @brief Host fixtures for committed UCI import and runtime payload records.
 * @author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>
 */

#define _GNU_SOURCE
#include "native.h"
#include "runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libubox/blobmsg.h>

static unsigned int checks;
static unsigned int failures;
#define CHECK(name, expression) do { \
	checks++; \
	if (!(expression)) { \
		printf("not ok %u - %s\n", checks, name); \
		failures++; \
	} else { \
		printf("ok %u - %s\n", checks, name); \
	} \
} while (0)

/* Only public synthetic test values and their derived hashes appear here. */
static const char setup[] =
	"config mesh11sd 'setup'\n"
	" option auto_config '1'\n"
	" option auto_mesh_id '--__'\n"
	" option auto_mesh_key 'TEST-ONLY-$(not-a-command);nonsecret'\n"
	" option auto_mesh_band '2g'\n"
	" option mesh_node_mobility_level '2'\n";
static const char network[] =
	"config interface 'lan'\n option device 'br-lan'\n option proto 'static'\n"
	" option ipaddr '192.0.2.1'\n option netmask '255.255.255.0'\n"
	"config interface 'wan'\n option device 'wan'\n option proto 'dhcp'\n";
static const char wireless[] =
	"config wifi-device 'radio0'\n option type 'mac80211'\n option phy 'phy0'\n"
	" option band '2g'\n option channel '1'\n option htmode 'HT20'\n option country 'GB'\n";
static const char dhcp[] =
	"config dhcp 'lan'\n option interface 'lan'\n option start '100'\n"
	" option limit '150'\n option leasetime '1h'\n";

static void directories(int root, const char *path)
{
	char copy[512];
	size_t i;

	if (strlen(path) >= sizeof(copy)) {
		abort();
	}
	memcpy(copy, path, strlen(path) + 1);
	for (i = 0; copy[i] != '\0'; i++) {
		if (copy[i] == '/') {
			copy[i] = '\0';
			if (mkdirat(root, copy, 0700) < 0 && errno != EEXIST) {
				abort();
			}
			copy[i] = '/';
		}
	}
}

static void put(int root, const char *path, const void *data, size_t length)
{
	int fd;

	directories(root, path);
	fd = openat(root, path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0 || write(fd, data, length) != (ssize_t)length || close(fd) != 0) {
		abort();
	}
}

static void text(int root, const char *path, const char *data)
{
	put(root, path, data, strlen(data));
}

static void fixture(int root)
{
	text(root, "etc/config/mesh11sd", setup);
	text(root, "etc/config/network", network);
	text(root, "etc/config/wireless", wireless);
	text(root, "etc/config/dhcp", dhcp);
	text(root, "etc/config/system", "config system\n option label_macaddr '00:11:22:33:44:55'\n");
	text(root, "etc/board.json", "{\"model\":{\"label_macaddr\":\"00:11:22:33:44:99\"},\"system\":{\"label_macaddr\":\"00:11:22:33:44:10\"}}");
	text(root, "sys/class/net/br-lan/bridge/stp_state", "0\n");
	text(root, "sys/class/net/eth0/ifindex", "2\n");
	text(root, "sys/class/net/eth0/type", "1\n");
	text(root, "sys/class/net/eth0/address", "00:11:22:33:44:02\n");
	text(root, "sys/class/net/eth0/addr_assign_type", "0\n");
}

struct probes {
	int root;
	bool change_on_radio;
	bool fail_bridge;
	enum mesh_role role;
};

static enum mesh_error radio_probe(void *context, uint32_t index, uint32_t frequency,
								   struct mesh_radio *radio)
{
	struct probes *probes;

	probes = context;
	if (index != 0 || frequency != 2412) {
		return MESH_E_RADIO;
	}
	memset(radio, 0, sizeof(*radio));
	memcpy(radio->phy, "phy0", 5);
	radio->mesh_supported = true;
	radio->frequency_supported = true;
	radio->combination_verified = true;
	if (probes->change_on_radio) {
		text(probes->root, "etc/config/dhcp", "config dhcp 'lan'\n option start '101'\n option limit '140'\n");
	}
	return MESH_OK;
}

static enum mesh_error bridge_probe(void *context, const char *name, uint8_t mac[6])
{
	struct probes *probes;

	probes = context;
	if (probes->fail_bridge || strcmp(name, "br-lan") != 0) {
		return MESH_E_PLATFORM;
	}
	(void)mesh_mac_parse("02:11:22:33:44:66", mac);
	return MESH_OK;
}

static enum mesh_error role_probe(void *context, const char *wan, enum mesh_role *role)
{
	struct probes *probes;

	probes = context;
	if (strcmp(wan, "wan") != 0) {
		return MESH_E_INPUT;
	}
	*role = probes->role;
	return MESH_OK;
}

static void test_inputs(int root)
{
	struct mesh_native_config config = {0};
	struct mesh_native_config second = {0};
	struct mesh_factory_input identity;
	struct mesh_factory_result chosen;
	struct mesh_facts facts = {0};
	struct mesh_plan plan = {0};
	struct mesh_plan again = {0};
	struct mesh_plan sentinel;
	struct probes probes = { .root = root, .role = MESH_ROLE_PEER };
	struct mesh_discovery_ops ops = { .context = &probes, .radio = radio_probe,
									 .bridge = bridge_probe, .role = role_probe };
	uint8_t expected[6];
	char buffer[64];
	size_t length;
	struct mesh_file_stamp stamp;
	struct mesh_mobility mobility;

	CHECK("committed UCI snapshots parsed natively", mesh_native_config_read(root, &config) == MESH_OK);
	CHECK("configuration pointers rebound to returned storage", config.config.auto_mesh_id == config.group &&
		  config.config.mesh_id == config.on_air_id && config.config.preferred_phy == config.phy);
	CHECK("legacy on-air ID derived without emitting group/key", strlen(config.on_air_id) == 30 && strlen(config.credential) == 64);
	CHECK("public test group gives exact legacy on-air vector", strcmp(config.on_air_id, "92d490daf46cfe534c56ddd669297e") == 0);
	CHECK("public TEST-ONLY key gives exact legacy derivation vector", strcmp(config.credential, "de6b4d7781124acbcfabec63cbf070a1fd4b8dbe2da64341be65b86f2f757f88") == 0);
	CHECK("configured radio channel htmode and country preserved", config.config.frequency_mhz == 2412 &&
		  strcmp(config.phy, "phy0") == 0 && strcmp(config.htmode, "HT20") == 0 && strcmp(config.country, "GB") == 0);
	CHECK("legacy interface naming retained", strcmp(config.mesh_ifname, "m-11s-r0p0") == 0);
	CHECK("native pool and lease decoded", config.config.portal_ipv4.pool_last - config.config.portal_ipv4.pool_first == 149 &&
		  config.config.portal_ipv4.lease_seconds == 3600);
	CHECK("common derived IPv4 is private deterministic /24", (config.config.portal_ipv4.address & 0xffff00ffU) == 0xc0a80001U);
	text(root, "tmp/.uci/mesh11sd", "mesh11sd.setup.auto_mesh_id='UNCOMMITTED-TEST-ONLY'\n");
	CHECK("saved UCI deltas are ignored", mesh_native_config_read(root, &second) == MESH_OK &&
		  strcmp(config.on_air_id, second.on_air_id) == 0 && strcmp(config.credential, second.credential) == 0);
	CHECK("native factory evidence collected", mesh_factory_collect(root, config.wan_device, &ops, &identity) == MESH_OK &&
		  mesh11sd_factory_mac(&identity, &chosen) == MESH_OK && chosen.source == MESH_FACTORY_BOARD);
	(void)mesh_mac_parse("00:11:22:33:44:10", expected);
	CHECK("JSON identity is from system object only", memcmp(chosen.mac, expected, 6) == 0);
	CHECK("native config to evidence to tested planner integrated", mesh_native_plan(root, &ops, &config, &facts, &plan) == MESH_OK);
	CHECK("bridge and factory MAC stay distinct", memcmp(facts.factory_mac, facts.bridge_mac, 6) != 0 &&
		  strstr(plan.identity.management_address, ":0211:22ff:fe33:4466/64") != NULL);
	CHECK("native repeat planning has no drift", mesh_native_plan(root, &ops, &second, &facts, &again) == MESH_OK &&
		  mesh_plan_delta(&plan, &again) == 0);
	probes.role = MESH_ROLE_MRP;
	CHECK("detected routed portal selects real server policy", mesh_native_plan(root, &ops, &second, &facts, &again) == MESH_OK &&
		  again.dhcp_server && second.mobility.rootmode == 4);
	probes.role = MESH_ROLE_PEER;
	probes.fail_bridge = true;
	CHECK("failed native bridge evidence cannot be substituted", mesh_native_plan(root, &ops, &second, &facts, &again) == MESH_E_PLATFORM);
	probes.fail_bridge = false;
	probes.change_on_radio = true;
	memset(&sentinel, 0xa5, sizeof(sentinel));
	again = sentinel;
	CHECK("concurrently changed committed input detected", mesh_native_plan(root, &ops, &second, &facts, &again) == MESH_E_INPUT);
	CHECK("failed native plan leaves proposed plan untouched", memcmp(&again, &sentinel, sizeof(again)) == 0);
	CHECK("failed native plan clears credential memory", second.credential[0] == '\0');
	probes.change_on_radio = false;
	fixture(root);
	CHECK("file snapshots captured", mesh_read_bounded_at(root, "etc/config/system", buffer, sizeof(buffer), &length, &stamp) == 0);
	CHECK("unchanged snapshot recognised", mesh_stamp_unchanged(root, &stamp) == 0);
	text(root, "etc/config/system", "config system\n option label_macaddr '00:11:22:33:44:56'\n");
	CHECK("in-place changed snapshot rejected", mesh_stamp_unchanged(root, &stamp) < 0);
	CHECK("relative traversal input rejected", mesh_read_bounded_at(root, "../etc/passwd", buffer, sizeof(buffer), &length, NULL) < 0);
	text(root, "confined/marker", "PUBLIC-TEST-ONLY");
	if (symlinkat("/confined", root, "etc/absolute") != 0 || symlinkat("../../..", root, "etc/escape") != 0) {
		abort();
	}
	CHECK("absolute intermediate symlink is anchored inside fixture root", mesh_read_bounded_at(root, "etc/absolute/marker",
		  buffer, sizeof(buffer), &length, NULL) == 0 && length == 16 && memcmp(buffer, "PUBLIC-TEST-ONLY", 16) == 0);
	CHECK("intermediate traversal symlink cannot escape fixture root", mesh_read_bounded_at(root, "etc/escape/etc/passwd",
		  buffer, sizeof(buffer), &length, NULL) < 0);
	CHECK("overlong input rejected rather than truncated", mesh_read_bounded_at(root, "etc/config/mesh11sd", buffer, 8, &length, NULL) < 0);
	text(root, "etc/board.json", "{broken-json");
	CHECK("malformed board JSON fails explicitly", mesh_factory_collect(root, "wan", &ops, &identity) == MESH_E_INPUT);
	fixture(root);
	text(root, "etc/config/mesh11sd", "config mesh11sd 'setup'\n option auto_config '1'\n option auto_mesh_network 'lan;not-a-command'\n");
	CHECK("untrusted UCI network name never reaches shell", mesh_native_config_read(root, &second) == MESH_E_INPUT);
	fixture(root);
	text(root, "etc/config/wireless", "config wifi-device 'radio0'\n option phy 'phy0'\n option band '2g'\n option channel 'auto'\n option htmode 'HT20'\n");
	CHECK("unresolved automatic channel does not invent a frequency", mesh_native_config_read(root, &second) == MESH_E_RADIO);
	fixture(root);
	text(root, "etc/config/mesh11sd", "config mesh11sd 'setup'\n option auto_config '1'\n option portal_channel '6'\n");
	CHECK("explicit supported portal channel overrides committed radio channel", mesh_native_config_read(root, &second) == MESH_OK && second.radio_channel == 6 && second.config.frequency_mhz == 2437);
	text(root, "etc/config/mesh11sd", "config mesh11sd 'setup'\n option auto_config '1'\n option portal_channel 'auto'\n");
	CHECK("automatic portal channel fails explicitly pending migration", mesh_native_config_read(root, &second) == MESH_E_RADIO);
	fixture(root);
	text(root, "etc/config/mesh11sd", "config mesh11sd 'setup'\n option auto_config '2'\n");
	CHECK("locked configuration not silently rewritten", mesh_native_config_read(root, &second) == MESH_E_INPUT);
	fixture(root);
	text(root, "etc/config/wireless", "config wifi-device 'radio0'\n option phy 'phy0'\n option band '2g'\n option channel '1'\n option htmode 'HT20'\n option disabled '1'\nconfig wifi-iface 'default_radio0'\n option device 'radio0'\n option mode 'ap'\n");
	CHECK("desired APs counted even when parent radio disabled", mesh_native_config_read(root, &second) == MESH_OK && second.configured_ap_count == 1);
	CHECK("empty current PHY does not prove desired AP mesh concurrency", mesh_native_plan(root, &ops, &second, &facts, &again) == MESH_E_RADIO);
	fixture(root);
	text(root, "etc/config/wireless", "config wifi-device 'radio0'\n option phy 'phy0'\n option band '2g'\n option channel '1'\n option htmode 'HT20'\nconfig wifi-iface 'foreign'\n option device 'radio0'\n option mode 'mesh'\n");
	CHECK("foreign mesh config never auto-enabled alongside beta", mesh_native_config_read(root, &second) == MESH_E_RADIO);
	fixture(root);
	text(root, "sys/class/net/br-lan/bridge/stp_state", "1\n");
	CHECK("enabled bridge STP fails closed without mutating state", mesh_native_plan(root, &ops, &second, &facts, &again) == MESH_E_PLATFORM);
	fixture(root);
	text(root, "etc/config/mesh11sd", "config mesh11sd 'setup'\n option auto_config '1'\n option auto_mesh_band '2g'\n option use_default_beacon_interval '1'\n");
	CHECK("closed v8 item15 keeps configured radio beacon independent of mobility", mesh_native_plan(root, &ops, &second, &facts, &again) == MESH_OK && second.mobility.beacon_interval == 0 && second.radio_beacon_interval == 100);
	fixture(root);
	CHECK("legacy mobility 0 leaves kernel defaults", mesh_mobility_read(0, false, false, &mobility) == MESH_OK && !mobility.tuned);
	CHECK("v8 moving-peer root4 without beacon changes", mesh_mobility_read(2, false, false, &mobility) == MESH_OK &&
		  mobility.rssi_threshold == -72 && mobility.beacon_interval == 0 && mobility.rootmode == 4 && mobility.active_path_timeout == 1100);
	CHECK("portal root4 does not enable legacy beacon tuning", mesh_mobility_read(2, true, true, &mobility) == MESH_OK &&
		  mobility.rootmode == 4 && mobility.beacon_interval == 0 && mobility.dtim_period == 0);
	CHECK("invalid mobility fails rather than applying defaults", mesh_mobility_read(7, false, false, &mobility) == MESH_E_INPUT);
	CHECK("fixture roots cannot accidentally query host networking", mesh_native_plan(root, NULL, &second, &facts, &again) == MESH_E_PLATFORM);
	mesh_native_config_clear(&config);
	mesh_native_config_clear(&second);
}

struct mock {
	struct mesh_runtime_data current;
	unsigned int reads;
	unsigned int writes;
	unsigned int reloads;
	unsigned int verifies;
	unsigned int fail_reload;
	bool fail_verify;
	bool concurrent_on_replace;
	bool concurrent_on_verify;
	bool block_journal_on_verify;
	int journal_fd;
	volatile sig_atomic_t *cancel_on_replace;
};

static int mock_read(void *context, struct mesh_runtime_data *data)
{
	struct mock *mock;

	mock = context;
	mock->reads++;
	*data = mock->current;
	return 0;
}

static int mock_replace(void *context, const struct mesh_runtime_data *expected,
						 const struct mesh_runtime_data *replacement)
{
	struct mock *mock;

	mock = context;
	if (mock->concurrent_on_replace) {
		mock->current.bytes[0] ^= 1;
		return -1;
	}
	if (expected->length != mock->current.length ||
		memcmp(expected->bytes, mock->current.bytes, expected->length) != 0) {
		return -1;
	}
	mock->writes++;
	mock->current = *replacement;
	if (mock->cancel_on_replace != NULL) {
		*mock->cancel_on_replace = 1;
		mock->cancel_on_replace = NULL;
	}
	return 0;
}

static int mock_reload(void *context)
{
	struct mock *mock;

	mock = context;
	mock->reloads++;
	if (mock->fail_reload != 0) {
		mock->fail_reload--;
		return -1;
	}
	return 0;
}

static int mock_verify(void *context, const struct mesh_plan *plan)
{
	struct mock *mock;

	mock = context;
	mock->verifies++;
	if (mock->block_journal_on_verify) {
		text(mock->journal_fd, "transaction.pending", "TEST-ONLY-JOURNAL-FAILURE");
	}
	if (mock->concurrent_on_verify) {
		mock->current.bytes[0] ^= 1;
	}
	return !mock->fail_verify && plan->role == MESH_ROLE_PEER ? 0 : -1;
}

static int journal_dir(int root, const char *name)
{
	if (mkdirat(root, name, 0700) < 0) {
		abort();
	}
	return openat(root, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

static bool channel_schema(const struct mesh_runtime_data *data, const char *radio)
{
	const struct blobmsg_policy top = { .name = "wifi-device", .type = BLOBMSG_TYPE_TABLE };
	const struct blobmsg_policy device = { .name = radio, .type = BLOBMSG_TYPE_TABLE };
	const struct blobmsg_policy channel = { .name = "channel", .type = BLOBMSG_TYPE_STRING };
	struct blob_attr *record[1];
	struct blob_attr *root;
	struct blob_attr *parent;

	root = (struct blob_attr *)data->bytes;
	if (data->length < sizeof(*root) || blob_pad_len(root) != data->length ||
		blobmsg_parse(&top, 1, record, blob_data(root), blob_len(root)) != 0 || record[0] == NULL) {
		return false;
	}
	parent = record[0];
	if (blobmsg_parse(&device, 1, record, blobmsg_data(parent), blobmsg_data_len(parent)) != 0 || record[0] == NULL) {
		return false;
	}
	parent = record[0];
	if (blobmsg_parse(&channel, 1, record, blobmsg_data(parent), blobmsg_data_len(parent)) != 0 || record[0] == NULL) {
		return false;
	}
	return strcmp(blobmsg_get_string(record[0]), "1") == 0;
}

static void test_runtime(int root)
{
	struct mesh_native_config config = {0};
	struct mesh_facts facts = {0};
	struct mesh_plan plan = {0};
	struct mesh_runtime_data wanted = {0};
	struct mesh_runtime_transaction transaction;
	struct probes probes = { .root = root, .role = MESH_ROLE_PEER };
	struct mesh_discovery_ops discovery = { .context = &probes, .radio = radio_probe,
										   .bridge = bridge_probe, .role = role_probe };
	struct mock mock;
	struct mesh_runtime_ops ops = { .context = &mock, .read = mock_read, .replace = mock_replace,
								   .reload = mock_reload, .verify = mock_verify };
	enum mesh_runtime_result result;
	volatile sig_atomic_t cancelled;
	struct stat metadata;
	char journal_text[2048];
	size_t journal_length;
	int directory;
	unsigned int writes;
	unsigned int reloads;

	fixture(root);
	CHECK("runtime uses integrated native plan", mesh_native_plan(root, &discovery, &config, &facts, &plan) == MESH_OK);
	CHECK("runtime procd payload built in confidential memory", mesh_runtime_payload(&config, &plan, &wanted) == MESH_OK && wanted.length > 0);
	CHECK("payload has wifi-device wifi-iface and network-interface records",
		  memmem(wanted.bytes, wanted.length, "wifi-device", 11) != NULL &&
		  memmem(wanted.bytes, wanted.length, "wifi-iface", 10) != NULL &&
		  memmem(wanted.bytes, wanted.length, "network-interface", 17) != NULL);
	CHECK("runtime channel matches netifd STRING schema not numeric guess", channel_schema(&wanted, config.radio_section));
	CHECK("credential goes only into confidential transport payload",
		  memmem(wanted.bytes, wanted.length, config.credential, 64) != NULL);
	memset(&transaction, 0, sizeof(transaction));
	memset(&mock, 0, sizeof(mock));
	directory = journal_dir(root, "runtime-success");
	cancelled = 0;
	CHECK("dry run is explicitly not apply", mesh_runtime_apply(root, directory, &config, &plan, &wanted, &ops,
		  &cancelled, true, &transaction) == MESH_RUNTIME_DRY_RUN);
	CHECK("dry run performs zero backend calls or lock/journal writes", mock.reads == 0 && mock.writes == 0 &&
		  fstatat(directory, "transaction.lock", &metadata, AT_SYMLINK_NOFOLLOW) < 0 && errno == ENOENT);
	CHECK("runtime mock apply publishes reloads and verifies", mesh_runtime_apply(root, directory, &config, &plan,
		  &wanted, &ops, &cancelled, false, &transaction) == MESH_RUNTIME_OK && transaction.active &&
		  mock.writes == 1 && mock.reloads == 1 && mock.verifies == 1);
	CHECK("metadata backup mode is 0600", fstatat(directory, "transaction.meta", &metadata, AT_SYMLINK_NOFOLLOW) == 0 &&
		  (metadata.st_mode & 0777) == 0600);
	CHECK("metadata backup readable", mesh_read_bounded_at(directory, "transaction.meta", journal_text,
		  sizeof(journal_text) - 1, &journal_length, NULL) == 0);
	journal_text[journal_length] = '\0';
	CHECK("journal contains no input values, on-air ID or credential", strstr(journal_text, config.credential) == NULL &&
		  strstr(journal_text, config.on_air_id) == NULL && strstr(journal_text, "TEST-ONLY") == NULL);
	writes = mock.writes;
	reloads = mock.reloads;
	CHECK("repeated owned apply is idempotent", mesh_runtime_apply(root, directory, &config, &plan, &wanted,
		  &ops, &cancelled, false, &transaction) == MESH_RUNTIME_UNCHANGED &&
		  mock.writes == writes && mock.reloads == reloads);
	CHECK("explicit stop restores original runtime data", mesh_runtime_stop(directory, &ops, &transaction) == MESH_RUNTIME_OK &&
		  !transaction.active && mock.current.length == 0 && mock.writes == 2 && mock.reloads == 2);
	CHECK("repeated stop is idempotent", mesh_runtime_stop(directory, &ops, &transaction) == MESH_RUNTIME_UNCHANGED);
	close(directory);
	mesh_runtime_transaction_clear(&transaction);
	memset(&mock, 0, sizeof(mock));
	directory = journal_dir(root, "runtime-reload-failure");
	mock.fail_reload = 1;
	CHECK("reload failure compensates fully", mesh_runtime_apply(root, directory, &config, &plan, &wanted, &ops,
		  &cancelled, false, &transaction) == MESH_RUNTIME_FAILED_ROLLED_BACK && !transaction.active &&
		  mock.current.length == 0 && mock.writes == 2);
	close(directory);
	mesh_runtime_transaction_clear(&transaction);
	memset(&mock, 0, sizeof(mock));
	directory = journal_dir(root, "runtime-verify-failure");
	mock.fail_verify = true;
	CHECK("verification failure compensates rather than claiming success", mesh_runtime_apply(root, directory, &config,
		  &plan, &wanted, &ops, &cancelled, false, &transaction) == MESH_RUNTIME_FAILED_ROLLED_BACK && mock.current.length == 0);
	close(directory);
	mesh_runtime_transaction_clear(&transaction);
	memset(&mock, 0, sizeof(mock));
	directory = journal_dir(root, "runtime-cancel");
	mock.cancel_on_replace = &cancelled;
	CHECK("interruption after publish compensates before returning", mesh_runtime_apply(root, directory, &config, &plan,
		  &wanted, &ops, &cancelled, false, &transaction) == MESH_RUNTIME_FAILED_ROLLED_BACK &&
		  mock.current.length == 0 && !transaction.active);
	close(directory);
	cancelled = 0;
	mesh_runtime_transaction_clear(&transaction);
	memset(&mock, 0, sizeof(mock));
	directory = journal_dir(root, "runtime-retry-rollback");
	mock.fail_reload = 2;
	result = mesh_runtime_apply(root, directory, &config, &plan, &wanted, &ops, &cancelled, false, &transaction);
	CHECK("failed compensation is explicit and retained", result == MESH_RUNTIME_ROLLBACK_NEEDED &&
		  transaction.rollback_needed && transaction.data_restored && transaction.active);
	CHECK("retry stop completes reload of already-restored data", mesh_runtime_stop(directory, &ops, &transaction) == MESH_RUNTIME_OK &&
		  mock.current.length == 0 && !transaction.rollback_needed && !transaction.active);
	close(directory);
	mesh_runtime_transaction_clear(&transaction);
	memset(&mock, 0, sizeof(mock));
	directory = journal_dir(root, "runtime-journal-failure");
	mock.block_journal_on_verify = true;
	mock.journal_fd = directory;
	CHECK("journal failure compensates runtime but retains recovery state", mesh_runtime_apply(root, directory, &config,
		  &plan, &wanted, &ops, &cancelled, false, &transaction) == MESH_RUNTIME_ROLLBACK_NEEDED &&
		  mock.current.length == 0 && transaction.active && transaction.data_restored);
	CHECK("foreign pending metadata is not overwritten or removed", fstatat(directory, "transaction.pending", &metadata,
		  AT_SYMLINK_NOFOLLOW) == 0);
	if (unlinkat(directory, "transaction.pending", 0) != 0) {
		abort();
	}
	CHECK("metadata recovery retry finishes already compensated transaction", mesh_runtime_stop(directory, &ops, &transaction) == MESH_RUNTIME_OK && !transaction.active);
	close(directory);
	mesh_runtime_transaction_clear(&transaction);
	memset(&mock, 0, sizeof(mock));
	directory = journal_dir(root, "runtime-concurrent");
	mock.concurrent_on_replace = true;
	CHECK("concurrent runtime replacement is not overwritten", mesh_runtime_apply(root, directory, &config, &plan, &wanted,
		  &ops, &cancelled, false, &transaction) == MESH_RUNTIME_CONFLICT && mock.writes == 0 && !transaction.active);
	close(directory);
	mesh_runtime_transaction_clear(&transaction);
	memset(&mock, 0, sizeof(mock));
	directory = journal_dir(root, "runtime-concurrent-after");
	mock.concurrent_on_verify = true;
	CHECK("concurrent post-publish change prevents destructive rollback", mesh_runtime_apply(root, directory, &config,
		  &plan, &wanted, &ops, &cancelled, false, &transaction) == MESH_RUNTIME_ROLLBACK_NEEDED &&
		  mock.writes == 1 && transaction.rollback_needed);
	close(directory);
	mesh_runtime_transaction_clear(&transaction);
	memset(&mock, 0, sizeof(mock));
	directory = journal_dir(root, "runtime-stale");
	text(directory, "transaction.meta", "mesh11sd-v8-transaction 1\nphase=verified\n");
	CHECK("stale prior-owner journal blocks fresh apply before backend calls", mesh_runtime_apply(root, directory, &config,
		  &plan, &wanted, &ops, &cancelled, false, &transaction) == MESH_RUNTIME_STALE_JOURNAL && mock.reads == 0 && mock.writes == 0);
	CHECK("stale journal remains unchanged", mesh_read_bounded_at(directory, "transaction.meta", journal_text,
		  sizeof(journal_text), &journal_length, NULL) == 0 && memmem(journal_text, journal_length, "phase=verified", 14) != NULL);
	close(directory);
	directory = journal_dir(root, "runtime-pending");
	text(directory, "transaction.pending", "TEST-ONLY-INCOMPLETE-METADATA");
	CHECK("interrupted pending journal blocks fresh apply", mesh_runtime_apply(root, directory, &config, &plan, &wanted,
		  &ops, &cancelled, false, &transaction) == MESH_RUNTIME_STALE_JOURNAL && mock.writes == 0);
	close(directory);
	directory = journal_dir(root, "runtime-restored-owner");
	text(directory, "transaction.meta", "mesh11sd-v8-transaction 1\nphase=restored\n");
	CHECK("known restored journal permits new owned transaction", mesh_runtime_apply(root, directory, &config, &plan,
		  &wanted, &ops, &cancelled, false, &transaction) == MESH_RUNTIME_OK);
	CHECK("new transaction from restored journal stops cleanly", mesh_runtime_stop(directory, &ops, &transaction) == MESH_RUNTIME_OK);
	close(directory);
	mesh_runtime_transaction_clear(&transaction);
	memset(&mock, 0, sizeof(mock));
	directory = journal_dir(root, "runtime-changed-config");
	text(root, "etc/config/network", "config interface 'lan'\n option device 'changed-by-user'\n");
	CHECK("changed committed config aborts before runtime mutation", mesh_runtime_apply(root, directory, &config,
		  &plan, &wanted, &ops, &cancelled, false, &transaction) == MESH_RUNTIME_CHANGED_INPUT && mock.reads == 0 && mock.writes == 0);
	CHECK("concurrent committed file is not restored or overwritten", mesh_read_bounded_at(root, "etc/config/network",
		  journal_text, sizeof(journal_text) - 1, &journal_length, NULL) == 0 &&
		  memmem(journal_text, journal_length, "changed-by-user", 15) != NULL);
	close(directory);
	fixture(root);
	plan.role = MESH_ROLE_MRP;
	CHECK("unfinished MRP backend cannot emit partial success payload", mesh_runtime_payload(&config, &plan, &wanted) == MESH_E_ROLE_PENDING);
	CHECK("transaction cannot bypass unsupported MRP payload gate", mesh_runtime_apply(root, -1, &config, &plan,
		  &wanted, &ops, &cancelled, true, &transaction) == MESH_RUNTIME_INVALID);
	mesh_native_config_clear(&config);
	mesh_runtime_data_clear(&wanted);
	mesh_runtime_transaction_clear(&transaction);
}

int main(int argc, char **argv)
{
	int root;

	if (argc != 2) {
		return 2;
	}
	root = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (root < 0) {
		return 2;
	}
	puts("TAP version 13");
	fixture(root);
	test_inputs(root);
	test_runtime(root);
	close(root);
	printf("1..%u\n# %u checks, %u failures\n", checks, checks, failures);
	return failures == 0 ? 0 : 1;
}
