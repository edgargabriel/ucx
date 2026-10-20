/*
 * Copyright (C) Advanced Micro Devices, Inc. 2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifndef ROCM_BASE_CACHE_H
#define ROCM_BASE_CACHE_H

#include <ucs/datastruct/pgtable.h>
#include <ucs/datastruct/list.h>
#include <ucs/sys/sys.h>
#include <ucs/type/status.h>

#include <sys/types.h>


/*
 * Transport-specific callbacks plugged in at cache creation time.
 * Separates the shared pgtable/hash infrastructure from the transport-
 * specific HSA map/unmap API calls.
 */
typedef struct uct_rocm_base_cache_ops {
    /*
     * Map a remote memory key into a local VA.
     * On success: *mapped_va_p holds the mapped address,
     *             *transport_ctx_p holds an opaque handle (or NULL)
     *             that will be passed to unmap() on eviction.
     */
    ucs_status_t (*map)(const void *key, size_t length,
                        void **mapped_va_p, void **transport_ctx_p);

    /*
     * Unmap and release a previously mapped region.
     * transport_ctx is whatever map() stored in *transport_ctx_p.
     */
    void         (*unmap)(void *mapped_va, size_t length, void *transport_ctx);

    /*
     * Return non-zero if the transport-specific sub-field of two keys
     * is equal (used for cache-hit detection).
     */
    int          (*key_equal)(const void *key_a, const void *key_b);
} uct_rocm_base_cache_ops_t;

typedef struct uct_rocm_base_cache {
    ucs_pgtable_t                  pgtable;
    char                           *name;
    const uct_rocm_base_cache_ops_t *ops;
} uct_rocm_base_cache_t;

/*
 * Common layout for every cached region. Each transport's key is stored
 * as a raw byte blob (the full key struct), giving the ops callbacks
 * access to any field.
 */
typedef struct uct_rocm_base_cache_region {
    ucs_pgt_region_t       super;         /**< Base class */
    ucs_list_link_t        list;          /**< List element for batch ops */
    void                   *mapped_va;   /**< Mapped local VA */
    void                   *transport_ctx; /**< Opaque handle for unmap() */
    uintptr_t              key_address;  /**< Base address in remote process */
    size_t                 key_length;   /**< Length of the mapping */
    uint8_t                key_data[];   /**< Flexible array: full key struct */
} uct_rocm_base_cache_region_t;

/*
 * Hash key for the per-peer remote-cache table.
 */
typedef struct uct_rocm_base_cache_peer_key {
    pid_t        pid;
    ucs_sys_ns_t pid_ns;
    int          dev_num;
} uct_rocm_base_cache_peer_key_t;

ucs_status_t uct_rocm_base_cache_create(uct_rocm_base_cache_t **cache,
                                        const char *name,
                                        const uct_rocm_base_cache_ops_t *ops);

void uct_rocm_base_cache_destroy(uct_rocm_base_cache_t *cache);

/*
 * Map a remote memory handle into the local address space, using a per-peer
 * cache keyed by (pid, pid_ns, dev_num).
 *
 * @key          Full transport key struct (IPC or fabric).
 * @key_size     sizeof the key struct.
 * @peer         Identity of the remote peer.
 * @address      Base address in the remote process.
 * @length       Length of the mapping.
 * @cache_name   Name prefix for a new per-peer cache (e.g. "rocm_ipc").
 * @mapped_va_p  [out] Mapped local VA on success.
 */
ucs_status_t
uct_rocm_base_cache_map_memhandle(const void *key, size_t key_size,
                                  uct_rocm_base_cache_peer_key_t peer,
                                  uintptr_t address, size_t length,
                                  const char *cache_name,
                                  const uct_rocm_base_cache_ops_t *ops,
                                  void **mapped_va_p);

#endif
