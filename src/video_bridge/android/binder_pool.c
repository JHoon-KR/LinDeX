#include "binder_pool.h"

#if !defined(__ANDROID__)
#error "binder_pool.c is Android-only"
#endif

#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static void *binder_ndk_handle;

int advc_start_binder_thread_pool(const char *log_tag) {
    typedef void (*binder_set_max_threads_fn)(uint32_t);
    typedef void (*binder_start_pool_fn)(void);
    typedef bool (*binder_pool_started_fn)(void);
    binder_set_max_threads_fn set_max_threads;
    binder_start_pool_fn start_pool;
    binder_pool_started_fn pool_started;
    const char *tag = log_tag != NULL ? log_tag : "advc";

    if (binder_ndk_handle == NULL)
        binder_ndk_handle = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_LOCAL);
    if (binder_ndk_handle == NULL) {
        fprintf(stderr, "%s: Binder NDK unavailable: %s\n", tag, dlerror());
        return 0;
    }
    set_max_threads = (binder_set_max_threads_fn)dlsym(
        binder_ndk_handle, "ABinderProcess_setThreadPoolMaxThreadCount");
    start_pool = (binder_start_pool_fn)dlsym(
        binder_ndk_handle, "ABinderProcess_startThreadPool");
    pool_started = (binder_pool_started_fn)dlsym(
        binder_ndk_handle, "ABinderProcess_isThreadPoolStarted");
    if (set_max_threads == NULL || start_pool == NULL) {
        fprintf(stderr, "%s: Binder NDK thread-pool API unavailable\n", tag);
        return 0;
    }
    if (pool_started != NULL && pool_started()) return 1;
    set_max_threads(4);
    start_pool();
    fprintf(stderr, "%s: started Binder callback thread pool\n", tag);
    return 1;
}
