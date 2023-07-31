/*
 * Copyright (C) Advanced Micro Devices, Inc. 2019-2023. ALL RIGHTS RESERVED.
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2020. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "rocm_ipc_ep.h"
#include "rocm_ipc_iface.h"
#include "rocm_ipc_md.h"

#include <uct/rocm/base/rocm_base.h>
#include <uct/rocm/base/rocm_signal.h>
#include <uct/base/uct_iov.inl>
#include <ucs/profile/profile.h>

static UCS_CLASS_INIT_FUNC(uct_rocm_ipc_ep_t, const uct_ep_params_t *params)
{
    uct_rocm_ipc_iface_t *iface = ucs_derived_of(params->iface, uct_rocm_ipc_iface_t);
    char target_name[64];
    ucs_status_t status;

    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &iface->super);

    self->remote_pid = *(const pid_t*)params->iface_addr;

    snprintf(target_name, sizeof(target_name), "dest:%d", *(pid_t*)params->iface_addr);
    status = uct_rocm_ipc_create_cache(&self->remote_memh_cache, target_name);
    if (status != UCS_OK) {
        ucs_error("could not create create rocm ipc cache: %s",
                  ucs_status_string(status));
        return status;
    }

    return UCS_OK;
}

static UCS_CLASS_CLEANUP_FUNC(uct_rocm_ipc_ep_t)
{
    uct_rocm_ipc_destroy_cache(self->remote_memh_cache);
}

UCS_CLASS_DEFINE(uct_rocm_ipc_ep_t, uct_base_ep_t);
UCS_CLASS_DEFINE_NEW_FUNC(uct_rocm_ipc_ep_t, uct_ep_t, const uct_ep_params_t *);
UCS_CLASS_DEFINE_DELETE_FUNC(uct_rocm_ipc_ep_t, uct_ep_t);

#define uct_rocm_ipc_trace_data(_remote_addr, _rkey, _fmt, ...) \
    ucs_trace_data(_fmt " to %"PRIx64"(%+ld)", ## __VA_ARGS__, (_remote_addr), \
                   (_rkey))


#if HAVE_HSA_AMD_MEMORY_ASYNC_COPY_ON_ENGINE
static inline int uct_rocm_ipc_map_id_to_engine(int id)
{
    hsa_amd_sdma_engine_id_t engine = (HSA_AMD_SDMA_ENGINE_0 << id);

    return engine;
}

static inline int uct_rocm_ipc_get_agent_pos(hsa_agent_t agent)
{
    int num_gpu;
    hsa_agent_t *gpu_agents;
    int i;
    int pos=0;

    num_gpu = uct_rocm_base_get_gpu_agents(&gpu_agents);
    for (i=0; i<num_gpu; i++) {
        if (gpu_agents[i].handle == agent.handle) {
            pos = i;
            break;
        }
    }

    return pos;
}

static int uct_rocm_ipc_get_engines (hsa_agent_t dst_agent, hsa_agent_t src_agent, int **engines,
                                     int desired_num_engines)
{
    int num_engines = 0;
    int *temp;
    int i;
    uint32_t mask;
    int pos=0;
    hsa_status_t status;

    status = hsa_amd_memory_copy_engine_status(dst_agent, src_agent,
                                               &mask);
    if ((status != HSA_STATUS_SUCCESS) &&
        (status != HSA_STATUS_ERROR_OUT_OF_RESOURCES)) {
        ucs_trace("error in hsa_amd_memory_copy_engine_status status %d", status);
        return HSA_STATUS_ERROR;
    }

    for (i=0; i<(sizeof(uint32_t)*8); i++) {
        if (mask & UCS_BIT(i)) {
            num_engines++;
	}
    }

    if (num_engines >= desired_num_engines) {
        temp = (int*) malloc (num_engines * sizeof(int));
        if (temp == NULL) {
            return HSA_STATUS_ERROR;
        }

        for (i=0; i<(sizeof(uint32_t)*8); i++) {
            if (mask & UCS_BIT(i)) {
	        temp[pos++] = i;
	    }
	}
    }
    else {
        /* hard coded values for MI200 series */
        num_engines = 3;
        temp = (int*) malloc (num_engines * sizeof(int));
        if (temp == NULL) {
            return HSA_STATUS_ERROR;
        }

        for (i=0; i<num_engines; i++) {
            temp[i] = 2+i;
        }
    }

    *engines = temp;
    return num_engines;
}

static void uct_rocm_ipc_select_engines(int total_engines, int *engines, int multiplier,
                                        hsa_agent_t dst_agent, hsa_agent_t src_agent)
{
    int agent_pos;
    int *dtemp;
    int i;

    /* perturbe engine array depending on dst_agent. We use the
    ** position of dst_agent in the gpu_agents array as the seed,
    ** since the order of agents should be identical on all processes
    ** within a node.
    */
    agent_pos = uct_rocm_ipc_get_agent_pos(dst_agent);
    agent_pos *= multiplier;
    dtemp = (int*) malloc (total_engines * sizeof(int));
    if (dtemp == NULL) {
        return;
    }
    memcpy(dtemp, engines, total_engines * sizeof(int));
    for (i=0; i<total_engines; i++) {
        engines[i] = dtemp[(i+agent_pos)%total_engines];
    }
    free (dtemp);

    return;
}
#endif

/* TODO:
** not sure we can keep the addr pointers as char* (instead of void*). But
** simplifies pointer arithmetic for now
*/
static hsa_status_t uct_rocm_ipc_async_multi_copy(uct_rocm_ipc_iface_t *iface, char* dst_addr,
                                                  hsa_agent_t dst_agent, char* src_addr,
                                                  hsa_agent_t src_agent, size_t size,
                                                  void *remote_base_addr, uct_completion_t *comp)
{
#if HAVE_HSA_AMD_MEMORY_ASYNC_COPY_ON_ENGINE
    uct_rocm_base_signal_desc_t **rocm_ipc_signals;
    hsa_status_t status;
    hsa_amd_sdma_engine_id_t engine_id;
    int num_engines, total_engines;
    int *engines=NULL;
    int i;
    size_t chunk_size;
    size_t data_size;

    total_engines = uct_rocm_ipc_get_engines (dst_agent, src_agent, &engines, iface->config.max_sdma_engines);
    num_engines   = total_engines;
    if ((num_engines <= 0) || (engines == NULL)) {
        ucs_error("Couldn't find any engines to execute data transfer");
        return HSA_STATUS_ERROR;
    }
    if (num_engines > iface->config.max_sdma_engines) {
        num_engines = iface->config.max_sdma_engines;
    }

    if ( (int)(size/iface->config.multi_sdma_thresh) < num_engines){
        num_engines = (int)(size/iface->config.multi_sdma_thresh);
    }
    if (num_engines < 1) {
        num_engines = 1;
    }

    uct_rocm_ipc_select_engines (total_engines, engines, num_engines,
                                 dst_agent, src_agent);
    rocm_ipc_signals = (uct_rocm_base_signal_desc_t**) malloc (num_engines *
                                                               sizeof(uct_rocm_base_signal_desc_t*));
    if (NULL == rocm_ipc_signals) {
        return HSA_STATUS_ERROR;
    }

    chunk_size = size / num_engines;
    chunk_size = ucs_align_up_pow2(chunk_size, ucs_get_page_size());

    ucs_trace("Using %d engines, chunk_size %lu first engine %d", num_engines,
              chunk_size, engines[0]);
    for (i=0; i<num_engines; i++) {
        data_size = (i == (num_engines - 1)) ? (size - (i*chunk_size)) : chunk_size;

        rocm_ipc_signals[i] = ucs_mpool_get(&iface->signal_pool);
        hsa_signal_store_screlease(rocm_ipc_signals[i]->signal, 1);

        engine_id = uct_rocm_ipc_map_id_to_engine(engines[i]);
        status = UCS_PROFILE_CALL_ALWAYS(hsa_amd_memory_async_copy_on_engine,
                                         dst_addr+(i*chunk_size), dst_agent,
                                         src_addr+(i*chunk_size), src_agent,
                                         data_size, 0,
                                         NULL, rocm_ipc_signals[i]->signal,
                                         engine_id, false);
        if (status != HSA_STATUS_SUCCESS) {
            ucs_mpool_put(rocm_ipc_signals[i]);
            return status;
        }

        /* TODO:
        ** extend rocm_ipc_signal_desc structure to keep track of all
        ** parts belonging to the same data transfer, and invoke the
        ** comp when all are done.
        */
        if (i==(num_engines -1)) {
            rocm_ipc_signals[i]->comp = comp;
        } else {
            rocm_ipc_signals[i]->comp = NULL;
        }

        rocm_ipc_signals[i]->mapped_addr = remote_base_addr;
        ucs_queue_push(&iface->signal_queue, &rocm_ipc_signals[i]->queue);
    }

    free (rocm_ipc_signals);
    free (engines);
#endif

    return HSA_STATUS_SUCCESS;
}

static hsa_status_t uct_rocm_ipc_async_copy(uct_rocm_ipc_iface_t *iface, void* dst_addr,
                                            hsa_agent_t dst_agent, void* src_addr,
                                            hsa_agent_t src_agent, size_t size,
                                            void *remote_base_addr, uct_completion_t *comp)
{
    uct_rocm_base_signal_desc_t *rocm_ipc_signal;
    hsa_status_t status;

    rocm_ipc_signal = ucs_mpool_get(&iface->signal_pool);
    hsa_signal_store_screlease(rocm_ipc_signal->signal, 1);

    status = UCS_PROFILE_CALL_ALWAYS(hsa_amd_memory_async_copy, dst_addr,
                                     dst_agent, src_addr, src_agent, size, 0,
                                     NULL, rocm_ipc_signal->signal);
    if (status != HSA_STATUS_SUCCESS) {
        ucs_mpool_put(rocm_ipc_signal);
        return status;
    }
    rocm_ipc_signal->comp = comp;
    rocm_ipc_signal->mapped_addr = remote_base_addr;
    ucs_queue_push(&iface->signal_queue, &rocm_ipc_signal->queue);

    return HSA_STATUS_SUCCESS;
}

ucs_status_t uct_rocm_ipc_ep_zcopy(uct_ep_h tl_ep,
                                   uint64_t remote_addr,
                                   const uct_iov_t *iov,
                                   uct_rocm_ipc_key_t *key,
                                   uct_completion_t *comp,
                                   int is_put)
{
    uct_rocm_ipc_ep_t *ep = ucs_derived_of(tl_ep, uct_rocm_ipc_ep_t);
    hsa_status_t status;
    hsa_agent_t local_agent, remote_agent;
    hsa_agent_t dst_agent, src_agent;
    size_t size = uct_iov_get_length(iov);
    ucs_status_t ret = UCS_OK;
    void *base_addr, *local_addr = iov->buffer;
    uct_rocm_ipc_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_rocm_ipc_iface_t);
    void *remote_base_addr, *remote_copy_addr;
    void *dst_addr, *src_addr;
    void *tmp_base_ptr;
    size_t tmp_base_size;
    hsa_agent_t *gpu_agents;
    hsa_amd_pointer_type_t mem_type;
    int num_gpu;

    /* no data to deliver */
    if (!size)
        return UCS_OK;

    if ((remote_addr < key->address) ||
        (remote_addr + size > key->address + key->length)) {
        ucs_error("remote addr %lx/%lx out of range %lx/%lx",
                  remote_addr, size, key->address, key->length);
        return UCS_ERR_INVALID_PARAM;
    }

    status = uct_rocm_base_get_ptr_info(local_addr, size, &base_addr, NULL,
                                        &mem_type, &local_agent, NULL);
    if ((status != HSA_STATUS_SUCCESS) ||
        (mem_type == HSA_EXT_POINTER_TYPE_UNKNOWN)) {
        ucs_error("local addr %p/%lx is not ROCM memory", local_addr, size);
        return UCS_ERR_INVALID_ADDR;
    }

    ret = uct_rocm_ipc_cache_map_memhandle((void *)ep->remote_memh_cache, key,
                                           &remote_base_addr);
    if (ret != UCS_OK) {
        ucs_error("fail to attach ipc mem %p %d\n", (void *)key->address, ret);
        return ret;
    }

    remote_copy_addr = UCS_PTR_BYTE_OFFSET(remote_base_addr,
                                           remote_addr - key->address);

    memset(&remote_agent, 0, sizeof(hsa_agent_t));
    status = uct_rocm_base_get_ptr_info(remote_copy_addr, size, &tmp_base_ptr,
                                        &tmp_base_size, &mem_type,
                                        &remote_agent, NULL);
    if ((status != HSA_STATUS_SUCCESS) ||
        (mem_type == HSA_EXT_POINTER_TYPE_UNKNOWN)) {
        ucs_error("remote addr %p %lu is not ROCM memory status=%d mem_type %d",
                  remote_copy_addr, size, status, mem_type);
        return UCS_ERR_INVALID_ADDR;
    }

    if (remote_agent.handle == 0) {
        /* No access to remote agent, e.g. because of limited visability of devices to
         * this process. Using local_agent as a backup plan. */
        remote_agent = local_agent;
    } else {
        num_gpu = uct_rocm_base_get_gpu_agents(&gpu_agents);
        status  = UCS_PROFILE_CALL_ALWAYS(hsa_amd_agents_allow_access, num_gpu,
                                          gpu_agents, NULL, base_addr);
        if (status != HSA_STATUS_SUCCESS) {
            ucs_error("failed to enable direct access for mem addr %p agent "
                      "%lu\n",
                      (void*)remote_addr, remote_agent.handle);
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

    if (iface->config.copy_on_engine && iface->config.enable_multi_sdma) {
        status = uct_rocm_ipc_async_multi_copy(iface, dst_addr, dst_agent, src_addr, src_agent, size,
                                               remote_base_addr, comp);
    } else {
        status = uct_rocm_ipc_async_copy(iface, dst_addr, dst_agent, src_addr, src_agent, size,
                                         remote_base_addr, comp);
    }

    if (status != HSA_STATUS_SUCCESS) {
        ucs_error("copy error");
        return UCS_ERR_IO_ERROR;
    }

    ucs_trace("rocm async copy issued: remote:%p, local:%p  len:%ld",
               (void *)remote_addr, local_addr, size);

    return UCS_INPROGRESS;
}

ucs_status_t uct_rocm_ipc_ep_put_zcopy(uct_ep_h tl_ep, const uct_iov_t *iov, size_t iovcnt,
                                       uint64_t remote_addr, uct_rkey_t rkey,
                                       uct_completion_t *comp)
{
    ucs_status_t ret;
    uct_rocm_ipc_key_t *key = (uct_rocm_ipc_key_t *)rkey;

    ret = UCS_PROFILE_CALL_ALWAYS(uct_rocm_ipc_ep_zcopy, tl_ep, remote_addr,
                                  iov, key, comp, 1);

    UCT_TL_EP_STAT_OP(ucs_derived_of(tl_ep, uct_base_ep_t), PUT, ZCOPY,
                      uct_iov_total_length(iov, iovcnt));
    uct_rocm_ipc_trace_data(remote_addr, rkey, "PUT_ZCOPY [length %zu]",
                            uct_iov_total_length(iov, iovcnt));

    return ret;
}

ucs_status_t uct_rocm_ipc_ep_get_zcopy(uct_ep_h tl_ep, const uct_iov_t *iov, size_t iovcnt,
                                       uint64_t remote_addr, uct_rkey_t rkey,
                                       uct_completion_t *comp)
{
    ucs_status_t ret;
    uct_rocm_ipc_key_t *key = (uct_rocm_ipc_key_t *)rkey;

    ret = UCS_PROFILE_CALL_ALWAYS(uct_rocm_ipc_ep_zcopy, tl_ep, remote_addr,
                                  iov, key, comp, 0);

    UCT_TL_EP_STAT_OP(ucs_derived_of(tl_ep, uct_base_ep_t), GET, ZCOPY,
                      uct_iov_total_length(iov, iovcnt));
    uct_rocm_ipc_trace_data(remote_addr, rkey, "GET_ZCOPY [length %zu]",
                            uct_iov_total_length(iov, iovcnt));

    return ret;
}
