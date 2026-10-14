#include "sdk.h"
#include SOURCE

struct mock_timer { esp_timer_create_args_t args; bool active; int64_t deadline; };
static struct mock_timer timer;
static unsigned restarts, marks, timer_creates;
static bool fail_arm, fail_mark;
static bool writer_busy;
bool factory_system_update_writer_busy(void) { return writer_busy; }
static const esp_partition_t running = {.address = 0x20000, .subtype = 32, .label = "vibe_mode"};
static const esp_partition_t target = {.address = 0x210000, .subtype = 16, .label = "ota_0"};
static const esp_partition_t alternate = {.address = 0x900000, .subtype = 17, .label = "ota_1"};
static const esp_partition_t *partitions[] = {&running, &target, &alternate};
static size_t partition_count = 2;
static uint32_t last_good;
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out)
{
    assert(!timer_creates++); timer.args = *args; *out = &timer; return ESP_OK;
}
bool esp_timer_is_active(esp_timer_handle_t t) { assert(t == &timer); return t->active; }
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t delay)
{
    if (fail_arm) return ESP_FAIL;
    assert(t == &timer && !t->active && delay == 1000000);
    t->active = true; t->deadline = mock_now + delay; return ESP_OK;
}
void esp_restart(void) { ++restarts; }
esp_err_t esp_iris_mark_planned_restart(void) { ++marks; return fail_mark ? ESP_FAIL : ESP_OK; }
esp_err_t esp_iris_start(void) { assert(timer_creates == 1); return ESP_OK; }
esp_err_t esp_iris_rpc_register(unsigned s, unsigned m, mock_rpc_t f, void *ctx) { return ESP_OK; }
esp_err_t esp_iris_get_status(esp_iris_status_t *s) { memset(s, 0, sizeof(*s)); return ESP_OK; }
const esp_partition_t *esp_ota_get_running_partition(void) { return &running; }
const esp_partition_t *esp_ota_get_boot_partition(void) { return &target; }
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *p) { return &target; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p, esp_ota_img_states_t *s) { *s = 0; return ESP_OK; }
esp_partition_iterator_t esp_partition_find(int t, int s, const char *l) { return partitions; }
const esp_partition_t *esp_partition_get(esp_partition_iterator_t i) { return *(const esp_partition_t **)i; }
void esp_partition_iterator_release(esp_partition_iterator_t i) { }
esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t i)
{
    const esp_partition_t **next = (const esp_partition_t **)i + 1;
    return next < partitions + partition_count ? next : NULL;
}
const esp_app_desc_t *esp_app_get_description(void) { static const esp_app_desc_t d = {0}; return &d; }
esp_err_t nvs_open_from_partition(const char *p, const char *ns, int mode, nvs_handle_t *h) { *h = 1; return ESP_OK; }
esp_err_t nvs_get_u32(nvs_handle_t h, const char *k, uint32_t *v) { *v = strcmp(k, "last_good") == 0 ? last_good : 0; return ESP_OK; }
esp_err_t nvs_set_u32(nvs_handle_t h, const char *k, uint32_t v) { return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { return ESP_OK; }
void nvs_close(nvs_handle_t h) { }
static void advance(int64_t us)
{
    mock_now += us;
    if (timer.active && mock_now >= timer.deadline) {
        timer.active = false; timer.args.callback(timer.args.arg);
    }
}
int main(void)
{
    uint32_t selected = 0;
    writer_busy = true;
    assert(esp_iris_platform_select_ota_target(target.address, &selected) == ESP_ERR_INVALID_STATE);
    writer_busy = false;
    assert(esp_iris_platform_select_ota_target(target.address, &selected) == ESP_OK);
    assert(selected == target.address);
    last_good = target.address;
    /* APP_TEST equals OTA_MAX, an exclusive upper bound. A retained single-slot
     * writer must replace main_app, never its own Vibe Mode image. */
    assert(esp_iris_platform_select_ota_target(target.address, &selected) == ESP_OK);
    assert(selected == target.address);
    partition_count = 3;
    assert(esp_iris_platform_select_ota_target(target.address, &selected) == ESP_OK);
    assert(selected == alternate.address);
    partition_count = 2;
    recovery_ota_support_start();
    assert(!timer.active && !restarts);
    esp_iris_platform_ota_committed();
    assert(timer.active && !restarts && marks == 1);
    advance(500000);
    esp_iris_platform_ota_committed(); /* Cannot defer an already committed restart. */
    advance(499999); assert(!restarts);
    /* No host command, Iris poll, or session state is needed for this callback. */
    advance(1); assert(restarts == 1);
    fail_mark = true;
    esp_iris_platform_ota_committed();
    advance(1000000); assert(restarts == 2);
    fail_arm = true;
    esp_iris_platform_ota_committed();
    assert(restarts == 3); /* Already selected image: scheduling failure cannot strand it. */
    puts("Recovery OTA restart is delayed, session independent and allocation free at commit");
}
