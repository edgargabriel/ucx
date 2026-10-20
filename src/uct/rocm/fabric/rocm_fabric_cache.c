/*
 * Copyright (C) Advanced Micro Devices, Inc. 2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "rocm_fabric_cache.h"

#include <uct/rocm/base/rocm_base_cache.h>
#include <uct/rocm/base/rocm_base.h>
#include <uct/rocm/base/rocm_signal.h>
#include <ucs/debug/log.h>

#include <hsa_ext_amd.h>


/* --------------------------------------------------------------------------
 * Fabric transport ops — VMM import/reserve/map/set_access
 * -------------------------------------------------------------------------- */

static ucs_status_t
uct_rocm_fabric_cache_map(const void *key_p, size_t length,
                          void **mapped_va_p, void **transport_ctx_p)
{
    const uct_rocm_fabric_key_t *key = key_p;
    hsa_amd_vmem_alloc_handle_t *vmem_handle;
    hsa_amd_memory_access_desc_t *descs;
    hsa_agent_t *gpu_agents;
    hsa_status_t hsa_status;
    void *va;
    int num_gpu;
    int i;

    vmem_handle = ucs_malloc(sizeof(*vmem_handle), "rocm_fabric_vmem_handle");
    if (vmem_handle == NULL) {
        return UCS_ERR_NO_MEMORY;
    }

    hsa_status = hsa_amd_vmem_import_fabric_handle(key->fabric, vmem_handle);
    if (ucs_unlikely(hsa_status != HSA_STATUS_SUCCESS)) {
        ucs_error("hsa_amd_vmem_import_fabric_handle(%p) failed: %d",
                  (void *)key->address, hsa_status);
        goto err_free_handle;
    }

    hsa_status = hsa_amd_vmem_address_reserve(&va, length, 0, 0);
    if (ucs_unlikely(hsa_status != HSA_STATUS_SUCCESS)) {
        ucs_error("hsa_amd_vmem_address_reserve(%zu) failed: %d",
                  length, hsa_status);
        goto err_release_handle;
    }

    hsa_status = hsa_amd_vmem_map(va, length, 0, *vmem_handle, 0);
    if (ucs_unlikely(hsa_status != HSA_STATUS_SUCCESS)) {
        ucs_error("hsa_amd_vmem_map(%p, %zu) failed: %d", va, length,
                  hsa_status);
        goto err_free_va;
    }

    num_gpu = uct_rocm_base_get_gpu_agents(&gpu_agents);
    descs   = ucs_alloca(num_gpu * sizeof(*descs));
    for (i = 0; i < num_gpu; i++) {
        descs[i].permissions  = HSA_ACCESS_PERMISSION_RW;
        descs[i].agent_handle = gpu_agents[i];
    }

    hsa_status = hsa_amd_vmem_set_access(va, length, descs, num_gpu);
    if (ucs_unlikely(hsa_status != HSA_STATUS_SUCCESS)) {
        ucs_error("hsa_amd_vmem_set_access(%p, %zu) failed: %d", va, length,
                  hsa_status);
        goto err_unmap;
    }

    *mapped_va_p      = va;
    *transport_ctx_p  = vmem_handle;
    return UCS_OK;

err_unmap:
    hsa_amd_vmem_unmap(va, length);
err_free_va:
    hsa_amd_vmem_address_free(va, length);
err_release_handle:
    hsa_amd_vmem_handle_release(*vmem_handle);
err_free_handle:
    ucs_free(vmem_handle);
    return UCS_ERR_INVALID_ADDR;
}

static void
uct_rocm_fabric_cache_unmap(void *mapped_va, size_t length, void *transport_ctx)
{
    hsa_amd_vmem_alloc_handle_t *vmem_handle = transport_ctx;
    hsa_status_t status;

    status = hsa_amd_vmem_unmap(mapped_va, length);
    if (status != HSA_STATUS_SUCCESS) {
        ucs_warn("hsa_amd_vmem_unmap(%p, %zu) failed: %d",
                 mapped_va, length, status);
    }
    status = hsa_amd_vmem_address_free(mapped_va, length);
    if (status != HSA_STATUS_SUCCESS) {
        ucs_warn("hsa_amd_vmem_address_free(%p) failed: %d", mapped_va, status);
    }
    status = hsa_amd_vmem_handle_release(*vmem_handle);
    if (status != HSA_STATUS_SUCCESS) {
        ucs_warn("hsa_amd_vmem_handle_release failed: %d", status);
    }
    ucs_free(vmem_handle);
}

static int
uct_rocm_fabric_cache_key_equal(const void *key_a, const void *key_b)
{
    return memcmp(&((const uct_rocm_fabric_key_t *)key_a)->fabric,
                  &((const uct_rocm_fabric_key_t *)key_b)->fabric,
                  sizeof(hsa_fabric_handle_t)) == 0;
}

static const uct_rocm_base_cache_ops_t uct_rocm_fabric_cache_ops = {
    .map       = uct_rocm_fabric_cache_map,
    .unmap     = uct_rocm_fabric_cache_unmap,
    .key_equal = uct_rocm_fabric_cache_key_equal,
};

ucs_status_t
uct_rocm_fabric_cache_map_memhandle(const uct_rocm_fabric_key_t *key,
                                    void **mapped_va_p,
                                    size_t *mapped_size_out)
{
    uct_rocm_base_cache_peer_key_t peer = {key->pid, key->pid_ns, key->dev_num};
    ucs_status_t status;

    status = uct_rocm_base_cache_map_memhandle(key, sizeof(*key), peer,
                                               key->address, key->length,
                                               "rocm_fabr",
                                               &uct_rocm_fabric_cache_ops,
                                               mapped_va_p);
    if (status == UCS_OK) {
        *mapped_size_out = key->length;
    }
    return status;
}

void uct_rocm_fabric_vmem_unmap(uct_rocm_base_signal_desc_t *desc)
{
    hsa_status_t status;

    status = hsa_amd_vmem_unmap(desc->mapped_addr, desc->mapped_size);
    if (status != HSA_STATUS_SUCCESS) {
        ucs_warn("hsa_amd_vmem_unmap(%p, %zu) failed: %d",
                 desc->mapped_addr, desc->mapped_size, status);
    }
    status = hsa_amd_vmem_address_free(desc->mapped_addr, desc->mapped_size);
    if (status != HSA_STATUS_SUCCESS) {
        ucs_warn("hsa_amd_vmem_address_free(%p) failed: %d",
                 desc->mapped_addr, status);
    }
}
