/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2018. ALL RIGHTS RESERVED.
 * Copyright (C) Advanced Micro Devices, Inc. 2019-2026. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "rocm_ipc_cache.h"

#include <uct/rocm/base/rocm_base_cache.h>
#include <ucs/debug/log.h>

#include <hsa_ext_amd.h>


/* --------------------------------------------------------------------------
 * IPC transport ops — thin wrappers around HSA IPC attach/detach
 * -------------------------------------------------------------------------- */

static ucs_status_t
uct_rocm_ipc_cache_map(const void *key_p, size_t length,
                       void **mapped_va_p, void **transport_ctx_p)
{
    const uct_rocm_ipc_key_t *key = key_p;
    hsa_status_t hsa_status;

    hsa_status = hsa_amd_ipc_memory_attach(&key->ipc, length, 0, NULL,
                                           mapped_va_p);
    if (ucs_unlikely(hsa_status != HSA_STATUS_SUCCESS)) {
        ucs_error("hsa_amd_ipc_memory_attach(%p, %zu) failed: %d",
                  (void *)key->address, length, hsa_status);
        return UCS_ERR_INVALID_ADDR;
    }

    *transport_ctx_p = NULL; /* IPC has no handle to retain */
    return UCS_OK;
}

static void
uct_rocm_ipc_cache_unmap(void *mapped_va, size_t length, void *transport_ctx)
{
    if (hsa_amd_ipc_memory_detach(mapped_va) != HSA_STATUS_SUCCESS) {
        ucs_warn("hsa_amd_ipc_memory_detach(%p) failed", mapped_va);
    }
}

static int
uct_rocm_ipc_cache_key_equal(const void *key_a, const void *key_b)
{
    return memcmp(&((const uct_rocm_ipc_key_t *)key_a)->ipc,
                  &((const uct_rocm_ipc_key_t *)key_b)->ipc,
                  sizeof(hsa_amd_ipc_memory_t)) == 0;
}

static const uct_rocm_base_cache_ops_t uct_rocm_ipc_cache_ops = {
    .map       = uct_rocm_ipc_cache_map,
    .unmap     = uct_rocm_ipc_cache_unmap,
    .key_equal = uct_rocm_ipc_cache_key_equal,
};

ucs_status_t
uct_rocm_ipc_cache_map_memhandle(uct_rocm_ipc_key_t *key, void **mapped_addr)
{
    uct_rocm_base_cache_peer_key_t peer = {key->pid, key->pid_ns, key->dev_num};

    return uct_rocm_base_cache_map_memhandle(key, sizeof(*key), peer,
                                             key->address, key->length,
                                             "rocm_ipc",
                                             &uct_rocm_ipc_cache_ops,
                                             mapped_addr);
}
