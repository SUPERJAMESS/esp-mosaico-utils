#include <assert.h>
#include "sdk.h"
#include FIRMWARE_SOURCE

static factory_update_check_snapshot_t setup(void)
{
    free(staged); staged = NULL;
    memset(&s_status, 0, sizeof(s_status));
    s_starting = false;
    reserved = prepared = commits = aborts = 0;
    create_error = reserve_error = stage_error = commit_error = ota_active = 0;
    read_error = chunked = incomplete = opens = cleanups = 0;
    now = 0; pending_task = NULL;
    http_status = 200; remote_size = 512;
    memset(remote, 0, sizeof(remote));
    esp_app_desc_t desc = {.magic_word=ESP_APP_DESC_MAGIC_WORD, .version="0.2.1", .project_name="vibe-mode"};
    memcpy(remote + 32, &desc, sizeof(desc));
    factory_update_check_snapshot_t release = {.available=true, .installable=true, .size=512,
        .version="0.2.1", .file="esp-mosaico-vibe-mode-0.2.1.bin"};
    uint8_t hash[32];
    assert(digest(remote, remote_size, hash, release.sha256) == ESP_OK);
    return release;
}
static void run_failure(factory_update_check_snapshot_t *release)
{
    assert(factory_firmware_install_start(release) == ESP_OK);
    assert(factory_firmware_install_start(release) == ESP_ERR_INVALID_STATE);
    pending_task(pending_argument);
    assert(s_status.phase == FIRMWARE_FAILED && s_status.error != ESP_OK);
    assert(commits == 0 && aborts == 1 && !reserved && opens == cleanups);
}
int main(void)
{
    factory_update_check_snapshot_t release = setup();
    assert(factory_firmware_install_start(&release) == ESP_OK);
    assert(s_status.phase == FIRMWARE_DOWNLOADING && commits == 0);
    pending_task(pending_argument);
    assert(commits == 1 && prepared == 1 && aborts == 0 && s_status.phase == FIRMWARE_RESTARTING);
    assert(!memcmp(staged, remote, remote_size));
    for (size_t i = remote_size; i < SLOT_BYTES; ++i) assert(staged[i] == 0xff);
    release = setup(); http_status = 404; run_failure(&release); assert(!prepared);
    release = setup(); http_status = 302; run_failure(&release); assert(!prepared);
    release = setup(); read_error = 1; run_failure(&release); assert(!prepared);
    release = setup(); remote_size--; run_failure(&release); assert(!prepared);
    release = setup(); chunked = 1; remote_size++; run_failure(&release); assert(!prepared);
    release = setup(); incomplete = 1; run_failure(&release); assert(!prepared);
    release = setup(); remote[400] ^= 1; run_failure(&release); assert(!prepared);
    release = setup(); strcpy(release.version, "0.2.2"); run_failure(&release); assert(!prepared);
    release = setup(); remote[80] = 'x'; run_failure(&release); assert(!prepared);
    release = setup(); stage_error = ESP_FAIL; run_failure(&release); assert(prepared == 1);
    release = setup(); commit_error = ESP_FAIL;
    assert(factory_firmware_install_start(&release) == ESP_OK); pending_task(pending_argument);
    assert(commits == 1 && aborts == 1 && s_status.phase == FIRMWARE_FAILED);
    release = setup(); create_error = 1;
    assert(factory_firmware_install_start(&release) == ESP_ERR_NO_MEM && !reserved && !pending_task);
    release = setup(); ota_active = 1;
    assert(factory_firmware_install_start(&release) == ESP_ERR_INVALID_STATE && !reserved && !pending_task);
    release = setup(); reserve_error = 1;
    assert(factory_firmware_install_start(&release) == ESP_ERR_INVALID_STATE && !pending_task);
    release = setup(); release.installable = false;
    assert(factory_firmware_install_start(&release) == ESP_ERR_NOT_SUPPORTED && !pending_task);
    free(staged);
    return 0;
}
