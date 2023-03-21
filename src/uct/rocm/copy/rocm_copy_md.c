/*
 * Copyright (C) Advanced Micro Devices, Inc. 2019-2023. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "rocm_copy_md.h"

#include <uct/rocm/base/rocm_base.h>

#include <string.h>
#include <limits.h>
#include <ucs/debug/log.h>
#include <ucs/sys/sys.h>
#include <ucs/sys/math.h>
#include <ucs/debug/memtrack_int.h>
#include <ucm/api/ucm.h>
#include <ucs/type/class.h>
#include <uct/api/v2/uct_v2.h>
#include <hsa_ext_amd.h>

static ucs_config_field_t uct_rocm_copy_md_config_table[] = {
    {"", "", NULL,
     ucs_offsetof(uct_rocm_copy_md_config_t, super),
     UCS_CONFIG_TYPE_TABLE(uct_md_config_table)},

    {NULL}
};

static ucs_status_t
uct_rocm_copy_md_query(uct_md_h md, uct_md_attr_v2_t *md_attr)
{
    md_attr->flags            = UCT_MD_FLAG_REG |
                                UCT_MD_FLAG_ALLOC;
    md_attr->reg_mem_types    = UCS_BIT(UCS_MEMORY_TYPE_HOST) |
                                UCS_BIT(UCS_MEMORY_TYPE_ROCM);
    md_attr->reg_nonblock_mem_types = 0;
    md_attr->cache_mem_types  = UCS_BIT(UCS_MEMORY_TYPE_ROCM);
    md_attr->alloc_mem_types  = UCS_BIT(UCS_MEMORY_TYPE_ROCM);
    md_attr->access_mem_types = UCS_BIT(UCS_MEMORY_TYPE_ROCM);
    md_attr->detect_mem_types = UCS_BIT(UCS_MEMORY_TYPE_ROCM);
    md_attr->dmabuf_mem_types = 0;
    if (uct_rocm_base_is_dmabuf_supported()) {
        md_attr->dmabuf_mem_types |= UCS_BIT(UCS_MEMORY_TYPE_ROCM);
    }
    md_attr->max_alloc        = SIZE_MAX;
    md_attr->max_reg          = ULONG_MAX;
    md_attr->rkey_packed_size = 0;
    md_attr->reg_cost         = UCS_LINEAR_FUNC_ZERO;
    memset(&md_attr->local_cpus, 0xff, sizeof(md_attr->local_cpus));

    return UCS_OK;
}

static ucs_status_t
uct_rocm_copy_mkey_pack(uct_md_h uct_md, uct_mem_h memh,
                        const uct_md_mkey_pack_params_t *params,
                        void *mkey_buffer)
{
    return UCS_OK;
}

static ucs_status_t uct_rocm_copy_rkey_unpack(uct_component_t *component,
                                              const void *rkey_buffer,
                                              uct_rkey_t *rkey_p, void **handle_p)
{
    *handle_p = NULL;
    *rkey_p   = 0xdeadbeef;

    return UCS_OK;
}

static ucs_status_t uct_rocm_copy_rkey_release(uct_component_t *component,
                                               uct_rkey_t rkey, void *handle)
{
    return UCS_OK;
}


static ucs_status_t
uct_rocm_copy_mem_reg(uct_md_h md, void *address, size_t length,
                      const uct_md_mem_reg_params_t *params, uct_mem_h *memh_p)
{
    *memh_p = (void*)0xdeadbeef;
    return UCS_OK;
}

static ucs_status_t
uct_rocm_copy_mem_dereg(uct_md_h md,
                        const uct_md_mem_dereg_params_t *params)
{
    return UCS_OK;
}

static void uct_rocm_copy_md_close(uct_md_h uct_md)
{
    uct_rocm_copy_md_t *md = ucs_derived_of(uct_md, uct_rocm_copy_md_t);

    ucs_free(md);
}

static ucs_status_t
uct_rocm_copy_mem_alloc(uct_md_h md, size_t *length_p, void **address_p,
                        ucs_memory_type_t mem_type, unsigned flags,
                        const char *alloc_name, uct_mem_h *memh_p)
{
    ucs_status_t status;
    hsa_status_t hsa_status;
    hsa_amd_memory_pool_t pool;

    if (mem_type != UCS_MEMORY_TYPE_ROCM) {
        return UCS_ERR_UNSUPPORTED;
    }

    status = uct_rocm_base_get_last_device_pool(&pool);
    if (status != UCS_OK) {
        return status;
    }

    hsa_status = hsa_amd_memory_pool_allocate(pool, *length_p, 0, address_p);
    if (hsa_status != HSA_STATUS_SUCCESS) {
        ucs_debug("could not allocate HSA memory: 0x%x", hsa_status);
        return UCS_ERR_UNSUPPORTED;
    }

    *memh_p = *address_p;
    return UCS_OK;
}

static ucs_status_t uct_rocm_copy_mem_free(uct_md_h md, uct_mem_h memh)
{
    hsa_status_t hsa_status;

    hsa_status = hsa_amd_memory_pool_free((void*)memh);
    if ((hsa_status != HSA_STATUS_SUCCESS) &&
        (hsa_status != HSA_STATUS_INFO_BREAK)) {
        ucs_debug("could not free HSA memory 0x%x", hsa_status);
        return UCS_ERR_UNSUPPORTED;
    }

    return UCS_OK;
}

static uct_md_ops_t md_ops = {
    .close                  = uct_rocm_copy_md_close,
    .query                  = uct_rocm_copy_md_query,
    .mkey_pack              = uct_rocm_copy_mkey_pack,
    .mem_alloc              = uct_rocm_copy_mem_alloc,
    .mem_free               = uct_rocm_copy_mem_free,
    .mem_reg                = uct_rocm_copy_mem_reg,
    .mem_dereg              = uct_rocm_copy_mem_dereg,
    .mem_attach             = ucs_empty_function_return_unsupported,
    .mem_query              = uct_rocm_base_mem_query,
    .detect_memory_type     = uct_rocm_base_detect_memory_type,
    .is_sockaddr_accessible = ucs_empty_function_return_zero_int,
};

static ucs_status_t
uct_rocm_copy_md_open(uct_component_h component, const char *md_name,
                      const uct_md_config_t *config, uct_md_h *md_p)
{
    ucs_status_t status;
    uct_rocm_copy_md_t *md;

    md = ucs_malloc(sizeof(uct_rocm_copy_md_t), "uct_rocm_copy_md_t");
    if (NULL == md) {
        ucs_error("Failed to allocate memory for uct_rocm_copy_md_t");
        return UCS_ERR_NO_MEMORY;
    }

    md->super.ops       = &md_ops;
    md->super.component = &uct_rocm_copy_component;
    md->reg_cost        = UCS_LINEAR_FUNC_ZERO;

    *md_p = (uct_md_h) md;
    status = UCS_OK;
out:
    return status;
err:
    ucs_free(md);
    goto out;
}

uct_component_t uct_rocm_copy_component = {
    .query_md_resources = uct_rocm_base_query_md_resources,
    .md_open            = uct_rocm_copy_md_open,
    .cm_open            = ucs_empty_function_return_unsupported,
    .rkey_unpack        = uct_rocm_copy_rkey_unpack,
    .rkey_ptr           = ucs_empty_function_return_unsupported,
    .rkey_release       = uct_rocm_copy_rkey_release,
    .name               = "rocm_cpy",
    .md_config          = {
        .name           = "ROCm-copy memory domain",
        .prefix         = "ROCM_COPY_",
        .table          = uct_rocm_copy_md_config_table,
        .size           = sizeof(uct_rocm_copy_md_config_t),
    },
    .cm_config          = UCS_CONFIG_EMPTY_GLOBAL_LIST_ENTRY,
    .tl_list            = UCT_COMPONENT_TL_LIST_INITIALIZER(&uct_rocm_copy_component),
    .flags              = 0,
    .md_vfs_init        = (uct_component_md_vfs_init_func_t)ucs_empty_function
};
UCT_COMPONENT_REGISTER(&uct_rocm_copy_component);

