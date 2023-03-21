/*
 * Copyright (C) Advanced Micro Devices, Inc. 2019. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifndef UCT_ROCM_COPY_MD_H
#define UCT_ROCM_COPY_MD_H

#include <uct/base/uct_md.h>
#include <ucs/config/types.h>
#include <ucs/memory/rcache.h>


extern uct_component_t uct_rocm_copy_component;

/*
 * @brief rocm_copy MD descriptor
 */
typedef struct uct_rocm_copy_md {
    uct_md_t            super;      /**< Domain info */
    ucs_linear_func_t   reg_cost;   /**< Memory registration cost */
} uct_rocm_copy_md_t;


/**
 * rocm copy domain configuration.
 */
typedef struct uct_rocm_copy_md_config {
    uct_md_config_t             super;
    ucs_linear_func_t           uc_reg_cost;  /**< Memory registration cost estimation
                                                   without using the cache */
} uct_rocm_copy_md_config_t;


#endif
