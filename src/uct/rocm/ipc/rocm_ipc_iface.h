/*
 * Copyright (C) Advanced Micro Devices, Inc. 2019-2023. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */


#ifndef ROCM_IPC_IFACE_H
#define ROCM_IPC_IFACE_H

#include <uct/base/uct_iface.h>

#include <hsa.h>

#define UCT_ROCM_IPC_TL_NAME "rocm_ipc"

typedef struct uct_rocm_ipc_iface {
    uct_base_iface_t super;
    ucs_mpool_t signal_pool;
    ucs_queue_head_t signal_queue;
    struct {
        ucs_ternary_auto_value_t enable_multi_sdma;
        size_t                   multi_sdma_thresh;
        unsigned                 max_sdma_engines;
        unsigned                 copy_on_engine;
    } config;
} uct_rocm_ipc_iface_t;

typedef struct uct_rocm_ipc_iface_config {
    uct_iface_config_t       super;
    ucs_ternary_auto_value_t enable_multi_sdma;
    size_t                   multi_sdma_thresh;
    unsigned                 max_sdma_engines;
} uct_rocm_ipc_iface_config_t;

#endif
