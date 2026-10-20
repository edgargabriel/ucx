/*
 * Copyright (C) Advanced Micro Devices, Inc. 2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifndef ROCM_FABRIC_IFACE_H
#define ROCM_FABRIC_IFACE_H

#include <uct/base/uct_iface.h>
#include <hsa.h>
#include <stdint.h>

#define UCT_ROCM_FABRIC_TL_NAME "rocm_fabr"

/*
 * Iface address: pod membership used to determine reachability.
 * Two peers are reachable iff ppod_id and vpod_id match exactly.
 */
typedef struct uct_rocm_fabric_iface_addr {
    uint8_t  ppod_id[16];
    uint32_t vpod_id;
} uct_rocm_fabric_iface_addr_t;

typedef struct uct_rocm_fabric_iface_config_params {
    size_t   min_zcopy;
    double   latency;
    int      enable_handle_cache;
    unsigned sigpool_max_elems;
} uct_rocm_fabric_iface_config_params_t;

typedef struct uct_rocm_fabric_iface {
    uct_base_iface_t                      super;
    ucs_mpool_t                           signal_pool;
    ucs_queue_head_t                      signal_queue;
    uct_rocm_fabric_iface_addr_t          local_addr;
    uct_rocm_fabric_iface_config_params_t config;
} uct_rocm_fabric_iface_t;

typedef struct uct_rocm_fabric_iface_config {
    uct_iface_config_t                    super;
    uct_rocm_fabric_iface_config_params_t params;
} uct_rocm_fabric_iface_config_t;

#endif
