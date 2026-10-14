// SPDX-License-Identifier: Apache-2.0
#include "sdk.h"
#include "storage_blockdev.h"
#include "storage_metrics.h"
#include <string.h>
#include <stdio.h>
int fail_allocation;
static uint8_t media[16*4096];
static unsigned reads,releases,writes;static bool fail_read,fail_write,fail_sync;static size_t page;
void storage_metrics_add(storage_metric_t k,uint64_t v){(void)k;(void)v;}
static int read_media(esp_blockdev_handle_t d,uint8_t*b,size_t cap,uint64_t a,size_t z){reads++;if(fail_read){memset(b,0xcc,z);return ESP_FAIL;}memcpy(b,media+a,z);return 0;}
static int write_media(esp_blockdev_handle_t d,const uint8_t*b,uint64_t a,size_t z){writes++;memcpy(media+a,b,z);return fail_write?ESP_FAIL:0;}
static int erase_media(esp_blockdev_handle_t d,uint64_t a,size_t z){memset(media+a,0xff,z);return 0;}
static int sync_media(esp_blockdev_handle_t d){return fail_sync?ESP_FAIL:0;}
static int ioctl_media(esp_blockdev_handle_t d,uint8_t k,void *arg){memset(media,0x55,page);return 0;}
static int release_media(esp_blockdev_handle_t d){releases++;return 0;}
static const esp_blockdev_ops_t ops={read_media,write_media,erase_media,sync_media,ioctl_media,release_media};
static void check_read(esp_blockdev_handle_t d,unsigned p){uint8_t b[4096];assert(d->ops->read(d,b,page,p*page,page)==0);assert(!memcmp(b,media+p*page,page));}
static void *reader(void *arg){for(int i=0;i<1000;i++)check_read(arg,i%3);return NULL;}
static void check(size_t p){
 page=p;reads=releases=writes=0;memset(media,0xa5,sizeof(media));
 esp_blockdev_t source={.geometry={sizeof(media),p,p,p},.ops=&ops};esp_blockdev_handle_t d;
 assert(storage_blockdev_wrap(&source,2*p,&d)==0);assert(d->geometry.disk_size==source.geometry.disk_size);
 check_read(d,0);check_read(d,1);check_read(d,0);assert(reads==2);check_read(d,2);check_read(d,0);assert(reads==3);check_read(d,1);assert(reads==4);
 uint8_t b[8192];assert(d->ops->read(d,b,sizeof(b),0,2*p)==0);assert(!memcmp(b,media,2*p));
 assert(d->ops->read(d,b,p,0,2*p)==ESP_ERR_INVALID_ARG);
 assert(d->ops->read(d,b,sizeof(b),UINT64_MAX-p,p)==ESP_ERR_INVALID_ARG);
 assert(d->ops->read(d,b,p,1,p)==ESP_ERR_INVALID_ARG);
 assert(d->ops->read(d,b,p,0,p-1)==ESP_ERR_INVALID_ARG);
 memset(b,0x12,p);assert(d->ops->write(d,b,0,p)==0);unsigned n=reads;check_read(d,0);assert(reads==n+1);
 fail_write=true;memset(b,0x34,p);assert(d->ops->write(d,b,0,p)==ESP_FAIL);fail_write=false;n=reads;check_read(d,0);assert(reads==n+1);
 assert(d->ops->erase(d,0,p)==0);n=reads;check_read(d,0);assert(reads==n+1);
 assert(d->ops->ioctl(d,0,NULL)==0);n=reads;check_read(d,0);assert(reads==n+1);
 fail_sync=true;assert(d->ops->sync(d)==ESP_FAIL);fail_sync=false;n=reads;check_read(d,0);assert(reads==n+1);
 fail_read=true;assert(d->ops->read(d,b,p,8*p,p)==ESP_FAIL);fail_read=false;n=reads;check_read(d,0);check_read(d,8);assert(reads==n+2);
 pthread_t a,c;assert(!pthread_create(&a,NULL,reader,d));assert(!pthread_create(&c,NULL,reader,d));pthread_join(a,NULL);pthread_join(c,NULL);
 assert(d->ops->release(d)==0);assert(releases==1);
 for(int i=1;i<=3;i++){
  fail_allocation=i;d=NULL;int err=storage_blockdev_wrap(&source,2*p,&d);
  if(i==1){assert(err==ESP_ERR_NO_MEM);assert(!d);}
  else{assert(!err);n=reads;check_read(d,0);check_read(d,0);assert(reads==n+2);assert(d->ops->release(d)==0);}
 }
 fail_allocation=0;assert(storage_blockdev_wrap(&source,0,&d)==0);n=reads;check_read(d,0);check_read(d,0);assert(reads==n+2);assert(!d->ops->release(d));
}
int main(void){check(2048);check(4096);puts("cache: LRU, geometry, multi-page, mutation/error invalidation, concurrent readers and allocation fallback passed");}
