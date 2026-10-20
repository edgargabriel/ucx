/*
 * Copyright (C) Advanced Micro Devices, Inc. 2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifndef ROCM_FABRIC_MD_H
#define ROCM_FABRIC_MD_H

#include <uct/base/uct_md.h>
#include <ucs/sys/sys.h>
#include <hsa_ext_amd.h>


typedef struct uct_rocm_fabric_component {
    uct_component_t super;
} uct_rocm_fabric_component_t;

extern uct_rocm_fabric_component_t uct_rocm_fabric_component;

typedef struct uct_rocm_fabric_md {
    struct uct_md super;
} uct_rocm_fabric_md_t;

typedef struct uct_rocm_fabric_md_config {
    uct_md_config_t super;
} uct_rocm_fabric_md_config_t;

/*
 * Memory key packed into the rkey buffer and exchanged between peers.
 * Uses hsa_fabric_handle_t (16-byte globally unique UUID) instead of the
 * process-local hsa_amd_ipc_memory_t used by rocm_ipc.
 */
typedef struct uct_rocm_fabric_key {
    hsa_fabric_handle_t  fabric;   /* globally unique, cross-process handle */
    uintptr_t            address;  /* base VA in the exporting process */
    size_t               length;
    int                  dev_num;
    pid_t                pid;
    ucs_sys_ns_t         pid_ns;
} uct_rocm_fabric_key_t;

#endif
