// Run the actual NAND reader with a failing allocator and checked write sink.
#define heap_caps_malloc unused_sdk_heap_caps_malloc
#define psa_hash_update unused_psa_hash_update
#define vTaskDeleteWithCaps unused_vTaskDeleteWithCaps
#include "backend_sdk.h"
#undef heap_caps_malloc
#undef psa_hash_update
#undef vTaskDeleteWithCaps
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#define CONFIG_IRIS_FACTORY_NAND_SYSTEM_UPDATE 1
#define CONFIG_IRIS_FACTORY_NAND_SYSTEM_UPDATE_TASK_STACK 8192
#define MALLOC_CAP_INTERNAL 4
#define MALLOC_CAP_DMA 8
#define portMAX_DELAY 0
#define pdTRUE 1
#define tskNO_AFFINITY -1
typedef int psa_status_t;
typedef int StaticSemaphore_t;
typedef void *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s) { return s; }
static inline int xSemaphoreTake(SemaphoreHandle_t s, int timeout) { (void)s; (void)timeout; return 1; }
static inline int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return 1; }
int mock_alloc_fail, mock_create_fail;
void (*mock_worker)(void *);
static void *chunks[2];
static unsigned allocations, begins, writes, ends, hashes, queues, tasks;
static bool write_fail, digest_fail, begin_fail, fail_extra, fail_queue, source_corrupt;
static bool extra_byte, check_overlap;
static unsigned short_read;
static atomic_uint reads;
static atomic_bool writing, overlapped;
static uint8_t expected[8192 + 37], received[sizeof(expected)];
static void *heap_caps_malloc(size_t size, int caps) {
    assert(size == 4096 && caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA));
    assert(allocations < 2);
    void *p = mock_alloc_fail || (fail_extra && allocations == 1) ? NULL : malloc(size);
    chunks[allocations++] = p;
    return p;
}
static int psa_hash_update(psa_hash_operation_t *h, const void *p, size_t n) {
    hashes++;
    return unused_psa_hash_update(h, p, n);
}
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    void *data;
    size_t size;
    bool full;
} *QueueHandle_t;
static QueueHandle_t xQueueCreate(unsigned length, size_t size) {
    assert(length == 1);
    if (fail_queue) return NULL;
    QueueHandle_t q = calloc(1, sizeof(*q));
    assert(q && (q->data = malloc(size)));
    q->size = size;
    assert(!pthread_mutex_init(&q->mutex, NULL) && !pthread_cond_init(&q->changed, NULL));
    queues++;
    return q;
}
static int xQueueSend(QueueHandle_t q, const void *data, int timeout) {
    (void)timeout;
    pthread_mutex_lock(&q->mutex);
    while (q->full) pthread_cond_wait(&q->changed, &q->mutex);
    memcpy(q->data, data, q->size);
    q->full = true;
    pthread_cond_broadcast(&q->changed);
    pthread_mutex_unlock(&q->mutex);
    return pdTRUE;
}
static int xQueueReceive(QueueHandle_t q, void *data, int timeout) {
    (void)timeout;
    pthread_mutex_lock(&q->mutex);
    while (!q->full) pthread_cond_wait(&q->changed, &q->mutex);
    memcpy(data, q->data, q->size);
    q->full = false;
    pthread_cond_broadcast(&q->changed);
    pthread_mutex_unlock(&q->mutex);
    return pdTRUE;
}
static void vQueueDelete(QueueHandle_t q) {
    assert(!q->full && queues);
    assert(!pthread_mutex_destroy(&q->mutex) && !pthread_cond_destroy(&q->changed));
    free(q->data); free(q); queues--;
}
typedef struct { pthread_t thread; void (*entry)(void *); void *arg; } reader_task_t;
static void *run_reader(void *p) {
    reader_task_t *t = p;
    t->entry(t->arg);
    return NULL;
}
static int xTaskCreatePinnedToCoreWithCaps(void (*entry)(void *), const char *name,
    unsigned stack, void *arg, unsigned priority, TaskHandle_t *out, int core, int caps) {
    (void)name; (void)priority; (void)core; (void)caps;
    assert(stack >= 4096);
    if (mock_create_fail) return 0;
    reader_task_t *t = calloc(1, sizeof(*t));
    assert(t);
    t->entry = entry; t->arg = arg; *out = t;
    assert(!pthread_create(&t->thread, NULL, run_reader, t));
    tasks++;
    return pdPASS;
}
static void vTaskSuspend(TaskHandle_t task) { assert(!task); pthread_exit(NULL); }
static void vTaskDeleteWithCaps(TaskHandle_t task) {
    reader_task_t *t = task;
    assert(tasks && !pthread_join(t->thread, NULL));
    free(t); tasks--;
}
static size_t test_fread(void *p, size_t unit, size_t size, FILE *f) {
    assert(unit == 1);
    unsigned call = atomic_fetch_add(&reads, 1) + 1;
    if (check_overlap && call == 2) {
        for (unsigned i = 0; !atomic_load(&writing) && i < 1000; ++i) usleep(1000);
        assert(atomic_load(&writing));
        atomic_store(&overlapped, true);
    }
    size_t n = fread(p, unit, short_read == call ? size / 2 : size, f);
    if (source_corrupt && n) ((uint8_t *)p)[0] ^= 1;
    return n;
}
static int test_fgetc(FILE *f) { return extra_byte ? 42 : fgetc(f); }
#define fread test_fread
#define fgetc test_fgetc
#include NAND_SOURCE

static void reset(void) {
    assert(!queues && !tasks);
    allocations = begins = writes = ends = hashes = 0;
    mock_alloc_fail = mock_create_fail = 0;
    write_fail = digest_fail = begin_fail = fail_extra = fail_queue = source_corrupt = false;
    extra_byte = check_overlap = false;
    short_read = 0;
    atomic_store(&reads, 0);
    atomic_store(&writing, false);
    atomic_store(&overlapped, false);
    memset(chunks, 0, sizeof(chunks));
    memset(received, 0, sizeof(received));
}

esp_err_t factory_system_update_source_begin_component(factory_system_update_owner_t owner,
    const esp_iris_system_update_component_t *c) {
    assert(owner == FACTORY_SYSTEM_UPDATE_OWNER_NAND && c->size == sizeof(expected));
    assert(chunks[0]); /* No erase may precede successful internal allocation. */
    begins++;
    return begin_fail ? ESP_FAIL : ESP_OK;
}
esp_err_t factory_system_update_source_write_component(factory_system_update_owner_t owner,
    const esp_iris_system_update_component_t *c, uint32_t offset, const uint8_t *data, size_t size) {
    (void)owner; (void)c;
    assert((data == chunks[0] || data == chunks[1]) && size <= 4096 && offset + size <= sizeof(received));
    writes++;
    if (check_overlap && !offset) {
        atomic_store(&writing, true);
        for (unsigned i = 0; !atomic_load(&overlapped) && i < 1000; ++i) usleep(1000);
        assert(atomic_load(&overlapped));
    }
    if (write_fail) return ESP_FAIL;
    memcpy(received + offset, data, size);
    return ESP_OK;
}
esp_err_t factory_system_update_source_end_idea_component(const esp_iris_system_update_component_t *c) {
    assert(c->size == sizeof(expected) && !hashes);
    ends++;
    uint8_t digest[32];
    SHA256(received, sizeof(received), digest);
    return digest_fail || memcmp(digest, c->sha256, sizeof(digest)) ? ESP_ERR_INVALID_CRC : ESP_OK;
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
int main(int argc, char **argv) {
    assert(argc == 2);
    char *path = argv[1];
    int fd = mkstemp(path);
    assert(fd >= 0);
#ifdef _WIN32
    assert(_setmode(fd, _O_BINARY) >= 0);
#endif
    for (unsigned i = 0; i < sizeof(expected); ++i) expected[i] = (uint8_t)(i * 17);
    assert(write(fd, expected, sizeof(expected)) == sizeof(expected));
    close(fd);
    esp_iris_system_update_component_t c = {.size = sizeof(expected)};
    SHA256(expected, sizeof(expected), c.sha256);
    for (unsigned idea = 0; idea < 2; ++idea) {
        reset(); mock_alloc_fail = 1;
        assert(read_component(path, &c, idea) == ESP_ERR_NO_MEM);
        assert(!begins && !writes && !ends);
        reset(); check_overlap = idea;
        assert(read_component(path, &c, idea) == ESP_OK);
        assert(begins == 1 && writes == 3 && ends == 1 && hashes == (idea ? 0 : 3));
        assert(atomic_load(&reads) == 3 && !memcmp(expected, received, sizeof(expected)));
        reset(); write_fail = true;
        assert(read_component(path, &c, idea) == ESP_FAIL);
        assert(begins == 1 && writes == 1 && !ends);
        reset(); begin_fail = true;
        assert(read_component(path, &c, idea) == ESP_FAIL);
        assert(begins == 1 && !writes && !ends);
        reset(); digest_fail = true;
        assert(read_component(path, &c, idea) == ESP_ERR_INVALID_CRC);
        assert(begins == 1 && writes == 3 && ends == 1);
        reset();
        for (short_read = 1; short_read <= 3;) {
            unsigned fail_at = short_read;
            assert(read_component(path, &c, idea) == ESP_ERR_INVALID_SIZE);
            reset(); short_read = fail_at + 1;
        }
        reset(); extra_byte = true;
        assert(read_component(path, &c, idea) == ESP_ERR_INVALID_SIZE && !ends);
    }
    reset(); source_corrupt = true;
    assert(read_component(path, &c, true) == ESP_ERR_INVALID_CRC && ends == 1);
    for (unsigned fallback = 0; fallback < 3; ++fallback) {
        reset();
        fail_extra = fallback == 0; fail_queue = fallback == 1; mock_create_fail = fallback == 2;
        assert(read_component(path, &c, true) == ESP_OK);
        assert(writes == 3 && ends == 1 && !hashes);
    }
    reset();
    unlink(path);
    return 0;
}
