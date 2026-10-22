/* Exercise the real services and on-disk formats with a host filesystem. */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* Model an embedded filesystem that cannot transfer a whole snapshot at
 * once. Apply the restriction to Contacts, including legacy-file reads. */
static size_t contacts_test_read(void *data, size_t size, size_t count, FILE *file)
{
    if (size * count > 4096U) { errno = ENOMEM; return 0; }
    return fread(data, size, count, file);
}
static size_t contacts_test_write(const void *data, size_t size, size_t count, FILE *file)
{
    if (size * count > 4096U) { errno = ENOMEM; return 0; }
    return fwrite(data, size, count, file);
}
#define fread contacts_test_read
#define fwrite contacts_test_write
#include "../../src/services/solar_os_contacts.c"
#undef fread
#undef fwrite
#include "../../src/services/solar_os_messaging.c"

static char test_root[] = "/tmp/solaros-messaging-XXXXXX";
bool solar_os_disk_image_path_busy(const char *path) { (void)path; return false; }
size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t length = strlen(src);
    if (size) { size_t n = length < size - 1 ? length : size - 1; memcpy(dst, src, n); dst[n] = 0; }
    return length;
}
void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t cls, const char *tag)
{ (void)cls; (void)tag; return calloc(count, size); }
void solar_os_memory_free(void *ptr) { free(ptr); }
bool solar_os_memory_is_external(const void *ptr) { return ptr != NULL; }
int64_t esp_timer_get_time(void) { return 1000000; }
TickType_t xTaskGetTickCount(void) { return 1000; }
void vTaskDelay(TickType_t ticks) { (void)ticks; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "test error"; }
bool __wrap_solar_os_storage_is_mounted(void) { return true; }
bool __wrap_solar_os_storage_sd_is_mounted(void) { return true; }
esp_err_t __wrap_solar_os_storage_default_path(const char *relative, char *path, size_t size)
{
    int n = snprintf(path, size, "%s/%s", test_root, relative);
    return n >= 0 && (size_t)n < size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
esp_err_t __wrap_solar_os_storage_mkdir(const char *path) { return mkdir(path, 0700) == 0 ? ESP_OK : ESP_FAIL; }
esp_err_t __wrap_solar_os_storage_remove(const char *path) { return unlink(path) == 0 ? ESP_OK : ESP_FAIL; }
esp_err_t __wrap_solar_os_storage_get_usage(solar_os_storage_usage_t *usage)
{ memset(usage, 0, sizeof(*usage)); usage->free_bytes = 1024 * 1024; return ESP_OK; }
int __real_rename(const char *source, const char *destination);
static bool fail_migration_promotion;
int __wrap_rename(const char *source, const char *destination)
{
    if (fail_migration_promotion && strstr(source, ".tmp") != NULL) {
        fail_migration_promotion = false;
        errno = EIO;
        return -1;
    }
    struct stat info;
    if (stat(destination, &info) == 0) { errno = EEXIST; return -1; }
    return __real_rename(source, destination);
}
esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag, const char *fmt, ...)
{ (void)level; (void)tag; (void)fmt; return ESP_OK; }
esp_err_t solar_os_inbox_init(void) { return ESP_OK; }
esp_err_t solar_os_inbox_get_status(solar_os_inbox_status_t *status)
{ memset(status, 0, sizeof(*status)); return ESP_OK; }
size_t solar_os_inbox_snapshot(solar_os_inbox_entry_t *entries, size_t max, bool unread, size_t *total)
{ (void)entries; (void)max; (void)unread; if (total) *total = 0; return 0; }
void solar_os_inbox_set_clear_observer(solar_os_inbox_clear_observer_t observer, void *user)
{ (void)observer; (void)user; }
bool solar_os_inbox_matches_source_id(uint32_t id, uint64_t source) { (void)id; (void)source; return false; }
esp_err_t solar_os_inbox_publish(const solar_os_inbox_publish_t *message, uint32_t *id)
{ (void)message; if (id) *id = 1; return ESP_OK; }
esp_err_t solar_os_inbox_delete(uint32_t id) { (void)id; return ESP_OK; }
esp_err_t solar_os_inbox_mark_read(uint32_t id, bool read) { (void)id; (void)read; return ESP_OK; }
esp_err_t solar_os_inbox_delete_sources(const char *const *sources, size_t count, size_t *deleted)
{ (void)sources; (void)count; if (deleted) *deleted = 0; return ESP_OK; }

static void restart_contacts(void)
{
    free(contacts_store.contacts); free(contacts_store.endpoints); free(contacts_store.scratch);
    vSemaphoreDelete(contacts_store.lock); vSemaphoreDelete(contacts_store.io_lock);
    memset(&contacts_store, 0, sizeof(contacts_store));
    assert(solar_os_contacts_init() == ESP_OK);
}
static void restart_messages(void)
{
    while (messaging.providers) {
        messaging_provider_node_t *next = messaging.providers->next;
        free(messaging.providers); messaging.providers = next;
    }
    while (messaging.classes) {
        messaging_class_node_t *next = messaging.classes->next;
        free(messaging.classes); messaging.classes = next;
    }
    free(messaging.conversations); free(messaging.messages); free(messaging.outbox);
    free(messaging.events); free(messaging.record_scratch);
    vSemaphoreDelete(messaging.lock); vSemaphoreDelete(messaging.io_lock);
    memset(&messaging, 0, sizeof(messaging));
    assert(solar_os_messaging_init() == ESP_OK);
}

static void legacy_contacts_fixture(void)
{
    typedef struct __attribute__((packed)) {
        uint32_t generation, next_contact_id, next_endpoint_id, evicted;
        uint16_t contact_count, endpoint_count;
        contacts_disk_contact_t contacts[64];
        contacts_disk_endpoint_t endpoints[80];
    } legacy_data_t;
    legacy_data_t *data = calloc(1, sizeof(*data));
    assert(data);
    data->generation = 31; data->next_contact_id = 9; data->next_endpoint_id = 15;
    data->contact_count = 1; data->endpoint_count = 1;
    data->contacts[63] = (contacts_disk_contact_t){ .active = 1, .id = 7,
        .flags = SOLAR_OS_CONTACT_FLAG_PINNED, .created_ms = 100, .updated_ms = 200 };
    strcpy(data->contacts[63].display_name, "Retained trusted contact");
    data->endpoints[79] = (contacts_disk_endpoint_t){ .active = 1, .id = 11, .contact_id = 7,
        .provider = SOLAR_OS_MESSAGING_PROVIDER_MESHCORE, .trust = SOLAR_OS_CONTACT_TRUST_TRUSTED,
        .address_len = 32, .provider_metadata_len = 3 };
    memset(data->endpoints[79].address, 0xa5, 32);
    memcpy(data->endpoints[79].provider_metadata, "old", 3);
    contacts_legacy_header_t header = { .magic = CONTACTS_STORE_MAGIC, .version = 1,
        .data_size = sizeof(*data), .generation = 17, .data_slot = 0,
        .data_crc32 = contacts_crc32(data, sizeof(*data)) };
    header.header_crc32 = contacts_crc32(&header, offsetof(contacts_legacy_header_t, header_crc32));
    contacts_legacy_header_t newer = header;
    newer.generation = 18; newer.data_slot = 1;
    newer.header_crc32 = contacts_crc32(&newer, offsetof(contacts_legacy_header_t, header_crc32));
    char path[160];
    assert(solar_os_storage_default_path(".contacts", path, sizeof(path)) == ESP_OK);
    assert(mkdir(path, 0700) == 0);
    assert(solar_os_storage_default_path(".contacts/contacts.bin", path, sizeof(path)) == ESP_OK);
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fwrite(&header, sizeof(header), 1, file) == 1);
    assert(fwrite(&newer, sizeof(newer), 1, file) == 1);
    assert(fwrite(data, sizeof(*data), 1, file) == 1);
    data->generation++; /* Deliberately corrupt the newer copy: fall back to the valid older one. */
    assert(fwrite(data, sizeof(*data), 1, file) == 1);
    assert(fclose(file) == 0); free(data);
}

static void assert_legacy_contact(void)
{
    solar_os_contact_t contact; solar_os_endpoint_t endpoint;
    assert(solar_os_contacts_get(7, &contact) == ESP_OK);
    assert(contact.flags == SOLAR_OS_CONTACT_FLAG_PINNED);
    assert(strcmp(contact.display_name, "Retained trusted contact") == 0);
    assert(solar_os_contacts_get_endpoint(11, &endpoint) == ESP_OK);
    assert(endpoint.contact_id == 7 && endpoint.trust == SOLAR_OS_CONTACT_TRUST_TRUSTED);
    assert(endpoint.address.bytes[0] == 0xa5 && endpoint.provider_metadata_len == 3);
    assert(memcmp(endpoint.provider_metadata, "old", 3) == 0);
}

static void test_contacts_migration_and_import(void)
{
    legacy_contacts_fixture();
    assert(solar_os_contacts_init() == ESP_OK);
    assert(contacts_store.legacy_format);
    assert_legacy_contact();
    /* A failed migration must leave the original file readable on restart. */
    char temporary[160];
    assert(solar_os_storage_default_path(".contacts/contacts.bin.tmp", temporary,
                                         sizeof(temporary)) == ESP_OK);
    assert(mkdir(temporary, 0700) == 0);
    assert(solar_os_contacts_flush() != ESP_OK);
    restart_contacts();
    assert(contacts_store.legacy_format);
    assert_legacy_contact();
    assert(rmdir(temporary) == 0);
    fail_migration_promotion = true;
    assert(solar_os_contacts_flush() != ESP_OK);
    assert(!fail_migration_promotion);
    restart_contacts();
    assert(contacts_store.legacy_format);
    assert_legacy_contact();
    uint32_t disk_generation = contacts_store.disk_generation;
    for (unsigned i = 0; i < SOLAR_OS_CONTACT_CAPACITY - 1; i++) {
        uint8_t key[32] = {0}; memcpy(key, &i, sizeof(i));
        assert(solar_os_contacts_import_discovered(SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
            key, sizeof(key), "Companion", SOLAR_OS_ENDPOINT_CAP_DIRECT, i + 300,
            NULL, 0, NULL, NULL) == ESP_OK);
    }
    assert(contacts_store.disk_generation == disk_generation); /* One batch, no per-contact writes. */
    uint8_t overflow_key[32]; memset(overflow_key, 0xff, sizeof(overflow_key));
    assert(solar_os_contacts_import_discovered(SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
        overflow_key, sizeof(overflow_key), "Overflow", SOLAR_OS_ENDPOINT_CAP_DIRECT,
        9000, NULL, 0, NULL, NULL) == ESP_ERR_NO_MEM);
    assert(contacts_store.evicted == 0 && contacts_store.contact_count == SOLAR_OS_CONTACT_CAPACITY);
    assert_legacy_contact();
    assert(solar_os_contacts_flush() == ESP_OK);
    assert(!contacts_store.legacy_format && contacts_store.disk_generation == disk_generation + 1);
    restart_contacts(); assert_legacy_contact();
    assert(contacts_store.contact_count == SOLAR_OS_CONTACT_CAPACITY);
    assert(contacts_store.evicted == 0);
    /* Recover the retained file if reset interrupts backup-and-replace. */
    assert(rename(contacts_store.store_path, contacts_store.backup_path) == 0);
    restart_contacts(); assert_legacy_contact();
    assert(contacts_store.contact_count == SOLAR_OS_CONTACT_CAPACITY);
    /* Updating an existing imported contact must preserve its endpoint ID at capacity. */
    uint8_t existing_key[32] = {0}; solar_os_endpoint_t before, after;
    assert(solar_os_contacts_find_endpoint(SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
        existing_key, 32, &before) == ESP_OK);
    assert(solar_os_contacts_import_discovered(SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
        existing_key, 32, "Updated", SOLAR_OS_ENDPOINT_CAP_DIRECT, 10000,
        NULL, 0, NULL, NULL) == ESP_OK);
    assert(solar_os_contacts_find_endpoint(SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
        existing_key, 32, &after) == ESP_OK);
    assert(before.id == after.id && after.last_seen_ms == 10000);
}

static solar_os_conversation_id_t group(const char *key, uint32_t ref)
{
    solar_os_messaging_conversation_upsert_t request = {
        .provider = SOLAR_OS_MESSAGING_PROVIDER_MESHCORE, .provider_key = key,
        .kind = SOLAR_OS_CONVERSATION_GROUP, .title = "Public", .group_ref = ref };
    solar_os_conversation_id_t id;
    assert(solar_os_messaging_conversation_upsert(&request, &id) == ESP_OK);
    return id;
}
static void test_channel_ownership_and_restart(void)
{
    assert(solar_os_messaging_init() == ESP_OK);
    assert(solar_os_messaging_provider_register(SOLAR_OS_MESSAGING_PROVIDER_MESHCORE, "meshcore") == ESP_OK);
    assert(solar_os_messaging_groups_begin_sync(SOLAR_OS_MESSAGING_PROVIDER_MESHCORE, "group:") == ESP_OK);
    solar_os_conversation_id_t radio = group("group:2147483649", 2147483649U);
    solar_os_conversation_id_t ble = group("ble-group:0", 0x424c4500U);
    solar_os_message_key_t queued, sending, sent;
    assert(solar_os_messaging_send(ble, "Wrong transport", false, NULL) == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_messaging_send(radio, "Queued before reboot", false, &queued) == ESP_OK);
    assert(solar_os_messaging_send(radio, "Sending before reboot", false, &sending) == ESP_OK);
    assert(solar_os_messaging_send(radio, "Already sent", false, &sent) == ESP_OK);
    solar_os_messaging_outbound_t requests[3];
    assert(solar_os_messaging_outbox_snapshot(requests, 3) == 3);
    assert(solar_os_messaging_outbox_update(requests[1].id, SOLAR_OS_DELIVERY_SENDING, NULL) == ESP_OK);
    assert(solar_os_messaging_outbox_update(requests[2].id, SOLAR_OS_DELIVERY_SENT, NULL) == ESP_OK);
    restart_messages();
    solar_os_messaging_message_t *message = malloc(sizeof(*message)); assert(message);
    assert(solar_os_messaging_message_get(queued, message) == ESP_OK);
    assert(message->delivery == SOLAR_OS_DELIVERY_FAILED && strstr(message->error, "restart"));
    assert(solar_os_messaging_message_get(sending, message) == ESP_OK);
    assert(message->delivery == SOLAR_OS_DELIVERY_FAILED);
    assert(solar_os_messaging_message_get(sent, message) == ESP_OK);
    assert(message->delivery == SOLAR_OS_DELIVERY_SENT);
    assert(solar_os_messaging_outbox_snapshot(requests, 3) == 0);
    solar_os_messaging_conversation_t conversation;
    assert(solar_os_messaging_conversation_get(message->conversation_id, &conversation) == ESP_OK);
    char label[64]; solar_os_messaging_conversation_label(&conversation, label, sizeof(label));
    assert(strcmp(label, "Public [radio] (history)") == 0);
    assert(solar_os_messaging_send(conversation.id, "Inactive", false, NULL) == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_messaging_groups_begin_sync(SOLAR_OS_MESSAGING_PROVIDER_MESHCORE, "ble-group:") == ESP_OK);
    ble = group("ble-group:0", 0x424c4500U);
    /* Inspecting/upserting the unused local transport must not reactivate it. */
    radio = group("group:2147483649", 2147483649U);
    assert(solar_os_messaging_send(radio, "Still inactive", false, NULL) == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_messaging_conversation_get(ble, &conversation) == ESP_OK);
    solar_os_messaging_conversation_label(&conversation, label, sizeof(label));
    assert(strcmp(label, "Public [companion]") == 0);
    assert(solar_os_messaging_send(ble, "Active companion", false, NULL) == ESP_OK);
    /* A same-transport refresh also retires channels no longer configured. */
    assert(solar_os_messaging_groups_begin_sync(SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
                                                "ble-group:") == ESP_OK);
    assert(solar_os_messaging_send(ble, "Removed channel", false, NULL) == ESP_ERR_NOT_SUPPORTED);
    assert(group("ble-group:0", 0x424c4500U) == ble);
    assert(solar_os_messaging_conversation_get(ble, &conversation) == ESP_OK);
    assert(!conversation.history_only);
    free(message);
}

static unsigned test_joins;
static bool reject_join;
static unsigned remove_prepares, remove_commits, remove_aborts;
static bool reject_remove_prepare;
static solar_os_messaging_provider_id_t remove_inside_join;
static esp_err_t test_normalize(void *context, const char *target, char *out, size_t size)
{
    assert(context == &test_joins);
    if (strncmp(target, "ROOM:", 5) == 0) target += 5;
    return strlcpy(out, target, size) < size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
static esp_err_t test_join(void *context, const char *target)
{
    assert(context == &test_joins); assert(strcmp(target, "#same") == 0);
    solar_os_messaging_status_t status;
    assert(solar_os_messaging_get_status(&status) == ESP_OK);
    if (remove_inside_join) assert(solar_os_messaging_instance_remove(remove_inside_join) == ESP_ERR_INVALID_STATE);
    test_joins++; return reject_join ? ESP_FAIL : ESP_OK;
}
static esp_err_t test_remove_prepare(void *context)
{
    assert(context == &test_joins); remove_prepares++;
    return reject_remove_prepare ? ESP_FAIL : ESP_OK;
}
static void test_remove_finish(void *context, bool removed)
{
    assert(context == &test_joins);
    if (removed) remove_commits++; else remove_aborts++;
}
static const solar_os_messaging_provider_class_t test_class = {
    .name = "test-net", .capabilities = SOLAR_OS_ENDPOINT_CAP_ROOM, .join = test_join,
    .normalize_target = test_normalize,
    .remove_prepare = test_remove_prepare, .remove_finish = test_remove_finish,
};
static void test_provider_instances(void)
{
    assert(solar_os_messaging_class_register(&test_class) == ESP_OK);
    unsigned config = 0x12345678;
    solar_os_messaging_provider_id_t a, b;
    assert(solar_os_messaging_instance_create("test-net", "network-a", &config, sizeof(config), &a) == ESP_OK);
    assert(solar_os_messaging_instance_create("test-net", "network-b", &config, sizeof(config), &b) == ESP_OK);
    assert(a > 3 && b > a);
    assert(solar_os_messaging_instance_register(a, "test-net", "network-a", &test_joins) == ESP_OK);
    assert(solar_os_messaging_instance_register(b, "test-net", "network-b", &test_joins) == ESP_OK);
    solar_os_conversation_id_t room_a, room_b;
    assert(solar_os_messaging_join(a, "ROOM:#same", &room_a) == ESP_OK);
    assert(solar_os_messaging_join(b, "#same", &room_b) == ESP_OK);
    assert(room_a != room_b && test_joins == 2);
    assert(solar_os_messaging_join_update(a, "#same", SOLAR_OS_MESSAGING_JOIN_JOINED, NULL) == ESP_OK);
    assert(solar_os_messaging_join(a, "#same", NULL) == ESP_OK && test_joins == 2);
    solar_os_messaging_conversation_t room;
    assert(solar_os_messaging_conversation_get(room_b, &room) == ESP_OK);
    assert(room.join_state == SOLAR_OS_MESSAGING_JOIN_PENDING);
    assert(solar_os_messaging_join_update(a, "#same", SOLAR_OS_MESSAGING_JOIN_LEFT, NULL) == ESP_OK);
    reject_join = true;
    assert(solar_os_messaging_join(a, "#same", NULL) == ESP_FAIL);
    assert(solar_os_messaging_conversation_get(room_a, &room) == ESP_OK && room.join_state == SOLAR_OS_MESSAGING_JOIN_FAILED);
    reject_join = false;
    solar_os_message_key_t key_a, key_b;
    assert(solar_os_messaging_send(room_a, "only A", false, &key_a) == ESP_OK);
    assert(solar_os_messaging_send(room_b, "only B", false, &key_b) == ESP_OK);
    solar_os_messaging_outbound_t *out = calloc(1, sizeof(*out)); assert(out);
    assert(solar_os_messaging_outbox_peek(a, out) == ESP_OK && out->message_key == key_a);
    assert(solar_os_messaging_outbox_update(out->id, SOLAR_OS_DELIVERY_SENT, NULL) == ESP_OK);
    assert(solar_os_messaging_outbox_peek(b, out) == ESP_OK && out->message_key == key_b);
    assert(solar_os_messaging_outbox_update(out->id, SOLAR_OS_DELIVERY_FAILED, "test") == ESP_OK);
    free(out);
    unsigned restored = 0;
    assert(solar_os_messaging_profile_load(a, &restored, sizeof(restored)) == ESP_OK && restored == config);
    fail_migration_promotion = true; unsigned updated = 0x77777777;
    assert(solar_os_messaging_profile_save(a, &updated, sizeof(updated)) != ESP_OK);
    assert(solar_os_messaging_profile_load(a, &restored, sizeof(restored)) == ESP_OK && restored == config);
    restart_messages();
    assert(solar_os_messaging_provider_find("network-a") == a);
    assert(solar_os_messaging_provider_find("network-b") == b);
    assert(solar_os_messaging_provider_is_class(a, "test-net"));
    assert(solar_os_messaging_profile_load(a, &restored, sizeof(restored)) == ESP_OK && restored == config);
    assert(solar_os_messaging_class_register(&test_class) == ESP_OK);
    assert(solar_os_messaging_instance_register(a, "test-net", "network-a", &test_joins) == ESP_OK);
    assert(solar_os_messaging_join(a, "#same", NULL) == ESP_OK);
    solar_os_messaging_provider_id_t cursor = 0; solar_os_messaging_provider_status_t status;
    unsigned count = 0;
    while (solar_os_messaging_provider_next(&cursor, &status)) count++;
    assert(count == 5);

    /* A removal cannot destroy a callback's context while it is in use. */
    assert(solar_os_messaging_join_update(a, "#same", SOLAR_OS_MESSAGING_JOIN_LEFT, NULL) == ESP_OK);
    remove_inside_join = a;
    assert(solar_os_messaging_join(a, "#same", NULL) == ESP_OK);
    remove_inside_join = 0;
    assert(solar_os_messaging_instance_remove(1) == ESP_ERR_NOT_SUPPORTED);
    reject_remove_prepare = true;
    assert(solar_os_messaging_instance_remove(a) == ESP_FAIL && remove_commits == 0 && remove_aborts == 0);
    reject_remove_prepare = false;
    fail_migration_promotion = true;
    assert(solar_os_messaging_instance_remove(a) == ESP_FAIL && remove_aborts == 1);
    assert(solar_os_messaging_provider_find("network-a") == a);
    assert(solar_os_messaging_profile_load(a, &restored, sizeof(restored)) == ESP_OK && restored == config);

    solar_os_conversation_id_t live_b = 0;
    solar_os_messaging_conversation_t conversations[32];
    size_t conversation_count = solar_os_messaging_conversation_snapshot(conversations, 32);
    for (size_t i = 0; i < conversation_count; i++) {
        if (conversations[i].provider == a) room_a = conversations[i].id;
        if (conversations[i].provider == b) live_b = conversations[i].id;
    }
    assert(live_b);
    solar_os_message_key_t pending_a, pending_b, pending_b2, sending_a;
    assert(solar_os_messaging_send(live_b, "B before", false, &pending_b) == ESP_OK);
    assert(solar_os_messaging_send(room_a, "A queued", false, &pending_a) == ESP_OK);
    assert(solar_os_messaging_send(live_b, "B after", false, &pending_b2) == ESP_OK);
    out = calloc(1, sizeof(*out)); assert(out);
    assert(solar_os_messaging_outbox_peek(a, out) == ESP_OK);
    assert(solar_os_messaging_outbox_update(out->id, SOLAR_OS_DELIVERY_SENDING, NULL) == ESP_OK);
    sending_a = pending_a;
    assert(solar_os_messaging_send(room_a, "A still queued", false, &pending_a) == ESP_OK);

    assert(solar_os_messaging_instance_remove(a) == ESP_OK && remove_commits == 1);
    assert(solar_os_messaging_instance_remove(a) == ESP_OK && remove_commits == 1);
    assert(solar_os_messaging_provider_find("network-a") == 0);
    assert(solar_os_messaging_provider_get_status(a, &status) == ESP_OK && status.removed && !status.connected);
    assert(solar_os_messaging_profile_load(a, &restored, sizeof(restored)) == ESP_ERR_NOT_FOUND);
    assert(solar_os_messaging_profile_save(a, &config, sizeof(config)) == ESP_ERR_INVALID_STATE);
    assert(solar_os_messaging_instance_register(a, "test-net", "network-a", &test_joins) == ESP_ERR_INVALID_STATE);
    assert(solar_os_messaging_join(a, "#same", NULL) == ESP_ERR_NOT_FOUND);
    assert(solar_os_messaging_connect(a, true) == ESP_ERR_NOT_FOUND);
    assert(solar_os_messaging_send(room_a, "cannot send", false, NULL) != ESP_OK);
    solar_os_messaging_message_t message;
    assert(solar_os_messaging_message_get(pending_a, &message) == ESP_ERR_NOT_FOUND);
    assert(solar_os_messaging_message_get(sending_a, &message) == ESP_ERR_NOT_FOUND);
    assert(solar_os_messaging_conversation_get(room_a, &room) == ESP_ERR_NOT_FOUND);
    assert(solar_os_messaging_outbox_peek(a, out) == ESP_ERR_NOT_FOUND);
    assert(solar_os_messaging_outbox_peek(b, out) == ESP_OK && out->message_key == pending_b);
    assert(solar_os_messaging_outbox_update(out->id, SOLAR_OS_DELIVERY_SENT, NULL) == ESP_OK);
    assert(solar_os_messaging_outbox_peek(b, out) == ESP_OK && out->message_key == pending_b2);
    assert(solar_os_messaging_outbox_update(out->id, SOLAR_OS_DELIVERY_SENT, NULL) == ESP_OK);
    free(out);

    char archived_path[160];
    assert(solar_os_storage_default_path(".chat/profiles/004.bin", archived_path, sizeof(archived_path)) == ESP_OK);
    struct stat info; assert(stat(archived_path, &info) == 0 && info.st_size == sizeof(messaging_profile_header_t));
    /* Simulate an old backup surviving an interrupted cleanup: the committed
     * removal record must take precedence, both before and after restart. */
    char backup_path[160]; assert(snprintf(backup_path, sizeof(backup_path), "%s.bak", archived_path) < (int)sizeof(backup_path));
    FILE *backup = fopen(backup_path, "wb"); assert(backup);
    messaging_profile_header_t old = {.magic = MESSAGING_PROFILE_MAGIC, .version = 1, .id = a, .size = sizeof(config)};
    strcpy(old.name, "network-a"); strcpy(old.class_name, "test-net");
    old.crc = (uint32_t)messaging_hash_update(messaging_hash_update(2166136261U, &old, sizeof(old)), &config, sizeof(config));
    assert(fwrite(&old, sizeof(old), 1, backup) == 1 && fwrite(&config, sizeof(config), 1, backup) == 1);
    assert(fclose(backup) == 0);
    restart_messages();
    assert(solar_os_messaging_provider_get_status(a, &status) == ESP_OK && status.removed);
    assert(solar_os_messaging_profile_load(a, &restored, sizeof(restored)) == ESP_ERR_NOT_FOUND);
    assert(solar_os_messaging_message_get(pending_a, &message) == ESP_ERR_NOT_FOUND);
    assert(strcmp(solar_os_messaging_provider_name(a), "network-a") == 0);
    solar_os_messaging_provider_id_t replacement;
    assert(solar_os_messaging_instance_create("test-net", "network-a", &config, sizeof(config), &replacement) == ESP_OK);
    assert(replacement > b && solar_os_messaging_provider_find("network-a") == replacement);
    restart_messages();
    assert(solar_os_messaging_provider_find("network-a") == replacement);
    assert(solar_os_messaging_provider_get_status(a, &status) == ESP_OK && status.removed);
    assert(solar_os_messaging_instance_remove(a) == ESP_OK);
    assert(access(backup_path, F_OK) != 0);
    char path[160];
    assert(solar_os_storage_default_path(".chat/profiles/006.bin", path, sizeof(path)) == ESP_OK); assert(unlink(path) == 0);
    assert(solar_os_storage_default_path(".chat/profiles/004.bin", path, sizeof(path)) == ESP_OK); assert(unlink(path) == 0);
    assert(solar_os_storage_default_path(".chat/profiles/005.bin", path, sizeof(path)) == ESP_OK); assert(unlink(path) == 0);
    assert(solar_os_storage_default_path(".chat/profiles", path, sizeof(path)) == ESP_OK); assert(rmdir(path) == 0);
    assert(solar_os_storage_default_path(".chat", path, sizeof(path)) == ESP_OK); assert(rmdir(path) == 0);
}

static void test_unbound_and_evicted_removal(void)
{
    unsigned config = 0x98765432;
    solar_os_messaging_provider_id_t id, unsupported;
    assert(solar_os_messaging_instance_create("absent-class", "unbound", &config, sizeof(config), &id) == ESP_OK);
    solar_os_conversation_id_t room;
    solar_os_messaging_conversation_upsert_t request = {.provider = id, .provider_key = "#room",
        .title = "#room", .kind = SOLAR_OS_CONVERSATION_ROOM};
    assert(solar_os_messaging_conversation_upsert(&request, &room) == ESP_OK);
    solar_os_message_key_t queued;
    assert(solar_os_messaging_send(room, "will be evicted", false, &queued) == ESP_OK);
    for (unsigned i = 0; i < SOLAR_OS_MESSAGING_MESSAGE_CAPACITY + 1; i++) {
        solar_os_messaging_inbound_t inbound = {.provider = id, .provider_message_key = i + 1,
            .conversation_key = "#room", .conversation_title = "#room", .conversation_kind = SOLAR_OS_CONVERSATION_ROOM,
            .sender = "remote", .body = "new inbound"};
        assert(solar_os_messaging_publish_inbound(&inbound, NULL, NULL) == ESP_OK);
    }
    solar_os_messaging_message_t message;
    assert(solar_os_messaging_message_get(queued, &message) == ESP_ERR_NOT_FOUND);
    solar_os_messaging_outbound_t *out = calloc(1, sizeof(*out)); assert(out);
    assert(solar_os_messaging_outbox_peek(id, out) == ESP_OK);
    assert(solar_os_messaging_instance_remove(id) == ESP_OK);
    assert(solar_os_messaging_outbox_peek(id, out) == ESP_ERR_NOT_FOUND);
    assert(solar_os_messaging_outbox_snapshot(out, 1) == 0);
    free(out);
    static const solar_os_messaging_provider_class_t no_remove = {.name = "no-remove"};
    assert(solar_os_messaging_class_register(&no_remove) == ESP_OK);
    assert(solar_os_messaging_instance_create("no-remove", "unsupported", &config, sizeof(config), &unsupported) == ESP_OK);
    assert(solar_os_messaging_instance_register(unsupported, "no-remove", "unsupported", &test_joins) == ESP_OK);
    assert(solar_os_messaging_instance_remove(unsupported) == ESP_ERR_NOT_SUPPORTED);
    unsigned restored = 0;
    assert(solar_os_messaging_profile_load(unsupported, &restored, sizeof(restored)) == ESP_OK && restored == config);
    /* Reset after committing the removal marker but before purging history. */
    solar_os_messaging_provider_id_t interrupted;
    assert(solar_os_messaging_instance_create("absent-class", "interrupted", &config, sizeof(config), &interrupted) == ESP_OK);
    solar_os_messaging_inbound_t inbound = {.provider = interrupted, .provider_message_key = 1,
        .conversation_key = "#reset", .conversation_title = "#reset", .conversation_kind = SOLAR_OS_CONVERSATION_ROOM,
        .sender = "remote", .body = "must disappear after reset"};
    solar_os_message_key_t removed_key;
    assert(solar_os_messaging_publish_inbound(&inbound, NULL, &removed_key) == ESP_OK);
    solar_os_messaging_provider_status_t status;
    assert(solar_os_messaging_provider_get_status(interrupted, &status) == ESP_OK);
    bool committed = false;
    assert(messaging_archive_profile(&status, &committed) == ESP_OK && committed);
    restart_messages();
    assert(solar_os_messaging_message_get(removed_key, &message) == ESP_ERR_NOT_FOUND);
    assert(solar_os_messaging_provider_find("interrupted") == 0);
    char path[160], relative[64];
    const solar_os_messaging_provider_id_t ids[] = {id, unsupported, interrupted};
    for (size_t i = 0; i < 3; i++) {
        snprintf(relative, sizeof(relative), ".chat/profiles/%03u.bin", ids[i]);
        assert(solar_os_storage_default_path(relative, path, sizeof(path)) == ESP_OK && unlink(path) == 0);
    }
    assert(solar_os_storage_default_path(".chat/profiles", path, sizeof(path)) == ESP_OK && rmdir(path) == 0);
    assert(solar_os_storage_default_path(".chat", path, sizeof(path)) == ESP_OK && rmdir(path) == 0);
}

int main(void)
{
    assert(mkdtemp(test_root));
    test_contacts_migration_and_import();
    test_channel_ownership_and_restart();
    test_provider_instances();
    test_unbound_and_evicted_removal();
    char path[160];
    const char *paths[] = { ".contacts/contacts.bin", ".messages/messages.bin", ".contacts", ".messages" };
    for (size_t i = 0; i < 4; i++) {
        assert(solar_os_storage_default_path(paths[i], path, sizeof(path)) == ESP_OK);
        assert((i < 2 ? unlink(path) : rmdir(path)) == 0);
    }
    assert(rmdir(test_root) == 0);
    puts("messaging service tests: ok");
    return 0;
}
