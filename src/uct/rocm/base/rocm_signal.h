/*
 * Copyright (C) Advanced Micro Devices, Inc. 2023-2026. ALL RIGHTS RESERVED.
 */

#ifndef ROCM_SIGNAL_H
#define ROCM_SIGNAL_H

#include <ucs/arch/cpu.h>
#include <ucs/type/class.h>
#include <ucs/sys/string.h>
#include <ucs/datastruct/mpool.h>
#include <ucs/datastruct/queue.h>
#include <uct/api/uct.h>
#include <hsa.h>


typedef struct uct_rocm_base_signal_desc uct_rocm_base_signal_desc_t;

/* Called after async copy completes to release the remote mapping.
 * NULL means use the default hsa_amd_ipc_memory_detach path. */
typedef void (*uct_rocm_unmap_fn_t)(uct_rocm_base_signal_desc_t *desc);

struct uct_rocm_base_signal_desc {
    hsa_signal_t         signal;
    void                 *mapped_addr;
    size_t               mapped_size;  /* used by VMM unmap path */
    uct_rocm_unmap_fn_t  unmap_fn;    /* NULL → hsa_amd_ipc_memory_detach */
    uct_completion_t     *comp;
    ucs_queue_elem_t     queue;
};

extern ucs_mpool_ops_t uct_rocm_base_signal_desc_mpool_ops;

unsigned uct_rocm_base_progress(ucs_queue_head_t *signal_queue);

#endif
