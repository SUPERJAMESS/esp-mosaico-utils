// SPDX-License-Identifier: Apache-2.0
#include "factory_ideas.h"
#include "factory_locale.h"
#include "sdk.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *code = "042731";
static factory_ideas_snapshot_t snapshot(void) {
    factory_ideas_snapshot_t out;
    assert(factory_ideas_snapshot(&out) == ESP_OK);
    return out;
}
static void run(factory_ideas_command_t command)
{
    assert(factory_ideas_request(command, (command == IDEAS_SCAN || command == IDEAS_RELOCALIZE || command == IDEAS_UPDATE_ALL || command == IDEAS_SAVE_DEVELOPMENT || command == IDEAS_REFRESH_UPDATES) ? NULL : code) == ESP_OK);
    assert(snapshot().busy);
    mock_run_worker();
    assert(!snapshot().busy);
}
static uint32_t preview(void) {
    run(IDEAS_RESOLVE);
    factory_ideas_snapshot_t s = snapshot();
    if (!s.preview_ready)
        fprintf(stderr, "preview failed: %s %s\n", s.title, s.detail);
    assert(s.preview_ready && s.preview_id && !s.resolving && !s.terminal);
    assert(s.custom_display && s.has_icon && !s.view_only);
    assert(!strcmp(s.preview.code, code));
    return s.preview_id;
}
static void dismiss(void) {
    assert(factory_ideas_cancel() == ESP_OK);
    mock_run_worker();
    assert(!snapshot().preview_ready && !snapshot().terminal);
}
static void confirm(uint32_t id) {
    assert(factory_ideas_confirm(id, true) == ESP_OK);
    assert(factory_ideas_confirm(id, true) == ESP_ERR_INVALID_STATE);
    assert(factory_ideas_request(IDEAS_SCAN, NULL) == ESP_ERR_INVALID_STATE);
    mock_run_worker();
}
static void library(const char *version) {
    factory_ideas_snapshot_t s = snapshot();
    assert(s.count == 1 && !s.invalid && !strcmp(s.items[0].version, version));
    FILE *f = fopen("/nand/ideas/042731/current/firmware/app.bin", "rb");
    assert(f && fseek(f, 48, SEEK_SET) == 0 && fgetc(f) == version[0]);
    fclose(f);
    DIR *dir = opendir("/nand/ideas/042731");
    assert(dir);
    struct dirent *entry;
    unsigned generations = 0;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        assert(!strcmp(entry->d_name, "current") || !strcmp(entry->d_name, ".roots"));
        generations++;
    }
    closedir(dir);
    assert(generations == 2);
}
static unsigned save_cancel_mode;
static bool batch_commit_cancelled;
static void cancel_batch_commit(void) {
    factory_ideas_snapshot_t s = snapshot();
    if (!batch_commit_cancelled && s.busy && s.batch && !s.cancellable) {
        batch_commit_cancelled = true;
        assert(factory_ideas_cancel() == ESP_OK);
    }
}
static bool save_cancel_seen;
static uint64_t save_copy_end;
static void cancel_save(void) {
    factory_ideas_snapshot_t s = snapshot();
    if (save_cancel_seen || !s.busy || s.command != IDEAS_SAVE_DEVELOPMENT) return;
    mock_environment_t *env = mock_environment();
    if (save_cancel_mode == 1) {
        if (!s.total || s.received != s.total) return;
        if (!save_copy_end) save_copy_end = env->flash_read_bytes;
        if (env->flash_read_bytes <= save_copy_end + 4096) return;
    } else if (save_cancel_mode == 2) {
        if (s.cancellable) return;
    } else if (!s.received) return;
    save_cancel_seen = true;
    assert(factory_ideas_cancel() == (save_cancel_mode == 2 ? ESP_ERR_INVALID_STATE : ESP_OK));
}
static void assert_no_pending_save(void) {
    DIR *dir = opendir("/nand/ideas");
    assert(dir);
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        char path[384]; struct stat info;
        snprintf(path, sizeof(path), "/nand/ideas/%s/.incoming", entry->d_name);
        assert(stat(path, &info) != 0);
    }
    closedir(dir);
}
static unsigned phases, last_phase, verify_samples;
static uint64_t verify_bytes, verify_total, image_bytes;
static unsigned image_samples;
static void observe_progress(void) {
    factory_ideas_snapshot_t s = snapshot();
    assert(s.phase >= last_phase);
    last_phase = s.phase;
    phases |= 1U << s.phase;
    if (s.phase == IDEAS_PHASE_VERIFY_FILES) {
        assert(s.received >= verify_bytes && s.received <= s.total);
        if (s.received > verify_bytes) ++verify_samples;
        verify_bytes = s.received;
        verify_total = s.total;
        assert(s.files_verified <= s.file_count);
    }
    if (s.phase == IDEAS_PHASE_CHECK_IMAGE) {
        assert(s.received <= s.total && s.files_verified <= s.file_count);
        if (s.received > image_bytes) ++image_samples;
        image_bytes = s.received;
    }
}
static bool cancelled, cancel_in_flash, cancel_at_commit;
static uint64_t flash_before, flash_at_cancel;
static void cancel_open(void) {
    factory_ideas_snapshot_t s = snapshot();
    if (cancelled || !s.busy) return;
    mock_environment_t *env = mock_environment();
    bool ready = cancel_at_commit ? s.phase == IDEAS_PHASE_SAVE :
        cancel_in_flash ? s.phase == IDEAS_PHASE_CHECK_IMAGE && env->flash_read_bytes > flash_before + 8192 :
        s.phase == IDEAS_PHASE_CHECK_IMAGE;
    if (!ready) return;
    cancelled = true;
    flash_at_cancel = env->flash_read_bytes;
    assert(factory_ideas_cancel() == (cancel_at_commit ? ESP_ERR_INVALID_STATE : ESP_OK));
    s = snapshot();
    assert(!s.cancellable);
    if (!cancel_at_commit) {
        assert(!strcmp(s.title, "Cancelling..."));
        assert(factory_ideas_cancel() == ESP_OK); /* Repeated taps are harmless. */
    }
}
static void download(void) {
    confirm(preview());
    factory_ideas_snapshot_t s = snapshot();
    assert(s.terminal && !s.failed && s.received == s.total && s.total > 4096);
}
static void copy_preview(uint32_t id) {
    for (unsigned slot = 0; slot < 3; slot++) {
        size_t size = 0;
        void *data = factory_ideas_copy_preview(id, slot, &size);
        assert(data && size);
        if (!slot) assert(size > 22 && !memcmp(data, "qoif", 4));
        else if (slot == 1) assert(size == 416U * 176 * 2);
        else
            assert(!memcmp(data, "\x89PNG\r\n\x1a\n", 8));
        free(data);
    }
}

static bool handed_off;
static bool writer_attempted;
static void try_writer_at_restart(void) {
    factory_ideas_snapshot_t s = snapshot();
    if (writer_attempted || !s.busy || s.command != IDEAS_OPEN || s.phase != IDEAS_PHASE_SAVE)
        return;
    writer_attempted = true;
    /* The updater can claim NOR as soon as the read owner releases it. */
    if (!mock_environment()->read_lease) mock_environment()->ota_busy = true;
}
static void handoff(void) {
    factory_ideas_snapshot_t s = snapshot();
    if (!handed_off && !s.busy && s.preview_ready) {
        handed_off = true;
        assert(factory_ideas_cancel() == ESP_OK);
        assert(snapshot().busy);
    }
}

int main(int argc, char **argv) {
    assert(argc == 4);
    const char *scenario = argv[1];
    mock_environment_t *env = mock_environment();
    env->fixture_dir = argv[2];
    env->mount_dir = argv[3];
    if (!strcmp(scenario, "init-retry")) {
        for (unsigned step = 1; step <= 5; ++step) {
            env->fail_init_step = step;
            assert(factory_ideas_init() == ESP_ERR_NO_MEM);
            assert(!env->mutex_live && !env->queue_live && !env->tasks_created);
            factory_ideas_snapshot_t state;
            assert(factory_ideas_snapshot(&state) == ESP_ERR_INVALID_ARG);
            assert(factory_ideas_request(IDEAS_SCAN, NULL) == ESP_ERR_INVALID_STATE);
            env->fail_init_step = 0;
        }
    }
    assert(factory_ideas_init() == ESP_OK);
    assert(factory_ideas_init() == ESP_OK);
    mock_run_worker();
    assert(snapshot().count == 0 && snapshot().scan_error == ESP_OK && env->writes == 0);
    assert(!env->directory_reads && !env->storage_info_calls && !snapshot().generation);
    assert(!snapshot().busy); /* Cloud admission is independent of library discovery. */

    if (!strncmp(scenario, "format-", 7)) {
        preview();
        assert(factory_ideas_request(IDEAS_FORMAT_NAND, NULL) == ESP_ERR_INVALID_STATE);
        assert(!env->formats); /* A retained preview must be dismissed first. */
        confirm(snapshot().preview_id);
        library("1.0");
        env->storage_error = ESP_FAIL;
        if (!strcmp(scenario, "format-admission")) {
            env->storage_busy = true;
            assert(factory_ideas_request(IDEAS_FORMAT_NAND, NULL) == ESP_OK);
            mock_run_worker();
            assert(snapshot().failed && !env->formats && !env->iris_stops);
            env->storage_busy = false;
            env->ota_busy = true;
            assert(factory_ideas_request(IDEAS_FORMAT_NAND, NULL) == ESP_OK);
            mock_run_worker();
            assert(snapshot().failed && !env->formats && !env->iris_stops && !env->lease);
            env->ota_busy = false;
            env->fail_stop = true;
            assert(factory_ideas_request(IDEAS_FORMAT_NAND, NULL) == ESP_OK);
            mock_run_worker();
            assert(snapshot().failed && !env->formats && !env->lease && !env->read_lease);
            assert(snapshot().count == 1); /* Admission failure preserves cached Ideas. */
            env->fail_stop = false;
            assert(factory_ideas_request(IDEAS_FORMAT_NAND, NULL) == ESP_OK);
            mock_run_worker();
            assert(!snapshot().failed && env->formats == 1 && env->iris_stops == 2 && env->iris_starts == 1);
            return 0;
        }
        env->fail_format = !strcmp(scenario, "format-failure");
        assert(factory_ideas_request(IDEAS_FORMAT_NAND, NULL) == ESP_OK);
        assert(snapshot().busy && !snapshot().cancellable);
        assert(factory_ideas_request(IDEAS_FORMAT_NAND, NULL) == ESP_ERR_INVALID_STATE);
        assert(factory_ideas_cancel() == ESP_ERR_INVALID_STATE);
        mock_run_worker();
        assert(env->formats == 1 && env->iris_stops == 1 && env->iris_starts == 1);
        assert(!env->lease && !env->read_lease && !env->iris_stopped);
        assert(snapshot().terminal && snapshot().failed == env->fail_format);
        assert(!snapshot().count && !env->restarts && !env->installs);
        assert((snapshot().storage_error != ESP_OK) == env->fail_format);
        if (!env->fail_format) { confirm(preview()); library("1.0"); }
        return 0;
    }
    if (!strcmp(scenario, "seed-full-library")) {
        char selected[7];
        for (unsigned i = 0; i < FACTORY_IDEAS_MAX_ITEMS; ++i) {
            snprintf(selected, sizeof(selected), "%06u", 42731 + i);
            code = selected; download();
            assert(snapshot().count == i + 1 && !snapshot().invalid);
        }
    } else if (!strncmp(scenario, "capacity-", 9)) {
        bool scan_error = !strcmp(scenario, "capacity-scan-error");
        if (scan_error) { env->fail_file = "/current/manifest.json"; env->fail_file_remaining = 64; }
        if (!strcmp(scenario, "capacity-development")) {
            mock_development(); run(IDEAS_SAVE_DEVELOPMENT);
        } else {
            bool update = !strcmp(scenario, "capacity-update");
            code = update ? "042731" : "042747";
            if (update) env->release = "v2";
            confirm(preview());
        }
        factory_ideas_snapshot_t s = snapshot();
        fprintf(stderr, "%s: failed=%d count=%zu invalid=%zu firmware=%u\n",
                scenario, s.failed, s.count, s.invalid, env->firmware);
        if (scan_error) {
            assert(s.failed && s.error == ESP_FAIL && !s.count && !env->writes && !env->firmware);
            env->fail_file = NULL; run(IDEAS_SCAN); s = snapshot();
        }
        assert(s.count == FACTORY_IDEAS_MAX_ITEMS && !s.invalid);
        if (!strcmp(scenario, "capacity-update")) {
            assert(!s.failed && env->firmware);
            bool updated = false;
            for (size_t i = 0; i < s.count; ++i)
                updated |= !strcmp(s.items[i].code, "042731") && !strcmp(s.items[i].version, "2.0");
            assert(updated);
        } else if (!scan_error) {
            assert(s.failed && s.error == ESP_ERR_NO_MEM && !env->writes && !env->firmware);
        }
    } else if (!strcmp(scenario, "init-retry") || !strcmp(scenario, "worker-reuse")) {
        env->deny_new_tasks = true;
        for (unsigned i = 0; i < 30; ++i) {
            preview();
            dismiss();
            run(IDEAS_SCAN);
            assert(factory_ideas_init() == ESP_OK);
            assert(env->tasks_created == 1 && env->mutex_live == 1 && env->queue_live == 1);
        }
        assert(env->jobs_finished == 91);
    } else if (!strcmp(scenario, "worker-handoff")) {
        assert(factory_ideas_request(IDEAS_RESOLVE, code) == ESP_OK);
        env->observe = handoff;
        mock_run_one_job();
        env->observe = NULL;
        assert(handed_off && snapshot().busy);
        mock_run_worker();
        assert(!snapshot().busy && !snapshot().preview_ready && !snapshot().terminal);
        assert(env->tasks_created == 1 && env->jobs_finished == 3);
    } else if (!strcmp(scenario, "download-stages")) {
        uint32_t id = preview();
        env->observe = observe_progress;
        confirm(id);
        env->observe = NULL;
        factory_ideas_snapshot_t s = snapshot();
        assert(s.terminal && !s.failed && s.phase == IDEAS_PHASE_COMPLETE);
        assert(phases == ((1U << (IDEAS_PHASE_COMPLETE + 1)) - 2));
        assert(verify_bytes == s.total && verify_samples >= s.file_count);
    } else if (!strcmp(scenario, "refresh-updates")) {
        download(); env->release = "v2";
        unsigned assets = env->assets, writes = env->writes;
        run(IDEAS_REFRESH_UPDATES);
        assert(snapshot().items[0].update_available && !snapshot().terminal);
        assert(env->assets == assets && env->writes == writes && !env->installs);
        library("1.0");
    } else if (!strcmp(scenario, "open-cancel")) {
        download(); env->cancel_on_delay = true;
        run(IDEAS_OPEN);
        assert(!snapshot().failed && !strcmp(snapshot().title, "Cancelled"));
        assert(!env->installs && !env->commits && !env->restarts);
        library("1.0");
    } else if (!strcmp(scenario, "open-current-lease") ||
               !strcmp(scenario, "open-current-intent") ||
               !strcmp(scenario, "open-current-intent-failure") ||
               !strcmp(scenario, "open-current-read-error")) {
        download(); run(IDEAS_OPEN);
        unsigned writes = env->writes, consumes = env->consume_requests;
        bool lease = !strcmp(scenario, "open-current-lease");
        bool failure = !strcmp(scenario, "open-current-intent-failure");
        bool read_error = !strcmp(scenario, "open-current-read-error");
        if (lease) env->observe = try_writer_at_restart;
        else env->recovery_requested = true; /* A failed maintenance operation left this intent. */
        env->fail_consume = failure;
        env->fail_flash_read = read_error;
        run(IDEAS_OPEN); env->observe = NULL;
        factory_ideas_snapshot_t s = snapshot();
        fprintf(stderr, "%s: failed=%d restarts=%u recovery=%u during-write=%u consumes=%u\n",
                scenario, s.failed, env->restarts, env->recovery_restarts, env->busy_restarts,
                env->consume_requests - consumes);
        assert(!env->read_lease && env->writes == writes && env->installs == 1);
        if (lease) assert(writer_attempted && !env->busy_restarts && !env->ota_busy);
        if (failure || read_error) {
            assert(s.failed && env->restarts == 1 && env->recovery_requested);
            env->fail_consume = env->fail_flash_read = false; run(IDEAS_OPEN);
        } else assert(!s.failed && env->restarts == 2);
        assert(!env->recovery_requested && !env->recovery_restarts);
    } else if (!strcmp(scenario, "open-progress") || !strcmp(scenario, "open-current-progress")) {
        download();
        bool current = !strcmp(scenario, "open-current-progress");
        if (current) run(IDEAS_OPEN);
        env->observe = observe_progress;
        run(IDEAS_OPEN);
        env->observe = NULL;
        factory_ideas_snapshot_t s = snapshot();
        assert(!s.failed && !s.cancelled && env->installs == 1 && env->restarts == (current ? 2U : 1U));
        assert(!verify_samples && !(phases & (1U << IDEAS_PHASE_VERIFY_FILES)));
        if (current) assert(image_samples > 1 && image_bytes == s.total);
    } else if (!strcmp(scenario, "open-cancel-block") ||
               !strcmp(scenario, "open-cancel-current") ||
               !strcmp(scenario, "open-cancel-queued") ||
               !strcmp(scenario, "open-cancel-gate")) {
        download();
        cancel_in_flash = !strcmp(scenario, "open-cancel-current");
        cancel_at_commit = !strcmp(scenario, "open-cancel-gate");
        if (cancel_in_flash) run(IDEAS_OPEN);
        uint32_t generation = snapshot().generation;
        unsigned writes = env->writes, reads = env->firmware_reads;
        unsigned installs = env->installs, commits = env->commits, restarts = env->restarts;
        flash_before = env->flash_read_bytes;
        assert(factory_ideas_request(IDEAS_OPEN, code) == ESP_OK);
        bool queued = !strcmp(scenario, "open-cancel-queued");
        if (queued) {
            assert(factory_ideas_cancel() == ESP_OK);
            cancelled = true;
        } else env->observe = cancel_open;
        mock_run_worker();
        env->observe = NULL;
        factory_ideas_snapshot_t s = snapshot();
        assert(cancelled && !s.failed && s.terminal && !s.busy && !s.cancellable);
        if (cancel_at_commit) {
            assert(!s.cancelled && env->installs == installs + 1 && env->restarts == restarts + 1);
        } else {
            assert(s.cancelled && !strcmp(s.title, "Cancelled"));
            assert(s.generation == generation); /* Cancellation must not trigger a scan. */
            assert(env->installs == installs && env->commits == commits && env->restarts == restarts);
            if (cancel_in_flash) assert(env->flash_read_bytes - flash_at_cancel <= 4096);
            else {
                assert(env->writes == writes && env->flash_read_bytes == flash_before);
                if (queued) assert(env->firmware_reads == reads);
            }
        }
        library("1.0");
    } else if (!strcmp(scenario, "no-space")) {
        uint32_t id = preview(); env->no_space = true;
        confirm(id);
        assert(snapshot().failed && !env->firmware && !env->installs);
        assert(snapshot().count == 0);
    } else if (!strcmp(scenario, "open")) {
        download(); env->network = false;
        run(IDEAS_OPEN);
        assert(!snapshot().failed && env->installs == 1 && env->restarts == 1);
        run(IDEAS_OPEN);
        assert(!snapshot().failed && env->installs == 1 && env->restarts == 2);
        mock_corrupt_flash(); /* Cached library identity cannot skip NOR validation. */
        run(IDEAS_OPEN);
        assert(!snapshot().failed && env->installs == 2 && env->restarts == 3);
    } else if (!strcmp(scenario, "delete-current-read-error") ||
               !strcmp(scenario, "delete-current-nvs-error") ||
               !strcmp(scenario, "delete-during-update") ||
               !strcmp(scenario, "delete-stale-record")) {
        download(); run(IDEAS_INSTALL);
        bool stale = !strcmp(scenario, "delete-stale-record");
        env->fail_flash_read = !strcmp(scenario, "delete-current-read-error");
        env->fail_boot_read = !strcmp(scenario, "delete-current-nvs-error");
        env->ota_busy = !strcmp(scenario, "delete-during-update");
        if (stale || env->ota_busy) mock_corrupt_flash();
        unsigned writes = env->writes;
        run(IDEAS_DELETE);
        factory_ideas_snapshot_t s = snapshot();
        fprintf(stderr, "%s: failed=%d count=%zu writes=%u\n", scenario, s.failed, s.count, env->writes - writes);
        if (stale) assert(!s.failed && s.count == 0);
        else assert(s.failed && s.count == 1 && env->writes == writes);
        env->fail_flash_read = env->fail_boot_read = env->ota_busy = false;
    } else if (!strcmp(scenario, "delete") || !strcmp(scenario, "delete-current")) {
        download();
        bool current = !strcmp(scenario, "delete-current");
        if (current) run(IDEAS_INSTALL);
        run(IDEAS_DELETE);
        assert(snapshot().failed == current && snapshot().count == (current ? 1U : 0U));
        assert(snapshot().terminal_command == IDEAS_DELETE);
        run(IDEAS_SCAN);
        assert(snapshot().count == (current ? 1U : 0U) && !snapshot().invalid);
        assert(snapshot().command == IDEAS_SCAN && snapshot().terminal_command == IDEAS_DELETE);
        assert(factory_ideas_request(IDEAS_STORAGE_INFO, NULL) == ESP_OK);
        mock_run_worker();
        assert(snapshot().command == IDEAS_STORAGE_INFO && snapshot().terminal_command == IDEAS_DELETE);
        assert(snapshot().failed == current); /* Background work preserves result ownership. */
    } else if (!strcmp(scenario, "download-cancel")) {
        download(); env->release = "v2";
        uint32_t id = preview();
        env->cancel_on_read = "app.bin";
        confirm(id);
        assert(!snapshot().failed && !strcmp(snapshot().title, "Cancelled"));
        library("1.0");
        assert(!env->installs && !env->restarts);
    } else if (!strcmp(scenario, "batch")) {
        download();
        code = "042732"; download(); code = "042731";
        env->release = "v2";
        run(IDEAS_UPDATE_ALL);
        assert(!snapshot().failed && !snapshot().batch && !snapshot().busy);
        factory_ideas_snapshot_t s = snapshot();
        assert(s.count == 2 && s.batch_index == 2 && s.batch_total == 2);
        for (size_t i = 0; i < s.count; ++i)
            assert(!strcmp(s.items[i].version, "2.0") && !s.items[i].queued && !s.items[i].update_available);
        assert(!env->installs && !env->restarts);
    } else if (!strncmp(scenario, "batch-cancel-", 13)) {
        download(); code = "042732"; download(); code = "042731";
        env->release = "v2";
        assert(factory_ideas_request(IDEAS_UPDATE_ALL, NULL) == ESP_OK);
        assert(snapshot().batch && snapshot().cancellable);
        bool queued = !strcmp(scenario, "batch-cancel-queued");
        if (queued) assert(factory_ideas_cancel() == ESP_OK);
        else env->observe = cancel_batch_commit;
        mock_run_worker(); env->observe = NULL;
        factory_ideas_snapshot_t s = snapshot();
        assert(s.cancelled && !s.failed && !s.busy && !s.batch);
        unsigned updated = 0;
        for (size_t i = 0; i < s.count; ++i) updated += !strcmp(s.items[i].version, "2.0");
        assert(updated == (queued ? 0U : 1U));
        assert(queued || batch_commit_cancelled);
        assert(!env->installs && !env->restarts);
    } else if (!strcmp(scenario, "scan-overflow")) {
        download();
        uint32_t generation = snapshot().generation;
        for (unsigned i = 0; i < 64; ++i) {
            char path[128]; snprintf(path, sizeof(path), "/nand/ideas/foreign-%u", i);
            FILE *f = fopen(path, "wb"); assert(f && fclose(f) == 0);
        }
        run(IDEAS_SCAN);
        assert(snapshot().scan_error == ESP_ERR_INVALID_SIZE && snapshot().count == 1 &&
               snapshot().generation == generation);
    } else if (!strcmp(scenario, "scan-read-error")) {
        download(); code = "042732"; download(); code = "042731";
        uint32_t generation = snapshot().generation;
        env->fail_file = "/current/manifest.json"; env->fail_file_remaining = 1;
        run(IDEAS_SCAN);
        factory_ideas_snapshot_t s = snapshot();
        fprintf(stderr, "scan-read-error: count=%zu invalid=%zu error=%d\n", s.count, s.invalid, s.scan_error);
        assert(s.count == 2 && !s.invalid && s.scan_error == ESP_FAIL && s.generation == generation);
        env->fail_file = NULL; run(IDEAS_SCAN);
        assert(snapshot().count == 2 && snapshot().scan_error == ESP_OK);
    } else if (!strcmp(scenario, "scan-metadata-only")) {
        assert(!env->storage_info_calls); /* Initial scan never traverses capacity. */
        assert(!snapshot().storage_total);
        assert(factory_ideas_request(IDEAS_STORAGE_INFO, NULL) == ESP_OK);
        mock_run_worker();
        assert(env->storage_info_calls == 1 && snapshot().storage_total);
        mock_development();
        uint64_t before = env->flash_read_bytes;
        run(IDEAS_SCAN);
        assert(snapshot().development_available && !snapshot().failed && !env->read_lease);
        assert(env->storage_info_calls == 1 && !snapshot().storage_total);
        assert(env->flash_read_bytes - before == sizeof(esp_app_desc_t));
        assert(!env->writes);
        mock_change_resource();
        before = env->flash_read_bytes;
        run(IDEAS_SCAN);
        assert(env->flash_read_bytes - before == sizeof(esp_app_desc_t));
    } else if (!strncmp(scenario, "save-cancel-", 12)) {
        mock_development();
        run(IDEAS_SCAN); run(IDEAS_SAVE_DEVELOPMENT);
        factory_ideas_snapshot_t saved = snapshot();
        assert(saved.count == 1 && saved.development_saved && saved.items[0].boot);
        mock_change_resource();
        run(IDEAS_SCAN);
        bool previous_boot = snapshot().items[0].boot;
        assert(factory_ideas_request(IDEAS_SAVE_DEVELOPMENT, NULL) == ESP_OK);
        assert(snapshot().cancellable);
        if (!strcmp(scenario, "save-cancel-queued")) assert(factory_ideas_cancel() == ESP_OK);
        else if (!strcmp(scenario, "save-cancel-hash")) env->cancel_on_delay = true;
        else {
            save_cancel_mode = !strcmp(scenario, "save-cancel-gate") ? 1 :
                               !strcmp(scenario, "save-cancel-committed") ? 2 : 0;
            env->observe = cancel_save;
        }
        mock_run_worker();
        if (env->observe) assert(save_cancel_seen);
        env->observe = NULL;
        factory_ideas_snapshot_t done = snapshot();
        bool committed = save_cancel_mode == 2;
        assert(done.terminal && !done.failed && done.cancelled == !committed && !done.cancellable);
        assert(!env->read_lease && !env->storage_busy && !env->restarts && !env->installs);
        assert_no_pending_save();
        run(IDEAS_SCAN);
        done = snapshot();
        assert(done.count == (committed ? 2 : 1));
        bool previous = false;
        for (size_t i = 0; i < done.count; ++i)
            previous |= !strcmp(done.items[i].release_id, saved.items[0].release_id);
        assert(previous);
        if (!committed) {
            assert(done.items[0].boot == previous_boot);
            run(IDEAS_SAVE_DEVELOPMENT); /* Cancellation released admission and storage leases. */
            assert(!snapshot().failed && snapshot().count == 2 && snapshot().development_saved);
        }
    } else if (!strncmp(scenario, "development", 11)) {
        mock_development();
        env->ota_missing = true; /* Offline boot before a lazy Iris OTA session exists. */
        run(IDEAS_SCAN);
        assert(snapshot().development_available && !snapshot().development_saved);
        env->ota_busy = !strcmp(scenario, "development-ota-busy");
        run(IDEAS_SAVE_DEVELOPMENT);
        assert(snapshot().failed == env->ota_busy);
        if (env->ota_busy) assert(snapshot().count == 0 && !env->writes);
        else {
            factory_ideas_snapshot_t s = snapshot();
            assert(s.count == 1 && s.items[0].code[0] == 'd' && s.items[0].components == 3);
            assert(s.development_saved && s.items[0].boot);
            run(IDEAS_SAVE_DEVELOPMENT);
            assert(!snapshot().failed && snapshot().count == 1);
            env->network = false;
            assert(factory_ideas_request(IDEAS_OPEN, s.items[0].code) == ESP_OK);
            mock_run_worker();
            assert(!snapshot().failed && env->restarts == 1 && !env->installs);
            mock_change_resource();
            run(IDEAS_SCAN);
            assert(snapshot().development_available && !snapshot().development_saved);
            assert(factory_ideas_request(IDEAS_OPEN, s.items[0].code) == ESP_OK);
            mock_run_worker();
            assert(!snapshot().failed && env->restarts == 2 && env->installs == 1);
        }
    } else if (!strcmp(scenario, "preview") || !strcmp(scenario, "preview-zh") ||
               !strcmp(scenario, "preview-single-byte")) {
        if (!strcmp(scenario, "preview-zh")) assert(factory_locale_set(0) == ESP_OK);
        if (!strcmp(scenario, "preview-single-byte")) env->read_chunk = 1;
        uint32_t id = preview();
        copy_preview(id);
        assert(env->resolves == 1 && env->assets == 0 && !env->firmware && !env->writes);
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
            env->cancel_on_read = !strcmp(scenario, "cancel-resolve") ? "resolve" : "display.json";
        mock_run_worker();
        factory_ideas_snapshot_t s = snapshot();
        assert(!s.preview_ready && !s.resolving && !s.preview_id && !s.terminal);
        assert(!env->writes && !env->firmware);
        assert(env->assets == 0);
        assert(factory_ideas_confirm(id, true) == ESP_ERR_INVALID_STATE);
        assert(env->cancels == (!strcmp(scenario, "cancel-queued") ? 0 : 1));
        preview();
        dismiss();
    } else if (!strcmp(scenario, "preview-storage-failure")) {
        env->storage_failure = true;
        uint32_t id = preview();
        assert(snapshot().preview_ready && snapshot().scan_error == ESP_FAIL);
        assert(env->resolves == 1 && !env->writes && !env->installs && !env->lease);
        copy_preview(id);
        dismiss();
        assert(!snapshot().preview_ready && !env->lease);
        env->storage_failure = false;
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
        assert(env->assets == 3 && env->requests_with_identity == 0);
        id = preview();
        assert(snapshot().already_downloaded);
        unsigned writes = env->writes;
        confirm(id);
        assert(snapshot().return_library && !snapshot().terminal);
        assert(env->writes == writes && env->firmware == 2);
    } else if (!strcmp(scenario, "queue-failure")) {
        env->fail_send = true;
        assert(factory_ideas_request(IDEAS_RESOLVE, code) == ESP_ERR_INVALID_STATE);
        assert(!snapshot().busy && !snapshot().resolving && !snapshot().preview_id);
        uint32_t id = preview();
        env->fail_send = true;
        assert(factory_ideas_confirm(id, true) == ESP_ERR_INVALID_STATE);
        assert(snapshot().preview_ready && snapshot().preview_id == id && !snapshot().busy);
        copy_preview(id);
        confirm(id);
        library("1.0");
    } else if ((!strcmp(scenario, "retry-manifest") || !strcmp(scenario, "retry-read"))) {
        env->fault_file = !strcmp(scenario, "retry-manifest") ? "resolve" : "display.json";
        env->fault = !strcmp(scenario, "retry-read") ? MOCK_READ_ERROR : MOCK_SERVICE_UNAVAILABLE;
        env->fault_limit = 2;
        preview();
        assert(env->fault_hits == 3 && !env->writes && !env->firmware);
        dismiss();
    } else if (!strcmp(scenario, "retry-limit")) {
        env->fault_file = "resolve";
        env->fault = MOCK_SERVICE_UNAVAILABLE;
        run(IDEAS_RESOLVE);
        assert(snapshot().failed && env->fault_hits == 4 && !env->writes);
    } else if (!strcmp(scenario, "bilingual-offline")) {
        download();
        unsigned firmware_reads = env->firmware_reads;
        env->network = false;
        unsigned requests = env->resolves + env->assets + env->firmware;
        for (unsigned locale = 0; locale < 2; ++locale) {
            assert(factory_locale_set(locale) == ESP_OK);
            run(IDEAS_RELOCALIZE);
            run(IDEAS_PREVIEW);
            factory_ideas_snapshot_t s = snapshot();
            assert(s.preview_ready && s.view_only);
            copy_preview(s.preview_id);
            dismiss();
            size_t size = 0;
            void *title = factory_ideas_copy_title(code, &size);
            if (!title) {
                assert(snapshot().busy);
                mock_run_worker();
                title = factory_ideas_copy_title(code, &size);
            }
            assert(title && size > 14 && !memcmp(title, "qoif", 4));
            free(title);
        }
        assert(requests == env->resolves + env->assets + env->firmware);
        assert(firmware_reads == env->firmware_reads);
    } else if (!strncmp(scenario, "reject-", 7)) {
        const char *bad = scenario + 7;
        if (!strncmp(bad, "bad-", 4))
            env->release = bad;
        else {
            env->fault_file = "display.json";
            if (!strcmp(bad, "etag"))
                env->fault = MOCK_ETAG;
            else if (!strcmp(bad, "hash"))
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
        env->fault_file = "app.bin";
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
            FILE *f = fopen("/nand/ideas/042731/current/firmware/app.bin", "r+b");
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
            assert(!strcmp(s.title, "Installation failed"));
            assert(env->installs == 1); /* Corruption is caught in NOR, before boot commit. */
            assert(factory_ideas_request(IDEAS_START, code) == ESP_ERR_INVALID_STATE);
            run(IDEAS_OPEN);
            s = snapshot();
            assert(s.failed && !strcmp(s.title, "Open failed") && !env->restarts);
        }
        assert(requests == env->resolves + env->assets + env->firmware);
    } else if (!strcmp(scenario, "io-budget")) {
        download();
        unsigned reads = env->firmware_reads;
        run(IDEAS_SCAN);
        run(IDEAS_RELOCALIZE);
        assert(env->firmware_reads == reads);
        uint64_t flash_reads = env->flash_read_bytes;
        run(IDEAS_INSTALL);
        assert(!snapshot().failed && snapshot().items[0].boot);
        /* Only the installer reads NAND firmware. Its NOR verification is
         * authoritative; the service must not hash either source again. */
        assert(env->firmware_reads - reads == 1);
        assert(env->flash_read_bytes == flash_reads);
        reads = env->firmware_reads;
        run(IDEAS_CHECK);
        assert(!snapshot().failed && !snapshot().preview_ready);
        assert(env->firmware_reads - reads == 1);
        reads = env->firmware_reads;
        env->release = "v2";
        run(IDEAS_CHECK);
        assert(snapshot().preview_ready);
        assert(env->firmware_reads == reads);
        confirm(snapshot().preview_id);
        assert(!snapshot().failed && env->firmware_reads - reads == 1);
        assert(env->flash_read_bytes == flash_reads);
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
        assert(!snapshot().development_saved);
        run(IDEAS_INSTALL);
        assert(!snapshot().failed && snapshot().items[0].boot && env->flash_version == '2');
        mock_corrupt_flash();
        run(IDEAS_SCAN);
        assert(!snapshot().items[0].boot);
        assert(factory_ideas_request(IDEAS_START, code) == ESP_ERR_INVALID_STATE);
    } else if (!strcmp(scenario, "repair-current")) {
        download();
        FILE *f = fopen("/nand/ideas/042731/current/firmware/app.bin", "r+b");
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
    mock_stop_worker();
    puts(scenario);
    return 0;
}
