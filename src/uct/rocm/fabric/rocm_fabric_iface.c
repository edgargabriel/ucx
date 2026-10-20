/*
 * Copyright (C) Advanced Micro Devices, Inc. 2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "rocm_fabric_iface.h"
#include "rocm_fabric_md.h"
#include "rocm_fabric_ep.h"

#include <uct/rocm/base/rocm_base.h>
#include <uct/rocm/base/rocm_signal.h>

#ifdef HAVE_AMDSMI_GPU_FABRIC_INFO
#include <amd_smi/amdsmi.h>
#endif

#include <string.h>
#include <unistd.h>


static ucs_config_field_t uct_rocm_fabric_iface_config_table[] = {
    {"", "", NULL,
     ucs_offsetof(uct_rocm_fabric_iface_config_t, super),
     UCS_CONFIG_TYPE_TABLE(uct_iface_config_table)},

    {"MIN_ZCOPY", "128",
     "Minimum data size for ROCm/Fabric zcopy protocols",
     ucs_offsetof(uct_rocm_fabric_iface_config_t, params.min_zcopy),
     UCS_CONFIG_TYPE_MEMUNITS},

    {"LAT", "1e-7", "Latency",
     ucs_offsetof(uct_rocm_fabric_iface_config_t, params.latency),
     UCS_CONFIG_TYPE_TIME},

    {"CACHE_FABRIC_HANDLES", "y",
     "Enable caching imported fabric handles",
     ucs_offsetof(uct_rocm_fabric_iface_config_t, params.enable_handle_cache),
     UCS_CONFIG_TYPE_BOOL},

    {"SIGPOOL_MAX_ELEMS", "1024",
     "Maximum number of elements in signal pool",
     ucs_offsetof(uct_rocm_fabric_iface_config_t, params.sigpool_max_elems),
     UCS_CONFIG_TYPE_UINT},

    {NULL}
};

static double uct_rocm_fabric_iface_get_bw(void)
{
    static double bw = -1.0;
    hsa_amd_link_info_type_t type;

    if (bw < 0.0) {
        uct_rocm_base_get_link_type(&type);
        switch (type) {
        case HSA_AMD_LINK_INFO_TYPE_XGMI:
            bw = 400.0 * UCS_GBYTE;
            break;
        case HSA_AMD_LINK_INFO_TYPE_PCIE:
            bw = 200.0 * UCS_GBYTE;
            break;
        default:
            bw = 100.0 * UCS_GBYTE;
            break;
        }
    }
    return bw;
}

static void
uct_rocm_fabric_iface_populate_local_addr(uct_rocm_fabric_iface_t *iface)
{
#ifdef HAVE_AMDSMI_GPU_FABRIC_INFO
    hsa_agent_t *gpu_agents;
    amdsmi_fabric_info_t fabric_info;
    amdsmi_processor_handle gpu_handle;
    amdsmi_status_t amdsmi_status;
    hsa_status_t hsa_status;
    amdsmi_bdf_t bdf;
    uint32_t bdfid;
    uint32_t domain;
    int num_gpu;
#endif

    memset(&iface->local_addr, 0, sizeof(iface->local_addr));

#ifdef HAVE_AMDSMI_GPU_FABRIC_INFO
    num_gpu = uct_rocm_base_get_gpu_agents(&gpu_agents);
    if (num_gpu == 0) {
        ucs_debug("rocm_fabric: no GPU agents found");
        return;
    }

    /* Derive PCI BDF from the first GPU agent using HSA info — no HIP. */
    hsa_status = hsa_agent_get_info(gpu_agents[0],
                                    (hsa_agent_info_t)HSA_AMD_AGENT_INFO_BDFID,
                                    &bdfid);
    if (hsa_status != HSA_STATUS_SUCCESS) {
        ucs_debug("rocm_fabric: HSA_AMD_AGENT_INFO_BDFID failed: %d", hsa_status);
        return;
    }

    hsa_status = hsa_agent_get_info(gpu_agents[0],
                                    (hsa_agent_info_t)HSA_AMD_AGENT_INFO_DOMAIN,
                                    &domain);
    if (hsa_status != HSA_STATUS_SUCCESS) {
        ucs_debug("rocm_fabric: HSA_AMD_AGENT_INFO_DOMAIN failed: %d", hsa_status);
        return;
    }

    bdf.bus_number      = (bdfid & (0xFF << 8)) >> 8;
    bdf.device_number   = (bdfid & (0x1F << 3)) >> 3;
    bdf.function_number = 0;
    bdf.domain_number   = domain;

    amdsmi_status = amdsmi_init(AMDSMI_INIT_AMD_GPUS);
    if (amdsmi_status != AMDSMI_STATUS_SUCCESS) {
        ucs_debug("rocm_fabric: amdsmi_init failed: %d", (int)amdsmi_status);
        return;
    }

    amdsmi_status = amdsmi_get_processor_handle_from_bdf(bdf, &gpu_handle);
    if (amdsmi_status != AMDSMI_STATUS_SUCCESS) {
        ucs_debug("rocm_fabric: amdsmi_get_processor_handle_from_bdf failed: %d",
                  (int)amdsmi_status);
        amdsmi_shut_down();
        return;
    }

    amdsmi_status = amdsmi_get_gpu_fabric_info(gpu_handle, &fabric_info);
    if (amdsmi_status != AMDSMI_STATUS_SUCCESS) {
        ucs_debug("rocm_fabric: amdsmi_get_gpu_fabric_info failed: %d",
                  (int)amdsmi_status);
        amdsmi_shut_down();
        return;
    }

    amdsmi_shut_down();

    memcpy(iface->local_addr.ppod_id, fabric_info.fabric_info.v1.ppod_id,
           sizeof(iface->local_addr.ppod_id));
    iface->local_addr.vpod_id = fabric_info.fabric_info.v1.vpod_id;

    ucs_debug("rocm_fabric: vpod_id=%u", iface->local_addr.vpod_id);
#endif
}

ucs_status_t
uct_rocm_fabric_iface_get_device_address(uct_iface_t *tl_iface,
                                         uct_device_addr_t *addr)
{
    *(uint64_t*)addr = ucs_get_system_id();
    return UCS_OK;
}

static ucs_status_t
uct_rocm_fabric_iface_get_address(uct_iface_h tl_iface,
                                  uct_iface_addr_t *iface_addr)
{
    uct_rocm_fabric_iface_t *iface = ucs_derived_of(tl_iface,
                                                     uct_rocm_fabric_iface_t);
    *(uct_rocm_fabric_iface_addr_t*)iface_addr = iface->local_addr;
    return UCS_OK;
}

static int
uct_rocm_fabric_iface_is_reachable_v2(
        const uct_iface_h tl_iface,
        const uct_iface_is_reachable_params_t *params)
{
    const uct_rocm_fabric_iface_t *iface;
    const uct_rocm_fabric_iface_addr_t *remote;
    static const uint8_t zero_ppod[16] = {};

    if (!uct_iface_is_reachable_params_addrs_valid(params)) {
        return 0;
    }

    /* Reject same process — use shared memory transport instead */
    if (getpid() == *(const pid_t*)params->iface_addr) {
        uct_iface_fill_info_str_buf(params, "same process");
        return 0;
    }

    iface  = ucs_derived_of(tl_iface, uct_rocm_fabric_iface_t);
    remote = (const uct_rocm_fabric_iface_addr_t*)params->iface_addr;

    /* If our own pod ID is all-zeros, AMD SMI was unavailable — not reachable */
    if (memcmp(iface->local_addr.ppod_id, zero_ppod, 16) == 0) {
        uct_iface_fill_info_str_buf(params, "fabric pod info unavailable");
        return 0;
    }

    if (memcmp(iface->local_addr.ppod_id, remote->ppod_id, 16) != 0 ||
        iface->local_addr.vpod_id != remote->vpod_id) {
        uct_iface_fill_info_str_buf(params, "different fabric pod");
        return 0;
    }

    return uct_iface_scope_is_reachable(tl_iface, params);
}

static ucs_status_t
uct_rocm_fabric_iface_query(uct_iface_h tl_iface, uct_iface_attr_t *iface_attr)
{
    uct_rocm_fabric_iface_t *iface = ucs_derived_of(tl_iface,
                                                     uct_rocm_fabric_iface_t);

    uct_base_iface_query(&iface->super, iface_attr);

    iface_attr->cap.put.min_zcopy       = iface->config.min_zcopy;
    iface_attr->cap.put.max_zcopy       = SIZE_MAX;
    iface_attr->cap.put.opt_zcopy_align = sizeof(uint32_t);
    iface_attr->cap.put.align_mtu       = iface_attr->cap.put.opt_zcopy_align;
    iface_attr->cap.put.max_iov         = 1;

    iface_attr->cap.get.min_zcopy       = iface->config.min_zcopy;
    iface_attr->cap.get.max_zcopy       = SIZE_MAX;
    iface_attr->cap.get.opt_zcopy_align = sizeof(uint32_t);
    iface_attr->cap.get.align_mtu       = iface_attr->cap.get.opt_zcopy_align;
    iface_attr->cap.get.max_iov         = 1;

    iface_attr->iface_addr_len  = sizeof(uct_rocm_fabric_iface_addr_t);
    iface_attr->device_addr_len = sizeof(uint64_t);
    iface_attr->ep_addr_len     = 0;
    iface_attr->max_conn_priv   = 0;
    iface_attr->cap.flags       = UCT_IFACE_FLAG_ERRHANDLE_PEER_FAILURE |
                                  UCT_IFACE_FLAG_GET_ZCOPY |
                                  UCT_IFACE_FLAG_PUT_ZCOPY |
                                  UCT_IFACE_FLAG_PENDING |
                                  UCT_IFACE_FLAG_CONNECT_TO_IFACE;
    iface_attr->ctl_device      = UCS_SYS_DEVICE_ID_UNKNOWN;
    iface_attr->latency         = ucs_linear_func_make(iface->config.latency, 0);
    iface_attr->bandwidth.dedicated = 0;
    iface_attr->bandwidth.shared    = uct_rocm_fabric_iface_get_bw();
    iface_attr->overhead            = 0;
    iface_attr->priority            = 0;

    return UCS_OK;
}

static UCS_CLASS_DECLARE_DELETE_FUNC(uct_rocm_fabric_iface_t, uct_iface_t);

static ucs_status_t
uct_rocm_fabric_iface_flush(uct_iface_h tl_iface, unsigned flags,
                            uct_completion_t *comp)
{
    uct_rocm_fabric_iface_t *iface = ucs_derived_of(tl_iface,
                                                     uct_rocm_fabric_iface_t);

    if (comp != NULL) {
        return UCS_ERR_UNSUPPORTED;
    }

    if (ucs_queue_is_empty(&iface->signal_queue)) {
        UCT_TL_IFACE_STAT_FLUSH(ucs_derived_of(tl_iface, uct_base_iface_t));
        return UCS_OK;
    }

    UCT_TL_IFACE_STAT_FLUSH_WAIT(ucs_derived_of(tl_iface, uct_base_iface_t));
    return UCS_INPROGRESS;
}

static ucs_status_t
uct_rocm_fabric_ep_flush(uct_ep_h tl_ep, unsigned flags,
                         uct_completion_t *comp)
{
    uct_rocm_fabric_iface_t *iface = ucs_derived_of(tl_ep->iface,
                                                     uct_rocm_fabric_iface_t);
    return uct_rocm_base_ep_flush(tl_ep, &iface->signal_pool,
                                  &iface->signal_queue, comp);
}

static unsigned uct_rocm_fabric_iface_progress(uct_iface_h tl_iface)
{
    uct_rocm_fabric_iface_t *iface = ucs_derived_of(tl_iface,
                                                     uct_rocm_fabric_iface_t);
    return uct_rocm_base_progress(&iface->signal_queue);
}

static uct_iface_internal_ops_t uct_rocm_fabric_iface_internal_ops = {
    .iface_query_v2         = uct_iface_base_query_v2,
    .iface_estimate_perf    = uct_base_iface_estimate_perf,
    .iface_vfs_refresh      = (uct_iface_vfs_refresh_func_t)ucs_empty_function,
    .ep_query               = (uct_ep_query_func_t)
            ucs_empty_function_return_unsupported,
    .ep_invalidate          = (uct_ep_invalidate_func_t)
            ucs_empty_function_return_unsupported,
    .ep_connect_to_ep_v2    = (uct_ep_connect_to_ep_v2_func_t)
            ucs_empty_function_return_unsupported,
    .iface_is_reachable_v2  = uct_rocm_fabric_iface_is_reachable_v2,
    .ep_is_connected        = uct_base_ep_is_connected,
    .ep_get_device_ep       = (uct_ep_get_device_ep_func_t)
            ucs_empty_function_return_unsupported,
    .ep_outstanding_purge   = (uct_ep_outstanding_purge_func_t)
            ucs_empty_function_return_unsupported
};

static uct_iface_ops_t uct_rocm_fabric_iface_ops = {
    .ep_put_zcopy             = uct_rocm_fabric_ep_put_zcopy,
    .ep_get_zcopy             = uct_rocm_fabric_ep_get_zcopy,
    .ep_pending_add           = (uct_ep_pending_add_func_t)
            ucs_empty_function_return_busy,
    .ep_pending_purge         = (uct_ep_pending_purge_func_t)ucs_empty_function,
    .ep_flush                 = uct_rocm_fabric_ep_flush,
    .ep_fence                 = uct_base_ep_fence,
    .ep_create                = UCS_CLASS_NEW_FUNC_NAME(uct_rocm_fabric_ep_t),
    .ep_destroy               = UCS_CLASS_DELETE_FUNC_NAME(uct_rocm_fabric_ep_t),
    .ep_check                 = (uct_ep_check_func_t)
            ucs_empty_function_return_unsupported,
    .iface_flush              = uct_rocm_fabric_iface_flush,
    .iface_fence              = uct_base_iface_fence,
    .iface_progress_enable    = uct_base_iface_progress_enable,
    .iface_progress_disable   = uct_base_iface_progress_disable,
    .iface_progress           = uct_rocm_fabric_iface_progress,
    .iface_close              = UCS_CLASS_DELETE_FUNC_NAME(uct_rocm_fabric_iface_t),
    .iface_query              = uct_rocm_fabric_iface_query,
    .iface_get_address        = uct_rocm_fabric_iface_get_address,
    .iface_get_device_address = uct_rocm_fabric_iface_get_device_address,
    .iface_is_reachable       = uct_base_iface_is_reachable
};

static UCS_CLASS_INIT_FUNC(uct_rocm_fabric_iface_t, uct_md_h md,
                           uct_worker_h worker,
                           const uct_iface_params_t *params,
                           const uct_iface_config_t *tl_config)
{
    uct_rocm_fabric_iface_config_t *config =
            ucs_derived_of(tl_config, uct_rocm_fabric_iface_config_t);
    ucs_mpool_params_t mp_params;
    ucs_status_t status;

    UCS_CLASS_CALL_SUPER_INIT(uct_base_iface_t,
                              &uct_rocm_fabric_iface_ops,
                              &uct_rocm_fabric_iface_internal_ops,
                              md, worker, params,
                              tl_config
                              UCS_STATS_ARG(params->stats_root)
                              UCS_STATS_ARG(UCT_ROCM_FABRIC_TL_NAME));

    self->config = config->params;

    ucs_mpool_params_reset(&mp_params);
    mp_params.elem_size       = sizeof(uct_rocm_base_signal_desc_t);
    mp_params.elems_per_chunk = 128;
    mp_params.max_elems       = config->params.sigpool_max_elems;
    mp_params.ops             = &uct_rocm_base_signal_desc_mpool_ops;
    mp_params.name            = "ROCM_FABRIC signal objects";
    status = ucs_mpool_init(&mp_params, &self->signal_pool);
    if (status != UCS_OK) {
        ucs_error("rocm/fabric signal mpool creation failed");
        return status;
    }

    ucs_queue_head_init(&self->signal_queue);
    uct_rocm_fabric_iface_populate_local_addr(self);

    return UCS_OK;
}

static UCS_CLASS_CLEANUP_FUNC(uct_rocm_fabric_iface_t)
{
    uct_base_iface_progress_disable(&self->super.super,
                                    UCT_PROGRESS_SEND | UCT_PROGRESS_RECV);
    ucs_mpool_cleanup(&self->signal_pool, 1);
}

UCS_CLASS_DEFINE(uct_rocm_fabric_iface_t, uct_base_iface_t);

static UCS_CLASS_DEFINE_NEW_FUNC(uct_rocm_fabric_iface_t, uct_iface_t,
                                 uct_md_h, uct_worker_h,
                                 const uct_iface_params_t *,
                                 const uct_iface_config_t *);
static UCS_CLASS_DEFINE_DELETE_FUNC(uct_rocm_fabric_iface_t, uct_iface_t);

UCT_TL_DEFINE(&uct_rocm_fabric_component.super, rocm_fabr,
              uct_rocm_base_query_devices, uct_rocm_fabric_iface_t,
              "ROCM_FABRIC_", uct_rocm_fabric_iface_config_table,
              uct_rocm_fabric_iface_config_t);
