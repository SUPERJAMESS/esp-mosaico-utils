#include "sdk.h"
#include "mosaico_boot.h"
#include "mosaico_recovery_contract.h"
#include <assert.h>
#include <string.h>

static esp_partition_t recovery = {0x20000, 0x1c0000, ESP_PARTITION_TYPE_APP,
                                  ESP_PARTITION_SUBTYPE_APP_TEST, "vibe_mode"};
static const esp_partition_t application = {0x210000, 0xcf0000, ESP_PARTITION_TYPE_APP,
                                           ESP_PARTITION_SUBTYPE_APP_OTA_MIN, "main_app"};
static const esp_partition_t otadata = {0x9000, 0x2000, ESP_PARTITION_TYPE_DATA,
                                       ESP_PARTITION_SUBTYPE_DATA_OTA, "otadata"};
static const esp_partition_t *running = &application;
static esp_err_t image_error, open_error, read_error, commit_error, select_error, erase_error;
static uint32_t stored_intent, pending_intent;
static unsigned commits, closes, selections, erases;
static uint8_t flash[0x2000];

const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *name)
{
    if (type == ESP_PARTITION_TYPE_DATA && subtype == ESP_PARTITION_SUBTYPE_DATA_OTA &&
        strcmp(name, "otadata") == 0) return &otadata;
    if (type == recovery.type && subtype == recovery.subtype &&
        strcmp(name, recovery.label) == 0) return &recovery;
    return NULL;
}
esp_partition_iterator_t esp_partition_find(int a, int b, const char *c) { return &application; }
const esp_partition_t *esp_partition_get(esp_partition_iterator_t i) { return i; }
esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t i) { return NULL; }
void esp_partition_iterator_release(esp_partition_iterator_t i) { }
esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *out, size_t n)
{ assert(p == &otadata && off + n <= sizeof(flash)); memcpy(out, flash + off, n); return ESP_OK; }
esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t off, size_t n)
{
    assert(p == &otadata && off == 0 && n == 0x1000);
    if (erase_error) return erase_error;
    memset(flash + off, 0xff, n); ++erases; return ESP_OK;
}
const esp_partition_t *esp_ota_get_running_partition(void) { return running; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p)
{ assert(p == &application); ++selections; return select_error; }
esp_err_t esp_image_verify(int mode, const esp_partition_pos_t *p, esp_image_metadata_t *m)
{ assert(p->offset == recovery.address && p->size == recovery.size); return image_error; }
esp_err_t nvs_flash_init_partition(const char *p)
{ assert(strcmp(p, MOSAICO_SYSMETA_PARTITION) == 0); return ESP_OK; }
esp_err_t nvs_open_from_partition(const char *p, const char *ns, int mode, nvs_handle_t *h)
{ assert(strcmp(ns, MOSAICO_BOOT_NAMESPACE) == 0); *h = 1; return open_error; }
esp_err_t nvs_get_u32(nvs_handle_t h, const char *k, uint32_t *v)
{ *v = stored_intent; return read_error; }
esp_err_t nvs_set_u32(nvs_handle_t h, const char *k, uint32_t v)
{ pending_intent = v; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h)
{ if (commit_error) return commit_error; stored_intent = pending_intent; ++commits; return ESP_OK; }
void nvs_close(nvs_handle_t h) { ++closes; }

int main(void)
{
    memset(flash, 0xff, sizeof(flash));
    recovery.subtype = 0;  /* Old factory layout is rejected without writing. */
    assert(mosaico_boot_request_recovery() == ESP_ERR_NOT_FOUND && commits == 0);
    recovery.subtype = ESP_PARTITION_SUBTYPE_APP_TEST;
    image_error = ESP_FAIL;
    assert(mosaico_boot_request_recovery() == ESP_FAIL && commits == 0);
    image_error = 0;
    open_error = ESP_FAIL;
    assert(mosaico_boot_request_recovery() == ESP_FAIL && closes == 0);
    open_error = 0;
    commit_error = ESP_FAIL;
    assert(mosaico_boot_request_recovery() == ESP_FAIL && stored_intent == 0 && closes == 1);
    commit_error = 0;
    assert(mosaico_boot_request_recovery() == ESP_OK && commits == 1);
    assert(mosaico_boot_request_recovery() == ESP_OK && commits == 1); /* idempotent */
    bool requested = false;
    assert(mosaico_boot_recovery_requested(&requested) == ESP_OK && requested);
    assert(mosaico_boot_consume_request() == ESP_ERR_INVALID_STATE && commits == 1);
    select_error = ESP_FAIL;
    assert(mosaico_boot_select(application.address) == ESP_FAIL);
    assert(stored_intent == MOSAICO_BOOT_INTENT_RECOVERY);
    select_error = 0;
    assert(mosaico_boot_select(application.address) == ESP_OK && stored_intent == 0);
    assert(mosaico_boot_select(0x30000) == ESP_ERR_NOT_FOUND);
    running = &recovery;
    memcpy(flash + MOSAICO_BOOTSTRAP_OFFSET, MOSAICO_BOOTSTRAP_MARKER, MOSAICO_BOOTSTRAP_BYTES);
    flash[0x1000] = 0x42; /* Consuming install marker never erases the other sector. */
    erase_error = ESP_FAIL;
    assert(mosaico_boot_consume_request() == ESP_FAIL && erases == 0);
    assert(memcmp(flash + MOSAICO_BOOTSTRAP_OFFSET, MOSAICO_BOOTSTRAP_MARKER, 16) == 0);
    erase_error = 0;
    assert(mosaico_boot_consume_request() == ESP_OK && erases == 1 && flash[0x1000] == 0x42);
    assert(mosaico_boot_consume_request() == ESP_OK && erases == 1);
    read_error = ESP_FAIL;
    assert(mosaico_boot_request_recovery() == ESP_FAIL && commits == 2);
    return 0;
}
