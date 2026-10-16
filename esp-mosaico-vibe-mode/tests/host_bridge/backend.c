#include "backend_sdk.h"
#include <assert.h>
#include BACKEND_SOURCE

_Atomic int64_t mock_time;
bool mock_network, mock_stop_on_delay;
int mock_create_fail, mock_alloc_fail, mock_commit_error, mock_writes, mock_commits,
    mock_reserved, mock_abort;
void (*mock_worker)(void *);
esp_partition_t mock_factory = {
    .address = 0x20000, .size = 0x1c0000, .subtype = ESP_PARTITION_SUBTYPE_APP_TEST, .label = "vibe_mode"};
void *esp_flash_default_chip;
static uint8_t flash[0x1000000];
static int erased, persisted, corrupt_readback;
static unsigned image_checks, boot_checks, boot_selections;
static bool fail_image;
static uint32_t last_write, last_progress, progress_samples;
static int fail_write_at;
static bool write_protected = true;
static const esp_partition_t *boot;
static size_t cjson_allocations;

static void *counted_cjson_malloc(size_t size)
{
    void *memory = malloc(size);
    if (memory != NULL)
        ++cjson_allocations;
    return memory;
}

static void counted_cjson_free(void *memory)
{
    if (memory != NULL)
        --cjson_allocations;
    free(memory);
}

void vTaskDelay(unsigned ms)
{
    if (ms == 1 && s_backend.status.commit_total) {
        factory_system_update_status_t status;
        assert(factory_system_update_get_status(&status) == ESP_OK);
        assert(status.commit_written > last_progress && status.commit_written <= status.commit_total);
        assert(status.commit_written - last_progress <= 64 * 1024);
        last_progress = status.commit_written;
        ++progress_samples;
    }
}
void vTaskDelete(void *task)
{
    (void)task;
}
void esp_restart(void)
{
    assert(false);
}
esp_err_t esp_flash_read(void *chip, void *buf, uint32_t offset, size_t size)
{
    (void)chip;
    assert(offset + size <= sizeof(flash));
    memcpy(buf, flash + offset, size);
    if (corrupt_readback && offset >= 0x200000 && size)
        ((uint8_t *)buf)[0] ^= 1;
    return 0;
}
esp_err_t esp_partition_table_verify(const esp_partition_info_t *entries, bool checksum,
                                     int *count)
{
    (void)checksum;
    *count = 0;
    while (entries[*count].magic == ESP_PARTITION_MAGIC)
        (*count)++;
    return 0;
}
esp_err_t esp_partition_erase_range(const esp_partition_t *p, uint32_t offset,
                                    size_t size)
{
    assert(p->address >= 0x200000);
    assert(offset + size <= p->size);
    memset(flash + p->address + offset, 0xff, size);
    erased++;
    return 0;
}
esp_err_t esp_partition_write(const esp_partition_t *p, uint32_t offset,
                              const void *data, size_t size)
{
    assert(offset + size <= p->size);
    memcpy(flash + p->address + offset, data, size);
    mock_writes++;
    return 0;
}
esp_err_t esp_flash_set_dangerous_write_protection(void *chip, bool enabled)
{
    (void)chip;
    write_protected = enabled;
    return 0;
}
esp_err_t esp_flash_erase_region(void *chip, uint32_t offset, size_t size)
{
    (void)chip;
    assert(boot == &mock_factory);
    memset(flash + offset, 0xff, size);
    erased++;
    return 0;
}
esp_err_t esp_flash_write(void *chip, const void *data, uint32_t offset, size_t size)
{
    (void)chip;
    if (fail_write_at && mock_writes + 1 == fail_write_at) return ESP_FAIL;
    memcpy(flash + offset, data, size);
    last_write = offset;
    mock_writes++;
    return 0;
}
esp_err_t esp_image_verify(int mode, const esp_partition_pos_t *pos,
                           esp_image_metadata_t *metadata)
{
    (void)mode;
    (void)pos;
    (void)metadata;
    ++image_checks;
    return fail_image ? ESP_ERR_INVALID_CRC : ESP_OK;
}
esp_err_t esp_image_verify_bootloader(uint32_t *size)
{
    *size = 0;
    return 0;
}
const esp_partition_t *esp_partition_find_first(int type, int subtype,
                                                const char *label)
{
    (void)type;
    (void)subtype;
    return !strcmp(label, "vibe_mode") ? &mock_factory : NULL;
}
const esp_partition_t *esp_ota_get_boot_partition(void)
{
    return boot;
}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p)
{
    ++boot_checks;
    return esp_ota_set_boot_partition_skip_validate(p);
}
esp_err_t esp_ota_set_boot_partition_skip_validate(const esp_partition_t *p)
{
    ++boot_selections;
    boot = p;
    return ESP_OK;
}
esp_err_t esp_iris_crash_loop_reset(void)
{
    return 0;
}
esp_err_t esp_iris_mark_planned_restart(void)
{
    return 0;
}
esp_err_t factory_system_metadata_store_last_result(const uint8_t *op, esp_err_t result)
{
    (void)op;
    (void)result;
    persisted++;
    return mock_commit_error;
}
esp_err_t
esp_iris_system_update_register(const esp_iris_system_update_backend_t *backend)
{
    (void)backend;
    return 0;
}
esp_err_t factory_system_update_nand_register(void)
{
    return 0;
}
static uint8_t op[16] = {1};
static void setup(void)
{
    s_backend.layout_needs_reload = false;
    s_backend.status.commit_written = s_backend.status.commit_total = 0;
    last_progress = progress_samples = fail_write_at = 0;
    write_protected = true;
    update_state_reset(&s_backend);
    update_owner_release(&s_backend);
    memset(flash, 0xff, sizeof(flash));
    erased = mock_writes = persisted = corrupt_readback = mock_alloc_fail =
        mock_commit_error = 0;
    boot = NULL;
    image_checks = boot_checks = boot_selections = 0;
    fail_image = false;
    esp_partition_info_t *entries = (void *)(flash + 0x8000);
    for (size_t i = 0; i < 5; i++) {
        const immutable_partition_contract_t *c = &s_immutable_partitions[i];
        entries[i] = (esp_partition_info_t){.magic = ESP_PARTITION_MAGIC,
                                            .type = c->type,
                                            .subtype = c->subtype,
                                            .pos = {c->offset, c->size}};
        strncpy(entries[i].label, c->label, 16);
    }
    entries[5] = (esp_partition_info_t){.magic = ESP_PARTITION_MAGIC,
                                        .type = ESP_PARTITION_TYPE_DATA,
                                        .subtype = 0x82,
                                        .pos = {0x200000, 0x10000},
                                        .label = "data"};
    entries[6] = (esp_partition_info_t){.magic = ESP_PARTITION_MAGIC,
                                        .type = ESP_PARTITION_TYPE_APP,
                                        .subtype = 16,
                                        .pos = {0x210000, 0x100000},
                                        .label = "main_app"};
}
static char *manifest(bool preserve, const char *kind, uint32_t offset, uint32_t size)
{
    uint8_t digest[32];
    char hash[65];
    update_hash_flash_region(0x8000, 4096, digest);
    for (int i = 0; i < 32; i++)
        sprintf(hash + 2 * i, "%02x", digest[i]);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "schema", FACTORY_SYSTEM_SCHEMA);
    cJSON *target = cJSON_AddObjectToObject(root, "target");
    cJSON_AddNumberToObject(target, "chip_id", 32);
    cJSON_AddNumberToObject(target, "flash_size", sizeof(flash));
    cJSON_AddStringToObject(root, "target_layout_sha256", hash);
    cJSON_AddBoolToObject(root, "preserve_layout", preserve);
    cJSON *items = cJSON_AddArrayToObject(root, "components"),
          *c = cJSON_CreateObject();
    cJSON_AddItemToArray(items, c);
    cJSON_AddNumberToObject(c, "id", 1);
    cJSON_AddStringToObject(c, "kind", kind);
    cJSON_AddNumberToObject(c, "target_offset", offset);
    cJSON_AddNumberToObject(c, "size", size);
    size_t written;
    psa_hash_compute(PSA_ALG_SHA_256, "data", 4, digest, 32, &written);
    for (int i = 0; i < 32; i++)
        sprintf(hash + 2 * i, "%02x", digest[i]);
    cJSON_AddStringToObject(c, "sha256", hash);
    cJSON_AddStringToObject(c, "file", "image.bin");
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}
static char *recovery_manifest(const uint8_t *image)
{
    uint8_t sha[32];
    update_hash_memory(image, 0x1c0000, sha);
    char *json = manifest(true, "recovery", 0x20000, 0x1c0000);
    cJSON *root = cJSON_Parse(json);
    free(json);
    char hash[65];
    for (int i = 0; i < 32; i++)
        sprintf(hash + 2 * i, "%02x", sha[i]);
    cJSON_ReplaceItemInObject(
        cJSON_GetArrayItem(cJSON_GetObjectItem(root, "components"), 0), "sha256",
        cJSON_CreateString(hash));
    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}
static esp_err_t prepare(factory_system_update_owner_t owner, char *json)
{
    esp_err_t err =
        factory_system_update_source_prepare(owner, (uint8_t *)json, strlen(json), op);
    free(json);
    return err;
}
static uint8_t new_table[4096];
static char *layout_manifest(bool complete)
{
    memcpy(new_table, flash + 0x8000, 4096);
    ((esp_partition_info_t *)new_table)[5].pos.offset = 0x310000;
    char *json = manifest(false, "partition_table", 0x8000, 4096);
    cJSON *root = cJSON_Parse(json);
    free(json);
    uint8_t digest[32];
    char hash[65];
    update_hash_memory(new_table, 4096, digest);
    for (int i = 0; i < 32; i++)
        sprintf(hash + 2 * i, "%02x", digest[i]);
    cJSON_ReplaceItemInObject(root, "target_layout_sha256", cJSON_CreateString(hash));
    cJSON *items = cJSON_GetObjectItem(root, "components");
    cJSON_ReplaceItemInObject(cJSON_GetArrayItem(items, 0), "sha256",
                              cJSON_CreateString(hash));
    for (int i = 0; i < (complete ? 2 : 1); i++) {
        char *extra =
            manifest(false, i ? "application" : "data", i ? 0x210000 : 0x310000, 4);
        cJSON *other = cJSON_Parse(extra);
        free(extra);
        cJSON *c =
            cJSON_DetachItemFromArray(cJSON_GetObjectItem(other, "components"), 0);
        cJSON_ReplaceItemInObject(c, "id", cJSON_CreateNumber(i + 2));
        cJSON_ReplaceItemInObject(c, "file",
                                  cJSON_CreateString(i ? "app.bin" : "data.bin"));
        cJSON_AddItemToArray(items, c);
        cJSON_Delete(other);
    }
    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static char *layout_manifest_without_data(void)
{
    char *json = layout_manifest(true);
    cJSON *root = cJSON_Parse(json);
    free(json);
    cJSON_DeleteItemFromArray(cJSON_GetObjectItem(root, "components"), 1);
    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static char *bootloader_manifest(void)
{
    char *json = layout_manifest(true);
    cJSON *root = cJSON_Parse(json);
    free(json);
    json = manifest(false, "bootloader", 0x2000, 0x6000);
    cJSON *other = cJSON_Parse(json);
    free(json);
    cJSON *component = cJSON_DetachItemFromArray(cJSON_GetObjectItem(other, "components"), 0);
    cJSON_ReplaceItemInObject(component, "id", cJSON_CreateNumber(4));
    cJSON_ReplaceItemInObject(component, "file", cJSON_CreateString("bootloader.bin"));
    cJSON_AddItemToArray(cJSON_GetObjectItem(root, "components"), component);
    cJSON_Delete(other);
    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static esp_err_t prepare_bridge(char *json, bool allow_bootloader)
{
    cJSON *root = cJSON_Parse(json);
    assert(root);
    esp_err_t err = factory_system_update_source_prepare_bridge(
        root, op, allow_bootloader);
    cJSON_Delete(root);
    free(json);
    return err;
}

static bool ota_active;
static esp_err_t ota_status_error;
esp_err_t esp_iris_ota_get_status(esp_iris_ota_status_t *status) { status->active = ota_active; return ota_status_error; }
int main(void)
{
    ota_active = true;
    assert(factory_system_update_read_acquire() == ESP_ERR_INVALID_STATE);
    assert(!factory_system_update_writer_busy());
    ota_active = false;
    assert(factory_system_update_read_acquire() == ESP_OK);
    assert(factory_system_update_writer_busy());
    assert(factory_system_update_read_acquire() == ESP_ERR_INVALID_STATE);
    factory_system_update_read_release();
    assert(!factory_system_update_writer_busy());
    ota_status_error = ESP_ERR_NOT_FOUND;
    assert(factory_system_update_read_acquire() == ESP_OK);
    factory_system_update_read_release();
    ota_status_error = ESP_FAIL;
    assert(factory_system_update_read_acquire() == ESP_FAIL);
    assert(!factory_system_update_writer_busy());
    ota_status_error = ESP_OK;
    /* A parsed prefix followed by trailing bytes must not leak its cJSON tree. */
    setup();
    char *trailing = manifest(true, "data", 0x200000, 4);
    size_t trailing_size = strlen(trailing);
    trailing = realloc(trailing, trailing_size + 2);
    assert(trailing);
    trailing[trailing_size++] = 'x';
    trailing[trailing_size] = '\0';
    cJSON_Hooks hooks = {
        .malloc_fn = counted_cjson_malloc,
        .free_fn = counted_cjson_free,
    };
    cjson_allocations = 0;
    cJSON_InitHooks(&hooks);
    for (int i = 0; i < 32; ++i) {
        assert(factory_system_update_source_prepare(
                   FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE,
                   (uint8_t *)trailing, trailing_size, op) ==
               ESP_ERR_INVALID_ARG);
        assert(cjson_allocations == 0);
    }
    cJSON_InitHooks(NULL);
    free(trailing);

    /* Published Ideas manifests may end in JSON whitespace. The NAND install
     * parser must accept it without accepting another value or control bytes. */
    const struct {
        const char *suffix;
        size_t size;
        esp_err_t expected;
    } endings[] = {
        {"", 0, ESP_OK},
        {"\n", 1, ESP_OK},
        {"\r\n", 2, ESP_OK},
        {" \t\r\n", 4, ESP_OK},
        {" \nx", 3, ESP_ERR_INVALID_ARG},
        {"\n{}", 3, ESP_ERR_INVALID_ARG},
        {"\n\0", 2, ESP_ERR_INVALID_ARG},
        {"\v", 1, ESP_ERR_INVALID_ARG},
        {"\f", 1, ESP_ERR_INVALID_ARG},
    };
    for (size_t i = 0; i < sizeof(endings) / sizeof(endings[0]); ++i) {
        setup();
        char *json = layout_manifest_without_data();
        size_t size = strlen(json);
        json = realloc(json, size + endings[i].size + 1);
        assert(json);
        memcpy(json + size, endings[i].suffix, endings[i].size);
        size += endings[i].size;
        json[size] = '\0';
        assert(factory_system_update_source_reserve_idea(op) == ESP_OK);
        esp_err_t result = factory_system_update_source_prepare(
            FACTORY_SYSTEM_UPDATE_OWNER_NAND, (uint8_t *)json, size, op);
        if (result != endings[i].expected)
            fprintf(stderr, "manifest ending %zu: expected %d, got %d\n", i, endings[i].expected, result);
        assert(result == endings[i].expected);
        assert(erased == 0 && mock_writes == 0);
        free(json);
    }

    setup();
    assert(factory_system_update_source_reserve(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE,
                                                op) == 0);
    assert(factory_system_update_source_reserve(FACTORY_SYSTEM_UPDATE_OWNER_NAND, op) ==
           ESP_ERR_INVALID_STATE);
    factory_system_update_source_abort(FACTORY_SYSTEM_UPDATE_OWNER_NAND, op, ESP_FAIL);
    assert(update_owner_is(&s_backend, FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE));
    factory_system_update_source_abort(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE, op,
                                       ESP_FAIL);
    assert(update_owner_is(&s_backend, FACTORY_SYSTEM_UPDATE_OWNER_NONE));
    /* JSON alone cannot opt USB/NAND into the remote policy. */
    setup();
    assert(prepare(FACTORY_SYSTEM_UPDATE_OWNER_NAND,
                   manifest(true, "data", 0x200000, 4)) != 0);
    assert(!erased);
    setup();
    char *json = manifest(true, "data", 0x200000, 4);
    cJSON *root = cJSON_Parse(json);
    free(json);
    cJSON_AddBoolToObject(root, "remote_bridge", true);
    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    assert(prepare(FACTORY_SYSTEM_UPDATE_OWNER_NAND, json) != 0);
    assert(!erased);
    setup();
    assert(prepare(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE,
                   manifest(true, "data", 0xc000, 4)) != 0);
    assert(!erased);
    setup();
    assert(prepare(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE,
                   manifest(true, "application", 0x200000, 4)) != 0);
    assert(!erased);
    /* Bridge APP erases only complete sectors covering the image. Tail and
     * adjacent partitions survive; DATA still clears its entire partition. */
    for (size_t image_size = 4096; image_size <= 4097; image_size++) {
        setup();
        memset(flash + 0x210000, 0x55, 0x100000);
        assert(prepare(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE,
                       manifest(true, "application", 0x210000, image_size)) == ESP_OK);
        esp_iris_system_update_component_t app_component = s_backend.transaction.plan[0].descriptor;
        assert(update_begin_component(&app_component, &s_backend) == ESP_OK);
        size_t rounded = (image_size + 4095) & ~4095U;
        assert(flash[0x210000] == 0xff && flash[0x210000 + rounded - 1] == 0xff);
        assert(flash[0x210000 + rounded] == 0x55 && flash[0x30ffff] == 0x55);
    }
    setup();
    memset(flash + 0x200000, 0x55, 0x10000);
    assert(prepare(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE,
                   manifest(true, "data", 0x200000, 4)) == ESP_OK);
    esp_iris_system_update_component_t data_component = s_backend.transaction.plan[0].descriptor;
    assert(update_begin_component(&data_component, &s_backend) == ESP_OK);
    assert(flash[0x200000] == 0xff && flash[0x20ffff] == 0xff);
    /* Actual source table hash and protected prefix are independently checked. */
    setup();
    json = manifest(true, "data", 0x200000, 4);
    flash[0x8000 + 12] = 'X';
    assert(prepare(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE, json) != 0);
    assert(!erased);
    setup();
    ((esp_partition_info_t *)(flash + 0x8000))[0].pos.offset = 0xa000;
    assert(prepare(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE,
                   manifest(true, "data", 0x200000, 4)) != 0);
    assert(!erased);
    /* Preserve-layout data update writes only selected partition, verifies readback. */
    setup();
    assert(prepare(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE,
                   manifest(true, "data", 0x200000, 4)) == 0);
    esp_iris_system_update_component_t c = s_backend.transaction.plan[0].descriptor;
    assert(factory_system_update_source_begin_component(
               FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE, &c) == 0);
    assert(factory_system_update_source_write_component(
               FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE, &c, 0, (uint8_t *)"data", 4) == 0);
    assert(factory_system_update_source_end_component(
               FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE, &c, c.sha256) == 0);
    assert(factory_system_update_source_commit(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE,
                                               op) == 0);
    assert(erased == 1 && mock_writes == 1 && boot == &mock_factory && persisted == 1);
    assert(
        factory_system_update_source_needs_restart(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE));
    setup();
    assert(prepare(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE,
                   manifest(true, "data", 0x200000, 4)) == 0);
    c = s_backend.transaction.plan[0].descriptor;
    assert(update_begin_component(&c, &s_backend) == 0);
    assert(update_write_component(&c, 0, (uint8_t *)"data", 4, &s_backend) == 0);
    corrupt_readback = 1;
    assert(update_end_component(&c, c.sha256, &s_backend) == ESP_ERR_INVALID_CRC);
    assert(update_commit_update(op, &s_backend) != 0);
    /* Factory staging failure cannot erase the running image. */
    setup();
    assert(prepare(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE,
                   manifest(true, "recovery", 0x20000, 0x1c0000)) == 0);
    mock_alloc_fail = 1;
    c = s_backend.transaction.plan[0].descriptor;
    assert(update_begin_component(&c, &s_backend) == ESP_ERR_NO_MEM);
    assert(!erased && !mock_writes);
    /* A malformed staged Recovery is rejected before dangerous writes. */
    mock_alloc_fail = 0;
    assert(update_begin_component(&c, &s_backend) == 0);
    memset(s_backend.transaction.recovery_image, 0, 0x1c0000);
    s_backend.transaction.received = 0x1c0000;
    assert(update_end_component(&c, c.sha256, &s_backend) == ESP_ERR_IMAGE_INVALID);
    assert(!erased && !mock_writes);
    /* Layout still requires every mutable application image. */
    setup();
    assert(prepare(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE, layout_manifest(false)) == 0);
    c = s_backend.transaction.plan[0].descriptor;
    assert(update_begin_component(&c, &s_backend) == 0);
    assert(update_write_component(&c, 0, new_table, 4096, &s_backend) == 0);
    assert(update_end_component(&c, c.sha256, &s_backend) != 0);
    assert(!erased);

    /* Omitted filesystem data is allowed, including when the target moves.
     * No implicit initialization/erase: untouched source and target bytes stay
     * intact. Only the explicit app and final table writes are performed. */
    const uint8_t data_subtypes[] = {0x81, 0x82, 0x83};
    for (size_t subtype = 0; subtype < sizeof(data_subtypes); ++subtype) {
        setup();
        ((esp_partition_info_t *)(flash + 0x8000))[5].subtype =
            data_subtypes[subtype];
        memset(flash + 0x200000, 0x55, 0x10000);
        memset(flash + 0x310000, 0x66, 0x10000);
        assert(prepare(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE,
                       layout_manifest_without_data()) == ESP_OK);
        for (size_t i = 0; i < 2; i++) {
            c = s_backend.transaction.plan[i].descriptor;
            assert(update_begin_component(&c, &s_backend) == ESP_OK);
            const uint8_t *data = i ? (const uint8_t *)"data" : new_table;
            assert(update_write_component(&c, 0, data, c.size, &s_backend) == ESP_OK);
            assert(update_end_component(&c, c.sha256, &s_backend) == ESP_OK);
        }
        assert(update_commit_update(op, &s_backend) == ESP_OK);
        assert(erased == 2 && mock_writes == 2 && last_write == 0x8000);
        for (size_t i = 0; i < 0x10000; ++i) {
            assert(flash[0x200000 + i] == 0x55);
            assert(flash[0x310000 + i] == 0x66);
        }
    }

    /* An omitted NVS image remains supported. */
    setup();
    esp_partition_info_t *mutable =
        &((esp_partition_info_t *)(flash + 0x8000))[5];
    mutable->subtype = ESP_PARTITION_SUBTYPE_DATA_NVS;
    strncpy(mutable->label, "nvs", sizeof(mutable->label));
    assert(prepare(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE,
                   layout_manifest_without_data()) == 0);
    for (size_t i = 0; i < 2; i++) {
        c = s_backend.transaction.plan[i].descriptor;
        assert(update_begin_component(&c, &s_backend) == 0);
        const uint8_t *data = i ? (const uint8_t *)"data" : new_table;
        assert(update_write_component(&c, 0, data, c.size, &s_backend) == 0);
        assert(update_end_component(&c, c.sha256, &s_backend) == 0);
    }
    assert(update_commit_update(op, &s_backend) == 0);
    assert(last_write == 0x8000);

    setup();
    assert(prepare(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE, layout_manifest(true)) == 0);
    factory_system_update_status_t telemetry;
    assert(factory_system_update_get_status(&telemetry) == ESP_OK);
    assert(telemetry.total_size == 4096 + 4 + 4 && telemetry.received_size == 0);
    uint64_t accepted = 0;
    for (size_t i = 0; i < 3; i++) {
        c = s_backend.transaction.plan[i].descriptor;
        assert(update_begin_component(&c, &s_backend) == 0);
        assert(factory_system_update_get_status(&telemetry) == ESP_OK);
        assert(telemetry.received_size == accepted && telemetry.completed_size == accepted);
        const uint8_t *data = i ? (const uint8_t *)"data" : new_table;
        assert(update_write_component(&c, 0, data, c.size / 2, &s_backend) == 0);
        assert(factory_system_update_get_status(&telemetry) == ESP_OK);
        assert(telemetry.received_size == accepted + c.size / 2);
        assert(update_write_component(&c, 0, data, c.size / 2, &s_backend) != ESP_OK);
        assert(factory_system_update_get_status(&telemetry) == ESP_OK);
        assert(telemetry.received_size == accepted + c.size / 2); /* rejected replay */
        assert(update_write_component(&c, c.size / 2, data + c.size / 2, c.size - c.size / 2, &s_backend) == 0);
        accepted += c.size;
        assert(update_end_component(&c, c.sha256, &s_backend) == 0);
        assert(factory_system_update_get_status(&telemetry) == ESP_OK);
        assert(telemetry.received_size == accepted && telemetry.completed_size == accepted);
        assert(telemetry.update.completed_components == i + 1);
        assert(!memcmp(flash + 0x8000 + 5 * 32 + 4, "\0\0\x20\0", 4));
    }
    assert(update_commit_update(op, &s_backend) == 0);
    assert(last_write == 0x8000);
    assert(!memcmp(flash + 0x8000, new_table, 4096));
    /* A complete Recovery image validates in PSRAM before its single copy commit. */
    setup();
    uint8_t *image = malloc(0x1c0000);
    memset(image, 0xff, 0x1c0000);
    esp_image_header_t header = {
        .magic = ESP_IMAGE_HEADER_MAGIC, .segment_count = 1, .chip_id = 32};
    memcpy(image, &header, sizeof(header));
    esp_image_segment_header_t segment = {.data_len = sizeof(esp_app_desc_t)};
    memcpy(image + sizeof(header), &segment, sizeof(segment));
    esp_app_desc_t app = {.magic_word = ESP_APP_DESC_MAGIC_WORD,
                          .version = "0.1.2"};
    memcpy(image + sizeof(header) + sizeof(segment), &app, sizeof(app));
    size_t end = (sizeof(header) + sizeof(segment) + sizeof(app) + 1 + 15) &
                 ~(size_t)15;
    uint8_t checksum = 0xef;
    for (size_t i = 0; i < sizeof(app); i++)
        checksum ^= ((const uint8_t *)&app)[i];
    image[end - 1] = checksum;
    assert(prepare(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE,
                   recovery_manifest(image)) == 0);
    c = s_backend.transaction.plan[0].descriptor;
    assert(update_begin_component(&c, &s_backend) == 0);
    assert(update_write_component(&c, 0, image, c.size, &s_backend) == 0);
    assert(update_end_component(&c, c.sha256, &s_backend) == 0);
    assert(!erased);
    mock_commit_error = ESP_FAIL;
    assert(update_commit_update(op, &s_backend) == ESP_FAIL);
    assert(
        factory_system_update_source_needs_restart(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE));
    assert(last_write == 0x1d0000 && !memcmp(flash + 0x20000, image, 0x1c0000));
    /* Online firmware has its own owner and cannot install application/layout plans. */
    setup();
    assert(prepare(FACTORY_SYSTEM_UPDATE_OWNER_FIRMWARE, layout_manifest(true)) != ESP_OK);
    setup();
    char *online_manifest = recovery_manifest(image);
    cJSON *online_root = cJSON_Parse(online_manifest);
    free(online_manifest);
    cJSON_DeleteItemFromObject(online_root, "preserve_layout");
    online_manifest = cJSON_PrintUnformatted(online_root);
    cJSON_Delete(online_root);
    assert(factory_system_update_source_reserve(FACTORY_SYSTEM_UPDATE_OWNER_FIRMWARE, op) == ESP_OK);
    assert(prepare(FACTORY_SYSTEM_UPDATE_OWNER_FIRMWARE, online_manifest) == ESP_OK);
    c = s_backend.transaction.plan[0].descriptor;
    assert(factory_system_update_source_begin_component(FACTORY_SYSTEM_UPDATE_OWNER_FIRMWARE, &c) == ESP_OK);
    assert(factory_system_update_source_write_component(FACTORY_SYSTEM_UPDATE_OWNER_FIRMWARE, &c, 0, image, c.size) == ESP_OK);
    assert(factory_system_update_source_end_component(FACTORY_SYSTEM_UPDATE_OWNER_FIRMWARE, &c, c.sha256) == ESP_OK);
    assert(!erased);
    assert(factory_system_update_source_commit(FACTORY_SYSTEM_UPDATE_OWNER_FIRMWARE, op) == ESP_OK);
    assert(last_write == 0x1d0000 && !memcmp(flash + 0x20000, image, 0x1c0000));
    assert(progress_samples == 28 && last_progress == c.size && write_protected);
    int writes_before = mock_writes;
    assert(update_commit_protected_image(&s_backend, &s_backend.transaction.plan[0], image) == ESP_OK);
    assert(mock_writes == writes_before && s_backend.status.commit_written == c.size);
    flash[0x20000] ^= 1;
    last_progress = progress_samples = 0;
    fail_write_at = mock_writes + 3;
    assert(update_commit_protected_image(&s_backend, &s_backend.transaction.plan[0], image) == ESP_FAIL);
    assert(s_backend.status.commit_written == 2 * 65536 && progress_samples == 2 && write_protected);
    /* Older Recovery images cannot replace the 0.1.2 release. */
    setup();
    esp_app_desc_t *old_app =
        (void *)(image + sizeof(header) + sizeof(segment));
    strcpy(old_app->version, "2.8.5-recovery");
    checksum = 0xef;
    for (size_t i = 0; i < sizeof(*old_app); i++)
        checksum ^= ((const uint8_t *)old_app)[i];
    image[end - 1] = checksum;
    assert(prepare(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE,
                   recovery_manifest(image)) == 0);
    c = s_backend.transaction.plan[0].descriptor;
    assert(update_begin_component(&c, &s_backend) == 0);
    assert(update_write_component(&c, 0, image, c.size, &s_backend) == 0);
    assert(update_end_component(&c, c.sha256, &s_backend) == ESP_ERR_INVALID_VERSION);
    assert(!erased && !mock_writes);
    free(image);
    /* Only an explicit local Bridge policy may grant bootloader replacement. */
    setup();
    assert(prepare(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE, bootloader_manifest()) ==
           ESP_ERR_NOT_SUPPORTED);
    assert(!erased && !mock_writes);
    setup();
    assert(prepare_bridge(bootloader_manifest(), false) == ESP_ERR_NOT_SUPPORTED);
    assert(!erased && !mock_writes);
    setup();
    assert(prepare_bridge(bootloader_manifest(), true) == ESP_OK);
    assert(update_owner_is(&s_backend, FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE) && s_backend.transaction.plan_count == 4);
    assert(!erased && !mock_writes);
    /* Remote JSON cannot override the generic API's local policy. */
    setup();
    json = bootloader_manifest();
    root = cJSON_Parse(json);
    free(json);
    cJSON_AddBoolToObject(root, "allow_bootloader", true);
    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    assert(prepare(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE, json) == ESP_ERR_NOT_SUPPORTED);
    assert(!erased && !mock_writes);
    /* Even local opt-in requires the matching partition table. */
    setup();
    assert(prepare_bridge(manifest(true, "bootloader", 0x2000, 0x6000), true) != ESP_OK);
    assert(!erased && !mock_writes);
    /* Manifest-only policy remains centralized in the shared backend. */
    setup();
    assert(prepare(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE,
                   manifest(true, "unknown", 0x200000, 4096)) != ESP_OK);
    assert(!erased && !mock_writes);
    setup();
    json = layout_manifest(true);
    root = cJSON_Parse(json);
    free(json);
    cJSON *components = cJSON_GetObjectItem(root, "components");
    cJSON_AddItemToArray(components, cJSON_DetachItemFromArray(components, 0));
    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    assert(prepare(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE, json) != ESP_OK);
    assert(!erased && !mock_writes);
    /* Source-neutral ownership and Recovery version checks still apply. */
    setup();
    assert(factory_system_update_source_reserve(FACTORY_SYSTEM_UPDATE_OWNER_NAND, op) == ESP_OK);
    assert(prepare_bridge(bootloader_manifest(), true) == ESP_ERR_INVALID_STATE);
    assert(update_owner_is(&s_backend, FACTORY_SYSTEM_UPDATE_OWNER_NAND));
    assert(!erased && !mock_writes);
    setup();
    json = bootloader_manifest();
    root = cJSON_Parse(json);
    free(json);
    cJSON_AddStringToObject(root, "minimum_recovery_version", "1.0");
    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    assert(prepare_bridge(json, true) == ESP_ERR_INVALID_VERSION);
    assert(!erased && !mock_writes);
    /* Offline Ideas install uses the validated target descriptor and does not
     * create a restart task. The cached old layout cannot admit another writer. */
    setup();
    assert(prepare(FACTORY_SYSTEM_UPDATE_OWNER_NAND, layout_manifest_without_data()) == ESP_OK);
    assert(mosaico_boot_request_recovery() == ESP_OK);
    assert(factory_system_update_source_commit_deferred(op) == ESP_ERR_INVALID_STATE);
    for (size_t i = 0; i < 2; i++) {
        c = s_backend.transaction.plan[i].descriptor;
        assert(update_begin_component(&c, &s_backend) == ESP_OK);
        const uint8_t *bytes = i ? (const uint8_t *)"data" : new_table;
        assert(update_write_component(&c, 0, bytes, c.size, &s_backend) == ESP_OK);
        assert(factory_system_update_source_end_idea_component(&c) == ESP_OK);
    }
    assert(image_checks == 1 && !boot_checks && !boot_selections);
    s_backend.transaction.plan[1].image_verified = false;
    assert(factory_system_update_source_commit_deferred(op) == ESP_ERR_INVALID_STATE);
    assert(!boot_selections);
    s_backend.transaction.plan[1].image_verified = true;
    mock_create_fail = 1; /* commit must succeed even when scheduling is impossible */
    assert(factory_system_update_source_commit_deferred(op) == ESP_OK);
    assert(image_checks == 1 && !boot_checks && boot_selections == 1);
    assert(boot == &s_backend.transaction.plan[1].target_partition && boot->address == 0x210000);
    assert(s_backend.status.update.phase == ESP_IRIS_SYSTEM_UPDATE_PHASE_COMMITTED);
    assert(factory_system_update_source_reserve(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE, op) == ESP_ERR_INVALID_STATE);
    assert(factory_system_update_writer_busy());
    assert(factory_system_update_source_reserve_idea(op) == ESP_OK);
    factory_system_update_source_abort(FACTORY_SYSTEM_UPDATE_OWNER_NAND, op, ESP_FAIL);
    assert(factory_system_update_writer_busy()); /* cached layout is still stale */
    assert(factory_system_update_source_reserve(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE, op) == ESP_ERR_INVALID_STATE);
    assert(factory_system_update_source_reserve_idea(op) == ESP_OK);
    assert(prepare(FACTORY_SYSTEM_UPDATE_OWNER_NAND, layout_manifest_without_data()) == ESP_OK);
    assert(mosaico_boot_request_recovery() == ESP_OK);
    for (size_t i = 0; i < 2; i++) {
        c = s_backend.transaction.plan[i].descriptor;
        assert(update_begin_component(&c, &s_backend) == ESP_OK);
        const uint8_t *bytes = i ? (const uint8_t *)"data" : new_table;
        assert(update_write_component(&c, 0, bytes, c.size, &s_backend) == ESP_OK);
        assert(factory_system_update_source_end_idea_component(&c) == ESP_OK);
    }
    assert(factory_system_update_source_commit_deferred(op) == ESP_OK);
    assert(boot->address == 0x210000 && !memcmp(flash + 0x210000, "data", 4));
    mock_create_fail = 0;
    assert(image_checks == 2 && !boot_checks && boot_selections == 2);
    for (int fault = 0; fault < 3; ++fault) {
        setup();
        assert(prepare(FACTORY_SYSTEM_UPDATE_OWNER_NAND, layout_manifest_without_data()) == ESP_OK);
        assert(!s_backend.transaction.plan[1].image_verified);
        assert(mosaico_boot_request_recovery() == ESP_OK);
        for (size_t i = 0; i < 2; ++i) {
            c = s_backend.transaction.plan[i].descriptor;
            assert(update_begin_component(&c, &s_backend) == ESP_OK);
            assert(update_write_component(&c, 0, i ? (const uint8_t *)(fault == 2 ? "bad!" : "data") : new_table,
                                          c.size, &s_backend) == ESP_OK);
            fail_image = i && fault == 0;
            corrupt_readback = i && fault == 1;
            assert((factory_system_update_source_end_idea_component(&c) == ESP_OK) == (i == 0));
        }
        assert(!s_backend.transaction.plan[1].image_verified);
        assert(factory_system_update_source_commit_deferred(op) != ESP_OK);
        assert(!boot_checks && !boot_selections && boot == &mock_factory);
    }
    /* A changed staged table must fail before it can authorize an erase using
     * its layout, even if its bytes still describe a structurally valid table. */
    setup();
    assert(prepare(FACTORY_SYSTEM_UPDATE_OWNER_NAND, layout_manifest_without_data()) == ESP_OK);
    c = s_backend.transaction.plan[0].descriptor;
    assert(update_begin_component(&c, &s_backend) == ESP_OK);
    assert(factory_system_update_source_end_idea_component(&c) == ESP_ERR_INVALID_STATE);
    assert(update_write_component(&c, 0, new_table, c.size, &s_backend) == ESP_OK);
    s_backend.transaction.partition_table_image[c.size - 1] ^= 1;
    assert(factory_system_update_source_end_idea_component(&c) == ESP_ERR_INVALID_CRC);
    assert(!s_backend.transaction.target_layout_valid && !erased && !mock_writes);
    assert(factory_system_update_source_commit_deferred(op) != ESP_OK);
    assert(!boot_selections);
    setup();
    assert(prepare(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE, layout_manifest_without_data()) == ESP_OK);
    c = s_backend.transaction.plan[0].descriptor;
    assert(factory_system_update_source_end_idea_component(&c) == ESP_ERR_INVALID_STATE);
    /* Resource partitions use the same final NOR check as applications. */
    for (unsigned fault = 0; fault < 2; ++fault) {
        setup();
        assert(prepare(FACTORY_SYSTEM_UPDATE_OWNER_NAND, layout_manifest(true)) == ESP_OK);
        c = s_backend.transaction.plan[0].descriptor;
        assert(update_begin_component(&c, &s_backend) == ESP_OK);
        assert(update_write_component(&c, 0, new_table, c.size, &s_backend) == ESP_OK);
        assert(factory_system_update_source_end_idea_component(&c) == ESP_OK);
        c = s_backend.transaction.plan[1].descriptor;
        assert(update_begin_component(&c, &s_backend) == ESP_OK);
        assert(update_write_component(&c, 0, (const uint8_t *)"data", 4, &s_backend) == ESP_OK);
        corrupt_readback = fault;
        assert(factory_system_update_source_end_idea_component(&c) ==
               (fault ? ESP_ERR_INVALID_CRC : ESP_OK));
        assert(s_backend.transaction.plan[1].completed == !fault);
    }
    puts("Recovery backend ownership, layout, readback and factory gates passed");
}

esp_err_t mosaico_boot_request_recovery(void) { boot = &mock_factory; return ESP_OK; }
esp_err_t mosaico_boot_consume_request(void) { return ESP_OK; }
esp_err_t mosaico_boot_recovery_requested(bool *requested)
{ *requested = boot == &mock_factory; return ESP_OK; }
esp_err_t mosaico_boot_select(uint32_t address)
{ assert(address >= 0x200000); return ESP_OK; }
