// SPDX-License-Identifier: Apache-2.0
#include "lfs.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PAGE=256, BLOCKS=4096, BYTES=PAGE*BLOCKS };
static unsigned char media[BYTES], original[BYTES];
static unsigned reads;
static int read_fs(const struct lfs_config *c,lfs_block_t block,lfs_off_t off,void *p,lfs_size_t size) {
    ++reads;memcpy(p,media+block*PAGE+off,size);return 0;
}
static int write_fs(const struct lfs_config *c,lfs_block_t block,lfs_off_t off,const void *p,lfs_size_t size) {
    memcpy(media+block*PAGE+off,p,size);return 0;
}
static int erase_fs(const struct lfs_config *c,lfs_block_t block) {memset(media+block*PAGE,0xff,PAGE);return 0;}
static int sync_fs(const struct lfs_config *c) {return 0;}
static void write_file(lfs_t *fs,const char *name,unsigned bytes) {
    unsigned char data[1024];memset(data,0x5a,sizeof(data));
    lfs_file_t file;assert(!lfs_file_open(fs,&file,name,LFS_O_CREAT|LFS_O_WRONLY));
    while(bytes) {
        unsigned size=bytes<sizeof(data)?bytes:sizeof(data);
        assert(lfs_file_write(fs,&file,data,size)==(lfs_ssize_t)size);bytes-=size;
    }
    assert(!lfs_file_close(fs,&file));
}
int main(void) {
    struct lfs_config config={.read=read_fs,.prog=write_fs,.erase=erase_fs,.sync=sync_fs,
        .read_size=PAGE,.prog_size=PAGE,.block_size=PAGE,.block_count=BLOCKS,
        .block_cycles=-1,.cache_size=PAGE,.lookahead_size=512};
    memset(media,0xff,sizeof(media));
    lfs_t fs;assert(!lfs_format(&fs,&config) && !lfs_mount(&fs,&config));
    write_file(&fs,"existing.bin",512*1024);assert(!lfs_unmount(&fs));
    memcpy(original,media,sizeof(media));
    unsigned counts[2];
    for(unsigned i=0;i<2;i++) {
        memcpy(media,original,sizeof(media));
        config.lookahead_size=i?512:8; /* Match 64x bitmap coverage on real NAND. */
        reads=0;assert(!lfs_mount(&fs,&config));
        write_file(&fs,"new.bin",128*1024);
        assert(!lfs_unmount(&fs));counts[i]=reads;
        assert(!lfs_mount(&fs,&config));
        lfs_file_t file;unsigned char data[1024];
        assert(!lfs_file_open(&fs,&file,"existing.bin",LFS_O_RDONLY));
        unsigned total=0;
        for(;;) {
            lfs_ssize_t n=lfs_file_read(&fs,&file,data,sizeof(data));assert(n>=0);
            if(!n)break;
            for(lfs_ssize_t j=0;j<n;j++)assert(data[j]==0x5a);
            total+=n;
        }
        assert(total==512*1024 && !lfs_file_close(&fs,&file) && !lfs_unmount(&fs));
    }
    printf("allocation reads: narrow=%u full=%u\n",counts[0],counts[1]);
    assert(counts[1]<counts[0]);
    return 0;
}
