/*
 * Copyright (C) Advanced Micro Devices, Inc. 2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifndef ROCM_FABRIC_CACHE_H
#define ROCM_FABRIC_CACHE_H

#include <uct/rocm/base/rocm_base_cache.h>
#include <uct/rocm/base/rocm_signal.h>
#include "rocm_fabric_md.h"


ucs_status_t uct_rocm_fabric_cache_map_memhandle(
        const uct_rocm_fabric_key_t *key, void **mapped_va_p,
        size_t *mapped_size_out);

/*
 * Unmap a VMM region that was imported via the fabric cache.
 * Called as uct_rocm_unmap_fn_t from the signal progress path when the
 * handle-cache is disabled (non-cached path).
 */
void uct_rocm_fabric_vmem_unmap(uct_rocm_base_signal_desc_t *desc);

#endif
