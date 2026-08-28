/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * C-compatible declaration subset of AOSP IMapper.h, AIMAPPER_VERSION_5.
 * Field order and function signatures are retained through
 * getStandardMetadata; LinDeX does not call or declare later fields.
 */
#ifndef ADVC_STABLE_MAPPER_V5_ABI_H
#define ADVC_STABLE_MAPPER_V5_ABI_H

#include <stddef.h>
#include <stdint.h>
#include <stdalign.h>

struct advc_native_handle {
    int version;
    int num_fds;
    int num_ints;
    int data[];
};

typedef const struct advc_native_handle *advc_buffer_handle;
typedef int32_t advc_ai_mapper_error;

enum {
    ADVC_AIMAPPER_VERSION_5 = 5,
    ADVC_AIMAPPER_ERROR_NONE = 0,
};

struct advc_ai_mapper_metadata_type {
    const char *name;
    int64_t value;
};

struct advc_a_rect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

struct advc_ai_mapper_v5 {
    advc_ai_mapper_error (*import_buffer)(const struct advc_native_handle *handle,
                                          advc_buffer_handle *out_handle);
    advc_ai_mapper_error (*free_buffer)(advc_buffer_handle buffer);
    advc_ai_mapper_error (*get_transport_size)(advc_buffer_handle buffer,
                                               uint32_t *out_num_fds,
                                               uint32_t *out_num_ints);
    advc_ai_mapper_error (*lock)(advc_buffer_handle, uint64_t,
                                 struct advc_a_rect, int, void **);
    advc_ai_mapper_error (*unlock)(advc_buffer_handle, int *);
    advc_ai_mapper_error (*flush_locked_buffer)(advc_buffer_handle);
    advc_ai_mapper_error (*reread_locked_buffer)(advc_buffer_handle);
    int32_t (*get_metadata)(advc_buffer_handle,
                            struct advc_ai_mapper_metadata_type, void *, size_t);
    int32_t (*get_standard_metadata)(advc_buffer_handle, int64_t, void *, size_t);
};

struct advc_ai_mapper {
    _Alignas(max_align_t) uint32_t version;
    struct advc_ai_mapper_v5 v5;
};

typedef advc_ai_mapper_error (*advc_load_ai_mapper_fn)(
    struct advc_ai_mapper **out_mapper);
typedef const struct advc_native_handle *(*advc_ahb_get_native_handle_fn)(
    const void *hardware_buffer);
typedef void *(*advc_open_passthrough_hal_fn)(const char *interface_name,
                                             const char *instance, int flags);

#endif
