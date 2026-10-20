// SPDX-License-Identifier: Apache-2.0
#include "sdk.h"
#include "storage_volume.h"
#include "lfs.h"
#include <string.h>

enum { PAGE = 256, BLOCKS = 64, ORIGINAL_BLOCKS = 48 };
static uint8_t media[PAGE * BLOCKS], preserved[sizeof(media)];
static unsigned writes, erases;
static bool fail_read;
static int read_fs(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *p, lfs_size_t n) {
    if (fail_read) return LFS_ERR_IO;
    memcpy(p, media + block * PAGE + off, n); return 0;
}
static int write_fs(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *p, lfs_size_t n) {
    ++writes; memcpy(media + block * PAGE + off, p, n); return 0;
}
static int erase_fs(const struct lfs_config *c, lfs_block_t block) {
    ++erases; memset(media + block * PAGE, 0xff, PAGE); return 0;
}
static int sync_fs(const struct lfs_config *c) { return 0; }
static esp_err_t read_device(esp_blockdev_handle_t d, uint8_t *p, size_t capacity, uint64_t off, size_t n) {
    if (fail_read || capacity < n || off > d->geometry.disk_size || n > d->geometry.disk_size - off) return ESP_FAIL;
    memcpy(p, media + off, n); return ESP_OK;
}
static esp_err_t forbidden_write(esp_blockdev_handle_t d, const uint8_t *p, uint64_t off, size_t n) {
    assert(!"probe must not write"); return ESP_FAIL;
}
static esp_err_t forbidden_erase(esp_blockdev_handle_t d, uint64_t off, size_t n) {
    assert(!"probe must not erase"); return ESP_FAIL;
}
static const esp_blockdev_ops_t ops = {.read=read_device, .write=forbidden_write, .erase=forbidden_erase};
int main(void) {
    struct lfs_config config = {.read=read_fs, .prog=write_fs, .erase=erase_fs, .sync=sync_fs,
        .read_size=PAGE, .prog_size=PAGE, .block_size=PAGE, .block_count=ORIGINAL_BLOCKS,
        .block_cycles=-1, .cache_size=PAGE, .lookahead_size=16};
    memset(media, 0xff, sizeof(media));
    lfs_t fs; lfs_file_t file;
    assert(!lfs_format(&fs,&config) && !lfs_mount(&fs,&config));
    const char payload[] = "Retained Idea survives an FTL capacity change.";
    assert(!lfs_file_open(&fs,&file,"old-idea.bin",LFS_O_CREAT|LFS_O_WRONLY));
    assert(lfs_file_write(&fs,&file,payload,sizeof(payload))==sizeof(payload));
    assert(!lfs_file_close(&fs,&file) && !lfs_unmount(&fs));
    memcpy(preserved,media,sizeof(media));
    const unsigned old_writes=writes, old_erases=erases;
    esp_blockdev_t device = {.ops=&ops, .geometry={sizeof(media),PAGE,PAGE,PAGE}};
    uint64_t bytes = 0;
    for (unsigned count=ORIGINAL_BLOCKS; count<=BLOCKS; ++count) {
        device.geometry.disk_size=count*PAGE;
        assert(storage_volume_size(&device,&bytes)==ESP_OK && bytes==ORIGINAL_BLOCKS*PAGE);
        assert(writes==old_writes && erases==old_erases && !memcmp(media,preserved,sizeof(media)));
    }
    /* The original-size mount can still read the complete saved file. */
    assert(!lfs_mount(&fs,&config) && !lfs_file_open(&fs,&file,"old-idea.bin",LFS_O_RDONLY));
    char readback[sizeof(payload)];
    assert(lfs_file_read(&fs,&file,readback,sizeof(readback))==sizeof(readback));
    assert(!memcmp(readback,payload,sizeof(payload)));
    assert(!lfs_file_close(&fs,&file) && !lfs_unmount(&fs));
    device.geometry.disk_size=(ORIGINAL_BLOCKS-1)*PAGE;
    assert(storage_volume_size(&device,&bytes)==ESP_ERR_INVALID_SIZE && !bytes);
    device.geometry.disk_size=sizeof(media); fail_read=true;
    assert(storage_volume_size(&device,&bytes)==ESP_FAIL && !bytes); fail_read=false;
    memset(media,0,2*PAGE);
    assert(storage_volume_size(&device,&bytes)==ESP_FAIL && !bytes);
    assert(writes==old_writes && erases==old_erases);
    memset(media,0xff,sizeof(media));
    assert(storage_volume_size(&device,&bytes)==ESP_FAIL && !bytes);
    device.geometry.write_size=PAGE/2;
    assert(storage_volume_size(&device,&bytes)==ESP_ERR_INVALID_ARG && !bytes);
    return 0;
}
