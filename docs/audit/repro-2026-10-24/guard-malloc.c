#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/mman.h>
#include <unistd.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
static void *guarded, *mapping;
void *__real_malloc(size_t);
void __real_free(void *);
void *__wrap_malloc(size_t n) {
    if (n != 7) return __real_malloc(n);
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    mapping = mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) abort();
    if (mprotect((char *)mapping + page, page, PROT_NONE)) abort();
    guarded = (char *)mapping + page - n;
    fprintf(stderr, "input allocation ends at unreadable guard page\n");
    return guarded;
}
void __wrap_free(void *p) {
    if (p == guarded) { munmap(mapping, (size_t)sysconf(_SC_PAGESIZE) * 2); guarded = NULL; }
    else __real_free(p);
}
