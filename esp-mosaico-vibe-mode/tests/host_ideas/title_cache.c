// SPDX-License-Identifier: Apache-2.0
#define main ideas_worker_fixture_main
#include "main.c"
#undef main
#include "vibe_ui.h"
#include "vui_ids.h"
#include "vibe_ui_state.h"

static uint16_t frame[480 * 480];
static void ui_snapshot(void *ctx, vibe_snapshot_t *out)
{
    (void)ctx;
    factory_ideas_snapshot_t value = snapshot();
    memset(out, 0, sizeof(*out));
    out->settings.language = 2; out->settings.revision = 1;
    out->storage_total = 512ULL * 1024 * 1024;
    out->nand_generation = value.generation; out->bundle_count = value.count;
    out->idea_busy = value.busy;
    for (size_t i = 0; i < value.count; ++i) {
        snprintf(out->bundles[i].code, sizeof(out->bundles[i].code), "%s", value.items[i].code);
        snprintf(out->bundles[i].release, sizeof(out->bundles[i].release), "%s", value.items[i].name);
        snprintf(out->bundles[i].version, sizeof(out->bundles[i].version), "%s", value.items[i].version);
    }
}
static int ui_command(void *ctx, vibe_command_t cmd, const char *a, const char *b)
{
    (void)ctx; (void)cmd; (void)a; (void)b; return ESP_OK;
}
static void *ui_title(void *ctx, const char *code, size_t *size)
{
    (void)ctx; return factory_ideas_copy_title(code, size);
}
static void settle(vui_handle_t ui)
{
    for (unsigned i = 0; i < 6; ++i) { vui_tick(ui, 50); vui_render(ui, frame); }
}
static void tap(vui_handle_t ui, int x, int y)
{
    vui_pointer(ui, x, y, true); vui_render(ui, frame);
    vui_pointer(ui, x, y, false); vui_render(ui, frame); settle(ui);
}
static unsigned load_visible(vui_handle_t ui, vibe_ui_t *controller)
{
    unsigned jobs = 0;
    for (unsigned i = 0; i < 32; ++i) {
        if (snapshot().busy) {
            assert(snapshot().command == IDEAS_ROW);
            mock_run_worker(); ++jobs;
        }
        vibe_ui_poll(controller); vui_render(ui, frame);
        if (!snapshot().busy) {
            unsigned completed = mock_environment()->jobs_finished;
            for (unsigned j = 0; j < 20; ++j) {
                vibe_ui_poll(controller); vui_render(ui, frame);
                assert(!snapshot().busy);
            }
            assert(mock_environment()->jobs_finished == completed);
            assert(jobs <= 8);
            return jobs;
        }
    }
    assert(!"visible title working set did not settle"); return 0;
}
int main(int argc, char **argv)
{
    assert(argc == 4);
    unsigned count = (unsigned)atoi(argv[1]); assert(count == 8 || count == 9 || count == 16);
    mock_environment_t *env = mock_environment();
    env->fixture_dir = argv[2]; env->mount_dir = argv[3];
    assert(factory_ideas_init() == ESP_OK); mock_run_worker();
    assert(factory_locale_set(0) == ESP_OK);
    char codes[16][7];
    for (unsigned i = 0; i < count; ++i) {
        snprintf(codes[i], sizeof(codes[i]), "%06u", 42731 + i);
        code = codes[i]; download();
    }
    preview(); dismiss(); assert(snapshot().count == count);
    /* A title queued before a locale switch must keep its original locale. */
    size_t title_size = 0;
    assert(!factory_ideas_copy_title(codes[0], &title_size) && snapshot().busy);
    assert(factory_locale_set(1) == ESP_OK);
    mock_run_worker();
    assert(factory_locale_set(0) == ESP_OK);
    void *title = factory_ideas_copy_title(codes[0], &title_size);
    assert(title && title_size && !snapshot().busy); free(title);
    assert(factory_locale_set(1) == ESP_OK);
    assert(!factory_ideas_copy_title(codes[0], &title_size) && snapshot().busy);
    mock_run_worker();
    assert(factory_locale_set(0) == ESP_OK);
    vui_handle_t ui; vibe_ui_t *controller;
    const vibe_services_t services = {.snapshot = ui_snapshot, .command = ui_command, .title = ui_title};
    assert(vui_create(&ui) == VUI_OK);
    assert(vibe_ui_create(ui, &services, &controller) == VUI_OK);
    settle(ui);
    assert(!snapshot().busy); /* The hidden Manage list starts no title work. */
    tap(ui, 240, 360);
    unsigned top = load_visible(ui, controller);
    assert(top && top < count);
    assert(vui_scroll_to(ui, VUI_VIBE_OBJ_KEY_BUNDLES, 100000) == VUI_OK);
    vui_render(ui, frame);
    unsigned bottom = load_visible(ui, controller);
    assert(vui_scroll_to(ui, VUI_VIBE_OBJ_KEY_BUNDLES, 352) == VUI_OK);
    vui_render(ui, frame);
    unsigned middle = load_visible(ui, controller);
    tap(ui, 38, 36);
    vibe_ui_status_t current; vibe_ui_get_status(controller, &current); assert(current.page == VIBE_HOME);
    assert(load_visible(ui, controller) == 0);
    /* The horizontal grid loads only its six visible names. Paging across more
     * than eight apps must settle without cache churn or blocking Open. */
    vibe_show(controller, VIBE_GRID);
    unsigned grid_jobs = load_visible(ui, controller);
    assert(!controller->grid_titles_pending);
    for (unsigned page = 1; page <= count / 6; ++page) {
        controller->grid_page = page;
        vibe_ui_poll(controller);
        grid_jobs += load_visible(ui, controller);
        assert(!controller->grid_titles_pending);
    }
    controller->grid_page = 0;
    vibe_ui_poll(controller); grid_jobs += load_visible(ui, controller);
    assert(!controller->grid_titles_pending);
    vibe_show(controller, VIBE_HOME);
    assert(load_visible(ui, controller) == 0);
    assert(factory_ideas_request(IDEAS_OPEN, codes[0]) == ESP_OK);
    mock_run_worker(); assert(!snapshot().failed && env->restarts == 1);
    printf("apps=%u title_jobs_top=%u bottom=%u middle=%u; hidden list idle; open accepted\n",
           count, top, bottom, middle);
    vibe_ui_deinit(controller); vui_destroy(ui);
    mock_stop_worker(); mock_assert_idle(); return 0;
}
