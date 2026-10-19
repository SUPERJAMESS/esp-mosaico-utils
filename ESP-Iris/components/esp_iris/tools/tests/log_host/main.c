#include "sdk.h"
#include <stdlib.h>

#define fopen host_vfs_open
#include "esp_iris_log.c"
#include "esp_iris_startup.c"
#undef fopen

iris_runtime_t g_iris;
static const esp_vfs_fs_ops_t *ops;
static void *vfs_context;
static bool fail_register, fail_open;
static unsigned registrations, app_calls;

esp_err_t esp_vfs_register_fs(const char *path, const esp_vfs_fs_ops_t *value,
                             int flags, void *ctx)
{
    assert(ops == NULL);
    if (fail_register) return ESP_FAIL;
    ops = value; vfs_context = ctx; ++registrations; return ESP_OK;
}
esp_err_t esp_vfs_unregister_fs(const char *path)
{ assert(ops != NULL); ops = NULL; vfs_context = NULL; return ESP_OK; }

static ssize_t cookie_write(void *cookie, const char *data, size_t size)
{ return ops->write_p(vfs_context, (int)(intptr_t)cookie, data, size); }

FILE *host_vfs_open(const char *path, const char *mode)
{
    if (fail_open) return NULL;
    assert(ops != NULL);
    const int fd = ops->open_p(vfs_context, path + strlen(IRIS_LOG_VFS_PATH), 0, 0);
    assert(fd == 1 || fd == 2);
    return fopencookie((void *)(intptr_t)fd, mode,
                      (cookie_io_functions_t){.write = cookie_write});
}

void __real_app_main(void)
{
    ++app_calls;
    fprintf(stdout, "first application log\n");
    fprintf(stderr, "early error\n");
}

int main(void)
{
    FILE *saved_out = stdout, *saved_err = stderr;
    FILE *native = tmpfile();
    assert(native != NULL);
    stdout = stderr = native;

    /* Capture failure must preserve native output and still run app_main. */
    fail_register = true;
    __wrap_app_main();
    assert(app_calls == 1 && stdout == native && stderr == native && !ops);
    fail_register = false;
    fail_open = true;
    __wrap_app_main();
    assert(app_calls == 2 && stdout == native && stderr == native && !ops);
    fail_open = false;

    __wrap_app_main();
    assert(app_calls == 3 && g_iris.stdio_redirected && g_iris.vfs_registered);
    const size_t early_size = g_iris.log_used;
    const unsigned registered = registrations;
    /* esp_iris_start adopts the existing tap without resetting or reopening. */
    assert(iris_log_capture_start(&g_iris) == ESP_OK);
    assert(g_iris.log_used == early_size && registrations == registered);
    fprintf(stdout, "USB initialization log\n");
    iris_log_record_t record;
    const char *expected[] = {"first application log\n", "early error\n", "USB initialization log\n"};
    for (unsigned i = 0; i < 3; ++i) {
        assert(iris_log_pop(&g_iris, UINT32_MAX, &record));
        assert(record.length == strlen(expected[i]));
        assert(memcmp(record.data, expected[i], record.length) == 0);
        assert(record.source == (i == 1 ? 2 : 1));
    }
    assert(!iris_log_pop(&g_iris, UINT32_MAX, &record));
    assert(g_iris_log_storage.retained_bytes > 0); /* Crash evidence survives TX. */

    /* A late reader gets the remaining records in order, with loss counted. */
    for (unsigned i = 0; i < 200; ++i) fprintf(stdout, "boot line %03u\n", i);
    assert(g_iris.log_dropped_bytes > 0);
    assert(g_iris.log_used <= CONFIG_ESP_IRIS_LOG_RING_BYTES);
    unsigned previous = 0, count = 0;
    while (iris_log_pop(&g_iris, UINT32_MAX, &record)) {
        char text[IRIS_LOG_RECORD_DATA_MAX + 1];
        memcpy(text, record.data, record.length); text[record.length] = '\0';
        unsigned current;
        assert(sscanf(text, "boot line %u", &current) == 1);
        if (count) assert(current == previous + 1);
        previous = current; ++count;
        assert(record.dropped_total == g_iris.log_dropped_bytes);
    }
    assert(count > 0 && previous == 199);
    assert(iris_log_capture_stop(&g_iris) == ESP_OK);
    assert(stdout == native && stderr == native && !ops);
    assert(iris_log_capture_start(&g_iris) == ESP_OK);
    assert(g_iris.log_used == 0 && g_iris.log_dropped_bytes == 0);
    assert(iris_log_capture_stop(&g_iris) == ESP_OK);

    fflush(native); rewind(native);
    char mirrored[8192];
    const size_t length = fread(mirrored, 1, sizeof(mirrored) - 1, native);
    mirrored[length] = '\0';
    assert(strstr(mirrored, "first application log\n") != NULL);
    assert(strstr(mirrored, "boot line 199\n") != NULL);
    stdout = saved_out; stderr = saved_err;
    fclose(native);
    return 0;
}
