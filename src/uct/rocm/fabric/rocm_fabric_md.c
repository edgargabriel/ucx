/*
 * Copyright (C) Advanced Micro Devices, Inc. 2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "rocm_fabric_md.h"

#include <uct/rocm/base/rocm_base.h>
#include <uct/api/v2/uct_v2.h>

#include <unistd.h>


static ucs_config_field_t uct_rocm_fabric_md_config_table[] = {
    {"", "", NULL,
     ucs_offsetof(uct_rocm_fabric_md_config_t, super),
     UCS_CONFIG_TYPE_TABLE(uct_md_config_table)},

    {NULL}
};

static ucs_status_t uct_rocm_fabric_md_query(uct_md_h md,
                                             uct_md_attr_v2_t *md_attr)
{
    uct_md_base_md_query(md_attr);
    md_attr->rkey_packed_size = sizeof(uct_rocm_fabric_key_t);
    md_attr->flags            = UCT_MD_FLAG_REG | UCT_MD_FLAG_NEED_RKEY |
                                UCT_MD_FLAG_INVALIDATE |
                                UCT_MD_FLAG_INVALIDATE_RMA |
                                UCT_MD_FLAG_INVALIDATE_AMO;
    md_attr->reg_mem_types    = UCS_BIT(UCS_MEMORY_TYPE_ROCM_VMM_FABRIC);
    md_attr->cache_mem_types  = UCS_BIT(UCS_MEMORY_TYPE_ROCM_VMM_FABRIC);
    md_attr->access_mem_types = UCS_BIT(UCS_MEMORY_TYPE_ROCM_VMM_FABRIC);
    md_attr->reg_cost         = ucs_linear_func_make(9e-9, 0);
    return UCS_OK;
}

static ucs_status_t
uct_rocm_fabric_mkey_pack(uct_md_h uct_md, uct_mem_h memh, void *address,
                          size_t length, const uct_md_mkey_pack_params_t *params,
                          void *mkey_buffer)
{
    uct_rocm_fabric_key_t *packed = mkey_buffer;
    uct_rocm_fabric_key_t *key    = memh;

    *packed = *key;
    return UCS_OK;
}

static ucs_status_t
uct_rocm_fabric_mem_reg(uct_md_h md, void *address, size_t length,
                        const uct_md_mem_reg_params_t *params,
                        uct_mem_h *memh_p)
{
    uct_rocm_fabric_key_t *key;
    hsa_amd_vmem_alloc_handle_t vmem_handle;
    hsa_status_t status;
    void *base_ptr = NULL;
    size_t base_size = 0;
    hsa_amd_pointer_type_t hsa_mem_type;
    hsa_agent_t agent;

    status = uct_rocm_base_get_ptr_info(address, length, &base_ptr, &base_size,
                                        &hsa_mem_type, &agent, NULL);
    if (status != HSA_STATUS_SUCCESS) {
        ucs_error("failed to get pointer info for %p/%zu", address, length);
        return UCS_ERR_INVALID_ADDR;
    }

    key = ucs_malloc(sizeof(*key), "uct_rocm_fabric_key_t");
    if (key == NULL) {
        ucs_error("failed to allocate uct_rocm_fabric_key_t");
        return UCS_ERR_NO_MEMORY;
    }

    status = hsa_amd_vmem_retain_alloc_handle(&vmem_handle, base_ptr);
    if (status != HSA_STATUS_SUCCESS) {
        ucs_error("hsa_amd_vmem_retain_alloc_handle(%p) failed: %d",
                  base_ptr, status);
        ucs_free(key);
        return UCS_ERR_INVALID_ADDR;
    }

    status = hsa_amd_vmem_export_fabric_handle(&key->fabric, vmem_handle, 0);
    if (status != HSA_STATUS_SUCCESS) {
        ucs_error("hsa_amd_vmem_export_fabric_handle(%p) failed: %d",
                  base_ptr, status);
        hsa_amd_vmem_handle_release(vmem_handle);
        ucs_free(key);
        return UCS_ERR_INVALID_ADDR;
    }

    /* Release the retained handle — the fabric handle keeps its own reference */
    hsa_amd_vmem_handle_release(vmem_handle);

    key->address = (uintptr_t)base_ptr;
    key->length  = base_size;
    key->dev_num = uct_rocm_base_get_dev_num(agent);
    key->pid     = getpid();
    key->pid_ns  = ucs_sys_get_ns(UCS_SYS_NS_TYPE_PID);

    *memh_p = key;
    return UCS_OK;
}

static ucs_status_t
uct_rocm_fabric_mem_dereg(uct_md_h md, const uct_md_mem_dereg_params_t *params)
{
    UCT_MD_MEM_DEREG_CHECK_PARAMS(params, 0);
    ucs_free(params->memh);
    return UCS_OK;
}

static ucs_status_t
uct_rocm_fabric_md_open(uct_component_h component, const char *md_name,
                        const uct_md_config_t *uct_md_config, uct_md_h *md_p)
{
    static uct_md_ops_t md_ops = {
        .close              = (uct_md_close_func_t)ucs_empty_function,
        .query              = uct_rocm_fabric_md_query,
        .mem_alloc          = (uct_md_mem_alloc_func_t)
                ucs_empty_function_return_unsupported,
        .mem_free           = (uct_md_mem_free_func_t)
                ucs_empty_function_return_unsupported,
        .mem_advise         = (uct_md_mem_advise_func_t)
                ucs_empty_function_return_unsupported,
        .mem_reg            = uct_rocm_fabric_mem_reg,
        .mem_dereg          = uct_rocm_fabric_mem_dereg,
        .mem_query          = (uct_md_mem_query_func_t)
                ucs_empty_function_return_unsupported,
        .mkey_pack          = uct_rocm_fabric_mkey_pack,
        .mem_elem_pack      = (uct_md_mem_elem_pack_func_t)
                ucs_empty_function_return_unsupported,
        .mem_attach         = (uct_md_mem_attach_func_t)
                ucs_empty_function_return_unsupported,
        .detect_memory_type = (uct_md_detect_memory_type_func_t)
                ucs_empty_function_return_unsupported,
        .mem_elem_release   = (uct_md_mem_elem_release_func_t)ucs_empty_function
    };
    static uct_md_t md = {
        .ops       = &md_ops,
        .component = &uct_rocm_fabric_component.super,
    };

    *md_p = &md;
    return UCS_OK;
}

static ucs_status_t
uct_rocm_fabric_rkey_unpack(uct_component_t *component,
                            const void *rkey_buffer,
                            const uct_rkey_unpack_params_t *params,
                            uct_rkey_t *rkey_p, void **handle_p)
{
    const uct_rocm_fabric_key_t *packed = rkey_buffer;
    uct_rocm_fabric_key_t *key;

    key = ucs_malloc(sizeof(*key), "uct_rocm_fabric_key_t");
    if (key == NULL) {
        ucs_error("failed to allocate uct_rocm_fabric_key_t");
        return UCS_ERR_NO_MEMORY;
    }

    *key      = *packed;
    *handle_p = NULL;
    *rkey_p   = (uintptr_t)key;
    return UCS_OK;
}

static ucs_status_t
uct_rocm_fabric_rkey_release(uct_component_t *component, uct_rkey_t rkey,
                             void *handle)
{
    ucs_assert(NULL == handle);
    ucs_free((void *)rkey);
    return UCS_OK;
}

uct_rocm_fabric_component_t uct_rocm_fabric_component = {
    .super = {
        .query_md_resources = uct_rocm_base_query_md_resources,
        .md_open            = uct_rocm_fabric_md_open,
        .cm_open            = (uct_component_cm_open_func_t)
                              ucs_empty_function_return_unsupported,
        .rkey_unpack        = uct_rocm_fabric_rkey_unpack,
        .rkey_ptr           = (uct_component_rkey_ptr_func_t)
                              ucs_empty_function_return_unsupported,
        .rkey_release       = uct_rocm_fabric_rkey_release,
        .rkey_compare       = uct_base_rkey_compare,
        .name               = "rocm_fabric",
        .md_config = {
            .name   = "ROCm-Fabric memory domain",
            .prefix = "ROCM_FABRIC_MD_",
            .table  = uct_rocm_fabric_md_config_table,
            .size   = sizeof(uct_rocm_fabric_md_config_t),
        },
        .cm_config   = UCS_CONFIG_EMPTY_GLOBAL_LIST_ENTRY,
        .tl_list     = UCT_COMPONENT_TL_LIST_INITIALIZER(
                           &uct_rocm_fabric_component.super),
        .flags       = 0,
        .md_vfs_init = (uct_component_md_vfs_init_func_t)ucs_empty_function
    }
};
UCT_COMPONENT_REGISTER(&uct_rocm_fabric_component.super);
