/*
 * Copyright (C) Advanced Micro Devices, Inc. 2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "rocm_fabric_ep.h"
#include "rocm_fabric_iface.h"
#include "rocm_fabric_cache.h"

#include <uct/rocm/base/rocm_base.h>
#include <uct/rocm/base/rocm_signal.h>
#include <uct/base/uct_iov.inl>
#include <ucs/profile/profile.h>


static UCS_CLASS_INIT_FUNC(uct_rocm_fabric_ep_t, const uct_ep_params_t *params)
{
    const uct_rocm_fabric_iface_addr_t *iface_addr =
            (const uct_rocm_fabric_iface_addr_t *)params->iface_addr;
    uct_rocm_fabric_iface_t *iface =
            ucs_derived_of(params->iface, uct_rocm_fabric_iface_t);

    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &iface->super);

    /* iface_addr layout: [ppod_id 16B][vpod_id 4B][pid 4B] —
     * the iface address struct is followed by pid in the address buffer.
     * For rocm_fabric we store remote_pid from the iface_addr parameter
     * which is actually a uct_rocm_fabric_iface_addr_t; the pid is
     * available separately via params->iface_addr cast to pid_t for the
     * connect-to-iface flow. Use the vpod_id field as a canary; the
     * actual remote PID comes from key->pid at copy time.
     * Store 0 here — actual remote PID is taken from the rkey at zcopy time.
     */
    (void)iface_addr;
    self->remote_pid = 0;
    return UCS_OK;
}

static UCS_CLASS_CLEANUP_FUNC(uct_rocm_fabric_ep_t)
{
}

UCS_CLASS_DEFINE(uct_rocm_fabric_ep_t, uct_base_ep_t);
UCS_CLASS_DEFINE_NEW_FUNC(uct_rocm_fabric_ep_t, uct_ep_t,
                          const uct_ep_params_t *);
UCS_CLASS_DEFINE_DELETE_FUNC(uct_rocm_fabric_ep_t, uct_ep_t);

#define uct_rocm_fabric_trace_data(_raddr, _rkey, _fmt, ...) \
    ucs_trace_data(_fmt " to %"PRIx64"(%+ld)", ##__VA_ARGS__, \
                   (_raddr), (long)(_rkey))

static ucs_status_t
uct_rocm_fabric_ep_zcopy(uct_ep_h tl_ep, uint64_t remote_addr,
                         const uct_iov_t *iov,
                         const uct_rocm_fabric_key_t *key,
                         uct_completion_t *comp, int is_put)
{
    uct_rocm_fabric_iface_t *iface =
            ucs_derived_of(tl_ep->iface, uct_rocm_fabric_iface_t);
    size_t size        = uct_iov_get_length(iov);
    void *local_addr   = iov->buffer;
    void *remote_va    = NULL;
    size_t remote_size = 0;
    void *remote_copy_addr;
    void *base_addr;
    hsa_agent_t local_agent, remote_agent;
    hsa_agent_t dst_agent, src_agent;
    void *dst_addr, *src_addr;
    void *tmp_base;
    size_t tmp_size;
    hsa_amd_pointer_type_t mem_type;
    hsa_agent_t *gpu_agents;
    hsa_amd_memory_access_desc_t *descs;
    hsa_status_t hsa_status;
    ucs_status_t status;
    int num_gpu;
    int i;

    if (!size) {
        return UCS_OK;
    }

    if ((remote_addr < key->address) ||
        (remote_addr + size > key->address + key->length)) {
        ucs_error("remote addr 0x%lx/%zu out of key range 0x%lx/%zu",
                  remote_addr, size, key->address, key->length);
        return UCS_ERR_INVALID_PARAM;
    }

    hsa_status = uct_rocm_base_get_ptr_info(local_addr, size, &base_addr, NULL,
                                            &mem_type, &local_agent, NULL);
    if ((hsa_status != HSA_STATUS_SUCCESS) ||
        (mem_type == HSA_EXT_POINTER_TYPE_UNKNOWN)) {
        ucs_error("local addr %p/%zu is not ROCm memory", local_addr, size);
        return UCS_ERR_INVALID_ADDR;
    }

    if (iface->config.enable_handle_cache) {
        status = uct_rocm_fabric_cache_map_memhandle(key, &remote_va,
                                                     &remote_size);
        if (ucs_unlikely(status != UCS_OK)) {
            ucs_error("failed to map fabric handle for %p: %s",
                      (void *)key->address, ucs_status_string(status));
            return status;
        }
    } else {
        hsa_amd_vmem_alloc_handle_t vmem_handle;

        hsa_status = hsa_amd_vmem_import_fabric_handle(key->fabric,
                                                        &vmem_handle);
        if (ucs_unlikely(hsa_status != HSA_STATUS_SUCCESS)) {
            ucs_error("hsa_amd_vmem_import_fabric_handle failed: %d",
                      hsa_status);
            return UCS_ERR_INVALID_ADDR;
        }

        hsa_status = hsa_amd_vmem_address_reserve(&remote_va, key->length,
                                                   0, 0);
        if (ucs_unlikely(hsa_status != HSA_STATUS_SUCCESS)) {
            ucs_error("hsa_amd_vmem_address_reserve(%zu) failed: %d",
                      key->length, hsa_status);
            hsa_amd_vmem_handle_release(vmem_handle);
            return UCS_ERR_NO_MEMORY;
        }

        hsa_status = hsa_amd_vmem_map(remote_va, key->length, 0,
                                       vmem_handle, 0);
        if (ucs_unlikely(hsa_status != HSA_STATUS_SUCCESS)) {
            ucs_error("hsa_amd_vmem_map(%p, %zu) failed: %d",
                      remote_va, key->length, hsa_status);
            hsa_amd_vmem_address_free(remote_va, key->length);
            hsa_amd_vmem_handle_release(vmem_handle);
            return UCS_ERR_INVALID_ADDR;
        }

        num_gpu = uct_rocm_base_get_gpu_agents(&gpu_agents);
        descs   = ucs_alloca(num_gpu * sizeof(*descs));
        for (i = 0; i < num_gpu; i++) {
            descs[i].permissions  = HSA_ACCESS_PERMISSION_RW;
            descs[i].agent_handle = gpu_agents[i];
        }
        hsa_amd_vmem_set_access(remote_va, key->length, descs, num_gpu);
        hsa_amd_vmem_handle_release(vmem_handle);
        remote_size = key->length;
    }

    remote_copy_addr = UCS_PTR_BYTE_OFFSET(remote_va,
                                           remote_addr - key->address);

    memset(&remote_agent, 0, sizeof(remote_agent));
    hsa_status = uct_rocm_base_get_ptr_info(remote_copy_addr, size,
                                            &tmp_base, &tmp_size, &mem_type,
                                            &remote_agent, NULL);
    if ((hsa_status != HSA_STATUS_SUCCESS) ||
        (mem_type == HSA_EXT_POINTER_TYPE_UNKNOWN)) {
        ucs_error("remote copy addr %p/%zu is not ROCm memory: %d",
                  remote_copy_addr, size, hsa_status);
        if (!iface->config.enable_handle_cache) {
            hsa_amd_vmem_unmap(remote_va, remote_size);
            hsa_amd_vmem_address_free(remote_va, remote_size);
        }
        return UCS_ERR_INVALID_ADDR;
    }

    if (remote_agent.handle == 0) {
        remote_agent = local_agent;
    } else {
        num_gpu    = uct_rocm_base_get_gpu_agents(&gpu_agents);
        hsa_status = UCS_PROFILE_CALL_ALWAYS(hsa_amd_agents_allow_access,
                                             num_gpu, gpu_agents, NULL,
                                             base_addr);
        if (hsa_status != HSA_STATUS_SUCCESS) {
            ucs_error("hsa_amd_agents_allow_access(%p) failed: %d",
                      base_addr, hsa_status);
            return UCS_ERR_INVALID_ADDR;
        }
    }

    if (is_put) {
        dst_addr  = remote_copy_addr;
        dst_agent = remote_agent;
        src_addr  = local_addr;
        src_agent = local_agent;
    } else {
        dst_addr  = local_addr;
        dst_agent = local_agent;
        src_addr  = remote_copy_addr;
        src_agent = remote_agent;
    }

    /* Cache path: mapped_addr stays alive until async copy completes;
     * the signal progress will call uct_rocm_fabric_vmem_unmap.
     * Non-cache path: likewise pass remote_va so signal cleanup releases it. */
    status = uct_rocm_base_async_copy(
            dst_addr, dst_agent, src_addr, src_agent, size,
            iface->config.enable_handle_cache ? NULL : remote_va,
            remote_size,
            iface->config.enable_handle_cache ? NULL : uct_rocm_fabric_vmem_unmap,
            &iface->signal_pool, &iface->signal_queue, comp);

    ucs_trace("rocm_fabric async copy: dst=%p src=%p len=%zu", dst_addr,
              src_addr, size);
    return status;
}

ucs_status_t
uct_rocm_fabric_ep_put_zcopy(uct_ep_h tl_ep, const uct_iov_t *iov,
                             size_t iovcnt, uint64_t remote_addr,
                             uct_rkey_t rkey, uct_completion_t *comp)
{
    uct_rocm_fabric_key_t *key = (uct_rocm_fabric_key_t *)rkey;
    ucs_status_t ret;

    ret = UCS_PROFILE_CALL_ALWAYS(uct_rocm_fabric_ep_zcopy, tl_ep,
                                  remote_addr, iov, key, comp, 1);

    UCT_TL_EP_STAT_OP(ucs_derived_of(tl_ep, uct_base_ep_t), PUT, ZCOPY,
                      uct_iov_total_length(iov, iovcnt));
    uct_rocm_fabric_trace_data(remote_addr, rkey, "PUT_ZCOPY [length %zu]",
                               uct_iov_total_length(iov, iovcnt));
    return ret;
}

ucs_status_t
uct_rocm_fabric_ep_get_zcopy(uct_ep_h tl_ep, const uct_iov_t *iov,
                             size_t iovcnt, uint64_t remote_addr,
                             uct_rkey_t rkey, uct_completion_t *comp)
{
    uct_rocm_fabric_key_t *key = (uct_rocm_fabric_key_t *)rkey;
    ucs_status_t ret;

    ret = UCS_PROFILE_CALL_ALWAYS(uct_rocm_fabric_ep_zcopy, tl_ep,
                                  remote_addr, iov, key, comp, 0);

    UCT_TL_EP_STAT_OP(ucs_derived_of(tl_ep, uct_base_ep_t), GET, ZCOPY,
                      uct_iov_total_length(iov, iovcnt));
    uct_rocm_fabric_trace_data(remote_addr, rkey, "GET_ZCOPY [length %zu]",
                               uct_iov_total_length(iov, iovcnt));
    return ret;
}
