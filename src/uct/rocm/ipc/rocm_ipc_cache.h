/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2018. ALL RIGHTS RESERVED.
 * Copyright (C) Advanced Micro Devices, Inc. 2019-2026. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_ROCM_IPC_CACHE_H_
#define UCT_ROCM_IPC_CACHE_H_

#include <uct/rocm/base/rocm_base_cache.h>
#include "rocm_ipc_md.h"


ucs_status_t
uct_rocm_ipc_cache_map_memhandle(uct_rocm_ipc_key_t *key, void **mapped_addr);

#endif
