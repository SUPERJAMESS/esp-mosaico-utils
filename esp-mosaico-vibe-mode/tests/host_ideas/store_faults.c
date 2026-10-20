// SPDX-License-Identifier: Apache-2.0
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static unsigned nth, allocations, hits, kind;
static unsigned io_nth, io_seen;
static char target[512];
static FILE *watched;
static int read_error;

void store_fault_reset(void) {
    nth = allocations = hits = kind = io_seen = 0;
    io_nth = 1;
    watched = NULL;
    read_error = 0;
    target[0] = 0;
}
void store_fault_allocation(unsigned n) { store_fault_reset(); nth = n; }
void store_fault_io(unsigned mode, const char *path) {
    store_fault_reset(); kind = mode; snprintf(target, sizeof(target), "%s", path);
}
void store_fault_io_at(unsigned mode, const char *path, unsigned n) {
    store_fault_io(mode, path); io_nth = n;
}
unsigned store_fault_hits(void) { return hits; }
unsigned store_fault_allocations(void) { return allocations; }
static int allocation_failed(void) {
    ++allocations;
    if (nth && allocations == nth) { ++hits; errno = ENOMEM; return 1; }
    return 0;
}
static int io_failed(unsigned mode, const char *path) {
    const char *match = strstr(path, target);
    if (kind == mode && !hits && match && (mode != 5 || !match[strlen(target)]) &&
        ++io_seen == io_nth) { ++hits; errno = EIO; return 1; }
    return 0;
}
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void *__wrap_malloc(size_t n) { return allocation_failed() ? NULL : __real_malloc(n); }
void *__wrap_calloc(size_t n, size_t size) { return allocation_failed() ? NULL : __real_calloc(n, size); }
void *__wrap_realloc(void *p, size_t n) { return allocation_failed() ? NULL : __real_realloc(p, n); }
FILE *__real_fopen(const char *, const char *);
FILE *__wrap_fopen(const char *path, const char *mode) {
    if (io_failed(1, path)) return NULL;
    FILE *f = __real_fopen(path, mode);
    if (kind == 4 && strstr(path, target)) watched = f;
    return f;
}
int __real_lstat(const char *, struct stat *);
int __wrap_lstat(const char *path, struct stat *out) { return io_failed(kind == 5 ? 5 : 2, path) ? -1 : __real_lstat(path, out); }
DIR *__real_opendir(const char *);
DIR *__wrap_opendir(const char *path) { return io_failed(3, path) ? NULL : __real_opendir(path); }
size_t __real_fread(void *, size_t, size_t, FILE *);
size_t __wrap_fread(void *out, size_t size, size_t count, FILE *f) {
    if (kind == 4 && f == watched && !hits) { ++hits; read_error = 1; errno = EIO; return 0; }
    return __real_fread(out, size, count, f);
}
int __real_ferror(FILE *);
int __wrap_ferror(FILE *f) { return f == watched && read_error ? 1 : __real_ferror(f); }
int __real_fclose(FILE *);
int __wrap_fclose(FILE *f) {
    if (f == watched) { watched = NULL; read_error = 0; }
    return __real_fclose(f);
}
