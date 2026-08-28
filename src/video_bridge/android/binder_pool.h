#ifndef ADVC_BINDER_POOL_H
#define ADVC_BINDER_POOL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Starts bounded NDK Binder callback dispatch when the runtime exports it. */
int advc_start_binder_thread_pool(const char *log_tag);

#ifdef __cplusplus
}
#endif

#endif
