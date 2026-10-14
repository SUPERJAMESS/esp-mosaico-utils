// SPDX-License-Identifier: Apache-2.0
#include "sdk.h"
#include "factory_ideas.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *code = "042731";
static factory_ideas_snapshot_t snapshot(void)
{
    factory_ideas_snapshot_t out;
    assert(factory_ideas_snapshot(&out) == ESP_OK);
    return out;
}
static void run(factory_ideas_command_t command)
{
    assert(factory_ideas_request(command, command == IDEAS_SCAN ? NULL : code) == ESP_OK);
    assert(snapshot().busy);
    mock_run_worker();
    assert(!snapshot().busy);
}
static uint32_t preview(void)
{
    run(IDEAS_RESOLVE);
    factory_ideas_snapshot_t s = snapshot();
    if (!s.preview_ready)
        fprintf(stderr, "preview failed: %s %s\n", s.title, s.detail);
    assert(s.preview_ready && s.preview_id && !s.resolving && !s.terminal);
    assert(s.custom_display && s.has_icon && !s.view_only);
    assert(!strcmp(s.preview.code, code));
    return s.preview_id;
}
static void dismiss(void)
{
    assert(factory_ideas_cancel() == ESP_OK);
    mock_run_worker();
    assert(!snapshot().preview_ready && !snapshot().terminal);
}
static void confirm(uint32_t id)
{
    assert(factory_ideas_confirm(id, true) == ESP_OK);
    assert(factory_ideas_confirm(id, true) == ESP_ERR_INVALID_STATE);
    assert(factory_ideas_request(IDEAS_SCAN, NULL) == ESP_ERR_INVALID_STATE);
    mock_run_worker();
}
static void library(const char *version)
{
    factory_ideas_snapshot_t s = snapshot();
    assert(s.count == 1 && !s.invalid && !strcmp(s.items[0].version, version));
    FILE *f = fopen("/nand/ideas/042731/current/app.bin", "rb");
    assert(f && fgetc(f) == version[0]);
    fclose(f);
    DIR *dir = opendir("/nand/ideas/042731");
    assert(dir);
    struct dirent *entry;
    unsigned generations = 0;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        assert(!strcmp(entry->d_name, "current"));
        generations++;
    }
    closedir(dir);
    assert(generations == 1);
}
static void download(void)
{
    confirm(preview());
    factory_ideas_snapshot_t s = snapshot();
    assert(s.terminal && !s.failed && s.received == s.total && s.total > 4096);
}
static void copy_preview(uint32_t id)
{
    for (unsigned slot = 0; slot < 3; slot++) {
        size_t size = 0;
        void *data = factory_ideas_copy_preview(id, slot, &size);
        assert(data && size);
        if (slot < 2) {
            assert(size == (slot ? 416U * 176 : 328U * 84) * 2);
            if (!slot)
                assert(((uint16_t *)data)[0] == 0xffff);
        } else
            assert(!memcmp(data, "\x89PNG\r\n\x1a\n", 8));
        free(data);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 4);
    const char *scenario = argv[1];
    mock_environment_t *env = mock_environment();
    env->fixture_dir = argv[2];
    env->mount_dir = argv[3];
    assert(factory_ideas_init() == ESP_OK);
    mock_run_worker();
    assert(snapshot().count == 0 && snapshot().scan_error == ESP_OK && env->writes == 0);

    if (!strcmp(scenario, "preview")) {
        uint32_t id = preview();
        copy_preview(id);
        assert(env->resolves == 1 && env->assets == 3 && !env->firmware && !env->writes);
        assert(factory_ideas_request(IDEAS_INSTALL, code) == ESP_ERR_INVALID_STATE);
        assert(factory_ideas_confirm(id + 1, true) == ESP_ERR_INVALID_STATE);
        dismiss();
        size_t size = 123;
        assert(!factory_ideas_copy_preview(id, 0, &size) && !size);
        uint32_t next = preview();
        assert(next != id && factory_ideas_confirm(id, true) == ESP_ERR_INVALID_STATE);
        dismiss();
    } else if (!strncmp(scenario, "cancel-", 7)) {
        assert(factory_ideas_request(IDEAS_RESOLVE, code) == ESP_OK);
        uint32_t id = snapshot().preview_id;
        if (!strcmp(scenario, "cancel-queued"))
            assert(factory_ideas_cancel() == ESP_OK);
        else
            env->cancel_on_read =
                !strcmp(scenario, "cancel-resolve") ? "resolve" : "v1-display.json";
        mock_run_worker();
        factory_ideas_snapshot_t s = snapshot();
        assert(!s.preview_ready && !s.resolving && !s.preview_id && !s.terminal);
        assert(!env->writes && !env->firmware);
        assert(env->assets == (!strcmp(scenario, "cancel-asset") ? 1 : 0));
        assert(factory_ideas_confirm(id, true) == ESP_ERR_INVALID_STATE);
        assert(env->cancels == (!strcmp(scenario, "cancel-queued") ? 0 : 1));
        preview();
        dismiss();
    } else if (!strcmp(scenario, "fixed-release")) {
        uint32_t id = preview();
        env->release = "v2"; /* Cloud now resolves to another release before confirmation. */
        confirm(id);
        assert(snapshot().terminal && !snapshot().failed);
        library("1.0");
        assert(env->resolves == 1 && env->assets == 3 && env->firmware == 2);
        assert(!env->installs && !env->commits && !env->restarts);
        env->release = "v1";
        run(IDEAS_CHECK);
        assert(snapshot().terminal && !snapshot().failed && !snapshot().preview_ready);
        assert(env->assets == 3 && env->requests_with_identity == 1);
        id = preview();
        assert(snapshot().already_downloaded);
        unsigned writes = env->writes;
        confirm(id);
        assert(snapshot().return_library && !snapshot().terminal);
        assert(env->writes == writes && env->firmware == 2);
    } else if (!strcmp(scenario, "task-failure")) {
        env->fail_task = true;
        assert(factory_ideas_request(IDEAS_RESOLVE, code) == ESP_ERR_NO_MEM);
        assert(!snapshot().busy && !snapshot().resolving && !snapshot().preview_id);
        uint32_t id = preview();
        env->fail_task = true;
        assert(factory_ideas_confirm(id, true) == ESP_ERR_NO_MEM);
        assert(snapshot().preview_ready && snapshot().preview_id == id && !snapshot().busy);
        copy_preview(id);
        confirm(id);
        library("1.0");
    } else if (!strncmp(scenario, "reject-", 7)) {
        const char *bad = scenario + 7;
        if (!strncmp(bad, "bad-", 4))
            env->release = bad;
        else {
            env->fault_file = "v1-display.json";
            if (!strcmp(bad, "hash"))
                env->fault = MOCK_CORRUPT;
            else if (!strcmp(bad, "truncated"))
                env->fault = MOCK_TRUNCATE;
            else if (!strcmp(bad, "missing"))
                env->fault = MOCK_NOT_FOUND;
            else if (!strcmp(bad, "redirect"))
                env->fault = MOCK_REDIRECT;
            else if (!strcmp(bad, "read-error"))
                env->fault = MOCK_READ_ERROR;
            else if (!strcmp(bad, "wrong-length"))
                env->fault = MOCK_WRONG_LENGTH;
            else if (!strcmp(bad, "unknown-code")) {
                env->fault_file = "resolve";
                env->fault = MOCK_NOT_FOUND;
            } else if (!strcmp(bad, "resolve-truncated")) {
                env->fault_file = "resolve";
                env->fault = MOCK_TRUNCATE;
            } else
                assert(false);
        }
        run(IDEAS_RESOLVE);
        assert(snapshot().terminal && snapshot().failed && !snapshot().preview_ready);
        assert(!env->writes && !env->firmware && !env->installs && !env->commits);
        env->fault_file = NULL;
        env->release = "v1";
        preview();
        dismiss();
    } else if (!strncmp(scenario, "rollback-", 9)) {
        download();
        env->release = "v2";
        uint32_t id = preview();
        env->fault_file = "v2-app.bin";
        env->fault = !strcmp(scenario, "rollback-hash") ? MOCK_CORRUPT : MOCK_TRUNCATE;
        confirm(id);
        assert(snapshot().terminal && snapshot().failed);
        library("1.0");
        assert(!env->installs && !env->commits);
        env->fault_file = NULL;
        download();
        library("2.0");
    } else if (!strcmp(scenario, "offline") || !strncmp(scenario, "install-", 8)) {
        download();
        env->network = false;
        unsigned requests = env->resolves + env->assets + env->firmware;
        run(IDEAS_PREVIEW);
        factory_ideas_snapshot_t s = snapshot();
        assert(s.preview_ready && s.view_only);
        copy_preview(s.preview_id);
        assert(factory_ideas_confirm(s.preview_id, true) == ESP_ERR_INVALID_STATE);
        dismiss();
        size_t size;
        void *icon = factory_ideas_copy_icon(code, &size);
        assert(icon && size);
        free(icon);
        if (!strcmp(scenario, "install-corrupt")) {
            FILE *f = fopen("/nand/ideas/042731/current/font.mif", "r+b");
            assert(f && fputc('X', f) != EOF && fclose(f) == 0);
        }
        if (!strcmp(scenario, "install-commit-failure"))
            env->fail_commit = true;
        run(IDEAS_INSTALL);
        s = snapshot();
        assert(s.terminal && !s.preview_ready && !env->restarts);
        if (!strcmp(scenario, "offline")) {
            assert(!s.failed && s.items[0].boot && env->installs == 1 && env->commits == 1);
            assert(env->flash_version == '1');
            assert(factory_ideas_request(IDEAS_START, code) == ESP_OK && env->restarts == 1);
        } else {
            assert(s.failed && !s.items[0].boot && !env->commits);
            assert(env->installs == (!strcmp(scenario, "install-corrupt") ? 0 : 1));
            assert(factory_ideas_request(IDEAS_START, code) == ESP_ERR_INVALID_STATE);
        }
        assert(requests == env->resolves + env->assets + env->firmware);
    } else if (!strcmp(scenario, "boot-identity")) {
        download();
        run(IDEAS_INSTALL);
        assert(snapshot().items[0].boot && env->flash_version == '1');
        env->release = "v2";
        run(IDEAS_CHECK);
        assert(snapshot().preview_ready && snapshot().items[0].update_available);
        assert(!strcmp(snapshot().items[0].latest_version, "2.0"));
        confirm(snapshot().preview_id);
        library("2.0");
        assert(snapshot().items[0].boot && env->flash_version == '1' && env->commits == 1);
        run(IDEAS_INSTALL);
        assert(!snapshot().failed && snapshot().items[0].boot && env->flash_version == '2');
        mock_corrupt_flash();
        run(IDEAS_SCAN);
        assert(!snapshot().items[0].boot);
        assert(factory_ideas_request(IDEAS_START, code) == ESP_ERR_INVALID_STATE);
    } else if (!strcmp(scenario, "repair-current")) {
        download();
        FILE *f = fopen("/nand/ideas/042731/current/app.bin", "r+b");
        assert(f && fputc('X', f) != EOF && fclose(f) == 0);
        uint32_t id = preview();
        assert(!snapshot().already_downloaded);
        confirm(id);
        assert(!snapshot().failed && env->firmware == 4);
        library("1.0");
        run(IDEAS_CHECK);
        assert(!snapshot().preview_ready && !snapshot().failed);
    } else if (!strcmp(scenario, "interrupted-generation")) {
        download();
        assert(rename("/nand/ideas/042731/current", "/nand/ideas/042731/.previous") == 0);
        assert(mkdir("/nand/ideas/042731/.incoming", 0700) == 0);
        FILE *f = fopen("/nand/ideas/042731/.incoming/app.bin", "wb");
        assert(f && fputs("partial", f) >= 0 && fclose(f) == 0);
        run(IDEAS_SCAN);
        library("1.0");
    } else if (!strcmp(scenario, "admission")) {
        assert(factory_ideas_request(IDEAS_DOWNLOAD, code) == ESP_ERR_INVALID_ARG);
        assert(factory_ideas_request(IDEAS_RESOLVE, "42731") == ESP_ERR_INVALID_ARG);
        assert(factory_ideas_request(IDEAS_RESOLVE, "04273x") == ESP_ERR_INVALID_ARG);
        assert(factory_ideas_request(IDEAS_RESOLVE, NULL) == ESP_ERR_INVALID_ARG);
        env->storage_busy = true;
        run(IDEAS_RESOLVE);
        assert(snapshot().failed && !env->resolves && !env->writes);
        env->storage_busy = false;
        env->network = false;
        run(IDEAS_RESOLVE);
        assert(snapshot().failed && !env->resolves && !env->writes);
        env->network = true;
        uint32_t id = preview();
        env->storage_busy = true;
        confirm(id);
        assert(snapshot().failed && !env->writes && !env->firmware);
        env->storage_busy = false;
        download();
        library("1.0");
    } else
        assert(false);
    mock_assert_idle();
    puts(scenario);
    return 0;
}
