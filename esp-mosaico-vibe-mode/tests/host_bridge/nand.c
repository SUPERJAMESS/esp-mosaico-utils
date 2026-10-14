// Run the actual NAND reader with a failing allocator and checked write sink.
#define heap_caps_malloc unused_sdk_heap_caps_malloc
#include "backend_sdk.h"
#undef heap_caps_malloc
#include <assert.h>
#include <unistd.h>
#define CONFIG_IRIS_FACTORY_NAND_SYSTEM_UPDATE 1
#define CONFIG_IRIS_FACTORY_NAND_SYSTEM_UPDATE_TASK_STACK 8192
#define MALLOC_CAP_INTERNAL 4
#define portMAX_DELAY 0
#define pdTRUE 1
typedef int psa_status_t;
typedef int StaticSemaphore_t;
typedef void *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s) { return s; }
static inline int xSemaphoreTake(SemaphoreHandle_t s, int timeout) { (void)s; (void)timeout; return 1; }
static inline int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return 1; }
int mock_alloc_fail, mock_create_fail;
void (*mock_worker)(void *);
static void *chunk;
static unsigned begins, writes, ends;
static bool write_fail, digest_fail;
static uint8_t expected[8192 + 37], received[sizeof(expected)];
static void *heap_caps_malloc(size_t size, int caps) {
    assert(size == 4096 && caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    chunk = mock_alloc_fail ? NULL : malloc(size);
    return chunk;
}
#include NAND_SOURCE

esp_err_t factory_system_update_source_begin_component(factory_system_update_owner_t owner,
    const esp_iris_system_update_component_t *c) {
    assert(owner == FACTORY_SYSTEM_UPDATE_OWNER_NAND && c->size == sizeof(expected));
    assert(chunk); /* No erase may precede successful internal allocation. */
    begins++;
    return ESP_OK;
}
esp_err_t factory_system_update_source_write_component(factory_system_update_owner_t owner,
    const esp_iris_system_update_component_t *c, uint32_t offset, const uint8_t *data, size_t size) {
    (void)owner; (void)c;
    assert(data == chunk && size <= 4096 && offset + size <= sizeof(received));
    writes++;
    if (write_fail) return ESP_FAIL;
    memcpy(received + offset, data, size);
    return ESP_OK;
}
esp_err_t factory_system_update_source_end_component(factory_system_update_owner_t owner,
    const esp_iris_system_update_component_t *c, const uint8_t *digest) {
    (void)owner; (void)c;
    uint8_t actual[32];
    SHA256(expected, sizeof(expected), actual);
    assert(!memcmp(actual, digest, sizeof(actual)));
    assert(!memcmp(expected, received, sizeof(expected)));
    ends++;
    return digest_fail ? ESP_ERR_INVALID_CRC : ESP_OK;
}
int main(void) {
    char path[] = "/tmp/nand-reader-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    for (unsigned i = 0; i < sizeof(expected); ++i) expected[i] = (uint8_t)(i * 17);
    assert(write(fd, expected, sizeof(expected)) == sizeof(expected));
    close(fd);
    esp_iris_system_update_component_t c = {.size = sizeof(expected)};
    mock_alloc_fail = 1;
    assert(read_component(path, &c) == ESP_ERR_NO_MEM);
    assert(!begins && !writes && !ends);
    mock_alloc_fail = 0;
    assert(read_component(path, &c) == ESP_OK);
    assert(begins == 1 && writes == 3 && ends == 1);
    write_fail = true;
    assert(read_component(path, &c) == ESP_FAIL);
    assert(begins == 2 && writes == 4 && ends == 1);
    write_fail = false;
    digest_fail = true;
    assert(read_component(path, &c) == ESP_ERR_INVALID_CRC);
    assert(begins == 3 && writes == 7 && ends == 2);
    unlink(path);
    return 0;
}
