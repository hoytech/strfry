#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <stdatomic.h>

static void* (*real_malloc)(size_t) = NULL;
static void* (*real_calloc)(size_t, size_t) = NULL;
static void* (*real_realloc)(void*, size_t) = NULL;
static void (*real_free)(void*) = NULL;

static atomic_size_t total_allocations = 0;
static atomic_size_t total_bytes = 0;

static __thread int in_hook = 0;
static char bootstrap_buf[4096];
static size_t bootstrap_offset = 0;

static void init() {
    if (real_malloc) return;
    in_hook = 1;
    real_malloc = (void* (*)(size_t))dlsym(RTLD_NEXT, "malloc");
    real_calloc = (void* (*)(size_t, size_t))dlsym(RTLD_NEXT, "calloc");
    real_realloc = (void* (*)(void*, size_t))dlsym(RTLD_NEXT, "realloc");
    real_free = (void (*)(void*))dlsym(RTLD_NEXT, "free");
    in_hook = 0;
}

void* malloc(size_t size) {
    if (!real_malloc) init();
    if (in_hook) return real_malloc ? real_malloc(size) : NULL;
    
    in_hook = 1;
    void* ptr = real_malloc(size);
    in_hook = 0;
    
    if (ptr) {
        atomic_fetch_add(&total_allocations, 1);
        atomic_fetch_add(&total_bytes, size);
    }
    return ptr;
}

void* calloc(size_t nmemb, size_t size) {
    if (!real_calloc) {
        if (in_hook) {
            size_t total = nmemb * size;
            if (bootstrap_offset + total < sizeof(bootstrap_buf)) {
                void* ptr = &bootstrap_buf[bootstrap_offset];
                bootstrap_offset += total;
                return ptr;
            }
            return NULL;
        }
        init();
    }
    if (in_hook) return real_calloc ? real_calloc(nmemb, size) : NULL;
    
    in_hook = 1;
    void* ptr = real_calloc(nmemb, size);
    in_hook = 0;
    
    if (ptr) {
        atomic_fetch_add(&total_allocations, 1);
        atomic_fetch_add(&total_bytes, nmemb * size);
    }
    return ptr;
}

void* realloc(void* ptr, size_t size) {
    if (!real_realloc) init();
    if (in_hook) return real_realloc ? real_realloc(ptr, size) : NULL;
    
    in_hook = 1;
    void* new_ptr = real_realloc(ptr, size);
    in_hook = 0;
    
    if (new_ptr) {
        atomic_fetch_add(&total_allocations, 1);
        atomic_fetch_add(&total_bytes, size);
    }
    return new_ptr;
}

void free(void* ptr) {
    if (!real_free) init();
    if (real_free) {
        if (ptr >= (void*)bootstrap_buf && ptr < (void*)(bootstrap_buf + sizeof(bootstrap_buf))) {
            return;
        }
        real_free(ptr);
    }
}

__attribute__((destructor)) void report() {
    const char* out_path = getenv("ALLOC_TRACKER_OUT");
    FILE* f = stderr;
    if (out_path) {
        f = fopen(out_path, "a");
        if (!f) f = stderr;
    }
    fprintf(f, "ALLOCS: %zu\nBYTES: %zu\n", atomic_load(&total_allocations), atomic_load(&total_bytes));
    if (f != stderr) fclose(f);
}
