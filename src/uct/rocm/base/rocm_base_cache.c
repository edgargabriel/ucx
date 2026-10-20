/*
 * Copyright (C) Advanced Micro Devices, Inc. 2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "rocm_base_cache.h"

#include <ucs/datastruct/khash.h>
#include <ucs/debug/log.h>
#include <ucs/debug/memtrack_int.h>
#include <ucs/profile/profile.h>
#include <ucs/sys/ptr_arith.h>
#include <ucs/sys/string.h>
#include <ucs/type/rwlock.h>


/* --------------------------------------------------------------------------
 * Per-peer hash table
 * -------------------------------------------------------------------------- */

static UCS_F_ALWAYS_INLINE int
uct_rocm_base_cache_peer_key_equal(uct_rocm_base_cache_peer_key_t k1,
                                   uct_rocm_base_cache_peer_key_t k2)
{
    return (k1.pid == k2.pid) && (k1.pid_ns == k2.pid_ns) &&
           (k1.dev_num == k2.dev_num);
}

static UCS_F_ALWAYS_INLINE khint32_t
uct_rocm_base_cache_peer_key_hash(uct_rocm_base_cache_peer_key_t key)
{
    uint64_t value = (uint64_t)key.pid ^ ((uint64_t)key.pid_ns << 32) ^
                     ((uint32_t)key.dev_num << 24);
    return kh_int64_hash_func(value);
}

KHASH_INIT(rocm_base_rem_cache, uct_rocm_base_cache_peer_key_t,
           uct_rocm_base_cache_t*, 1,
           uct_rocm_base_cache_peer_key_hash,
           uct_rocm_base_cache_peer_key_equal)

typedef struct uct_rocm_base_remote_cache {
    khash_t(rocm_base_rem_cache) hash;
    ucs_rw_spinlock_t            lock;
} uct_rocm_base_remote_cache_t;

static uct_rocm_base_remote_cache_t uct_rocm_base_remote_cache;


/* --------------------------------------------------------------------------
 * Page-table directory alloc/release
 * -------------------------------------------------------------------------- */

static ucs_pgt_dir_t *
uct_rocm_base_cache_pgt_dir_alloc(const ucs_pgtable_t *pgtable)
{
    void *ptr;
    int ret;

    ret = ucs_posix_memalign(&ptr,
                             ucs_max(sizeof(void *), UCS_PGT_ENTRY_MIN_ALIGN),
                             sizeof(ucs_pgt_dir_t), "rocm_base_cache_pgdir");
    return (ret == 0) ? ptr : NULL;
}

static void uct_rocm_base_cache_pgt_dir_release(const ucs_pgtable_t *pgtable,
                                                ucs_pgt_dir_t *dir)
{
    ucs_free(dir);
}


/* --------------------------------------------------------------------------
 * Region helpers
 * -------------------------------------------------------------------------- */

static void
uct_rocm_base_cache_region_collect_cb(const ucs_pgtable_t *pgtable,
                                      ucs_pgt_region_t *pgt_region, void *arg)
{
    uct_rocm_base_cache_region_t *r = ucs_derived_of(pgt_region,
                                                      uct_rocm_base_cache_region_t);
    ucs_list_add_tail((ucs_list_link_t *)arg, &r->list);
}

static void uct_rocm_base_cache_release_region(uct_rocm_base_cache_t *cache,
                                               uct_rocm_base_cache_region_t *region)
{
    cache->ops->unmap(region->mapped_va, region->key_length,
                      region->transport_ctx);
    ucs_free(region);
}

static void uct_rocm_base_cache_purge(uct_rocm_base_cache_t *cache)
{
    uct_rocm_base_cache_region_t *region, *tmp;
    ucs_list_link_t region_list;

    ucs_list_head_init(&region_list);
    ucs_pgtable_purge(&cache->pgtable,
                      uct_rocm_base_cache_region_collect_cb, &region_list);

    ucs_list_for_each_safe(region, tmp, &region_list, list) {
        uct_rocm_base_cache_release_region(cache, region);
    }
    ucs_trace("%s: rocm base cache purged", cache->name);
}

static void uct_rocm_base_cache_invalidate_regions(uct_rocm_base_cache_t *cache,
                                                   void *from, void *to)
{
    uct_rocm_base_cache_region_t *region, *tmp;
    ucs_list_link_t region_list;
    ucs_status_t status;

    ucs_list_head_init(&region_list);
    ucs_pgtable_search_range(&cache->pgtable,
                             (ucs_pgt_addr_t)from, (ucs_pgt_addr_t)to - 1,
                             uct_rocm_base_cache_region_collect_cb, &region_list);

    ucs_list_for_each_safe(region, tmp, &region_list, list) {
        status = ucs_pgtable_remove(&cache->pgtable, &region->super);
        if (status != UCS_OK) {
            ucs_error("failed to remove address:%p from cache (%s)",
                      (void *)region->key_address, ucs_status_string(status));
        }
        uct_rocm_base_cache_release_region(cache, region);
    }
    ucs_trace("%s: invalidated cache range [%p..%p]", cache->name, from, to);
}


/* --------------------------------------------------------------------------
 * cache_map_region — the shared pgtable lookup + insert skeleton.
 * The only transport-specific work is the ops->map / ops->unmap calls.
 * -------------------------------------------------------------------------- */

static ucs_status_t
uct_rocm_base_cache_map_region(uct_rocm_base_cache_t *cache,
                               const void *key, size_t key_size,
                               uintptr_t address, size_t length,
                               void **mapped_va_p)
{
    uct_rocm_base_cache_region_t *region;
    ucs_pgt_region_t *pgt_region;
    void *mapped_va;
    void *transport_ctx;
    ucs_status_t status;
    int ret;

    pgt_region = UCS_PROFILE_CALL(ucs_pgtable_lookup, &cache->pgtable, address);
    if (ucs_likely(pgt_region != NULL)) {
        region = ucs_derived_of(pgt_region, uct_rocm_base_cache_region_t);
        if (cache->ops->key_equal(key, region->key_data)) {
            ucs_trace("%s: cache hit addr:%p size:%zu region:" UCS_PGT_REGION_FMT,
                      cache->name, (void *)address, length,
                      UCS_PGT_REGION_ARG(&region->super));
            *mapped_va_p = region->mapped_va;
            return UCS_OK;
        }

        ucs_trace("%s: remove stale region:" UCS_PGT_REGION_FMT
                  " new_addr:%p new_size:%zu",
                  cache->name, UCS_PGT_REGION_ARG(&region->super),
                  (void *)address, length);
        status = ucs_pgtable_remove(&cache->pgtable, &region->super);
        if (status != UCS_OK) {
            ucs_error("%s: failed to remove address:%p from cache",
                      cache->name, (void *)address);
            goto err;
        }
        uct_rocm_base_cache_release_region(cache, region);
    }

    status = cache->ops->map(key, length, &mapped_va, &transport_ctx);
    if (ucs_unlikely(status != UCS_OK)) {
        goto err;
    }

    ret = ucs_posix_memalign((void **)&region,
                             ucs_max(sizeof(void *), UCS_PGT_ENTRY_MIN_ALIGN),
                             sizeof(*region) + key_size,
                             "uct_rocm_base_cache_region");
    if (ret != 0) {
        ucs_warn("failed to allocate rocm base cache region");
        status = UCS_ERR_NO_MEMORY;
        /* unmap what we just mapped since we cannot cache it */
        cache->ops->unmap(mapped_va, length, transport_ctx);
        goto err;
    }

    region->super.start   = ucs_align_down_pow2(address, UCS_PGT_ADDR_ALIGN);
    region->super.end     = ucs_align_up_pow2(address + length, UCS_PGT_ADDR_ALIGN);
    region->mapped_va     = mapped_va;
    region->transport_ctx = transport_ctx;
    region->key_address   = address;
    region->key_length    = length;
    memcpy(region->key_data, key, key_size);

    status = UCS_PROFILE_CALL(ucs_pgtable_insert, &cache->pgtable, &region->super);
    if (status == UCS_ERR_ALREADY_EXISTS) {
        uct_rocm_base_cache_invalidate_regions(cache,
                                               (void *)region->super.start,
                                               (void *)region->super.end);
        status = UCS_PROFILE_CALL(ucs_pgtable_insert,
                                  &cache->pgtable, &region->super);
    }
    if (status != UCS_OK) {
        ucs_error("%s: failed to insert region:" UCS_PGT_REGION_FMT
                  " size:%zu: %s",
                  cache->name, UCS_PGT_REGION_ARG(&region->super), length,
                  ucs_status_string(status));
        ucs_free(region);
        cache->ops->unmap(mapped_va, length, transport_ctx);
        goto err;
    }

    ucs_trace("%s: new region:" UCS_PGT_REGION_FMT " size:%zu",
              cache->name, UCS_PGT_REGION_ARG(&region->super), length);
    *mapped_va_p = mapped_va;
    return UCS_OK;

err:
    return status;
}


/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

ucs_status_t uct_rocm_base_cache_create(uct_rocm_base_cache_t **cache,
                                        const char *name,
                                        const uct_rocm_base_cache_ops_t *ops)
{
    uct_rocm_base_cache_t *c;
    ucs_status_t status;

    c = ucs_malloc(sizeof(*c), "uct_rocm_base_cache_t");
    if (c == NULL) {
        ucs_error("failed to allocate rocm base cache");
        return UCS_ERR_NO_MEMORY;
    }

    status = ucs_pgtable_init(&c->pgtable,
                              uct_rocm_base_cache_pgt_dir_alloc,
                              uct_rocm_base_cache_pgt_dir_release);
    if (status != UCS_OK) {
        goto err;
    }

    c->name = ucs_strdup(name, "rocm_base_cache_name");
    if (c->name == NULL) {
        status = UCS_ERR_NO_MEMORY;
        goto err_cleanup_pgtable;
    }

    c->ops = ops;
    *cache = c;
    return UCS_OK;

err_cleanup_pgtable:
    ucs_pgtable_cleanup(&c->pgtable);
err:
    ucs_free(c);
    return status;
}

void uct_rocm_base_cache_destroy(uct_rocm_base_cache_t *cache)
{
    uct_rocm_base_cache_purge(cache);
    ucs_pgtable_cleanup(&cache->pgtable);
    ucs_free(cache->name);
    ucs_free(cache);
}

/* Look up the per-peer cache, creating it on first use.
 * Caller must hold the global remote cache write lock. */
static ucs_status_t
uct_rocm_base_remote_cache_get(uct_rocm_base_cache_peer_key_t peer,
                               const char *cache_name,
                               const uct_rocm_base_cache_ops_t *ops,
                               uct_rocm_base_cache_t **cache_p)
{
    khash_t(rocm_base_rem_cache) *hash = &uct_rocm_base_remote_cache.hash;
    uct_rocm_base_cache_t *cache;
    char target_name[64];
    ucs_status_t status;
    khint_t it;
    int ret;

    it = kh_get(rocm_base_rem_cache, hash, peer);
    if (ucs_likely(it != kh_end(hash))) {
        *cache_p = kh_val(hash, it);
        return UCS_OK;
    }

    it = kh_put(rocm_base_rem_cache, hash, peer, &ret);
    if (ret == UCS_KH_PUT_FAILED) {
        ucs_error("failed to allocate rocm base remote_cache hash entry");
        return UCS_ERR_NO_MEMORY;
    }

    ucs_snprintf_safe(target_name, sizeof(target_name), "%s:dest:%d:%u:%d",
                      cache_name, (int)peer.pid, peer.pid_ns, peer.dev_num);
    status = uct_rocm_base_cache_create(&cache, target_name, ops);
    if (status != UCS_OK) {
        kh_del(rocm_base_rem_cache, hash, it);
        ucs_error("failed to create rocm base cache: %s",
                  ucs_status_string(status));
        return status;
    }

    kh_val(hash, it) = cache;
    *cache_p         = cache;
    return UCS_OK;
}

ucs_status_t
uct_rocm_base_cache_map_memhandle(const void *key, size_t key_size,
                                  uct_rocm_base_cache_peer_key_t peer,
                                  uintptr_t address, size_t length,
                                  const char *cache_name,
                                  const uct_rocm_base_cache_ops_t *ops,
                                  void **mapped_va_p)
{
    uct_rocm_base_cache_t *cache;
    ucs_status_t status;

    ucs_rw_spinlock_write_lock(&uct_rocm_base_remote_cache.lock);
    status = uct_rocm_base_remote_cache_get(peer, cache_name, ops, &cache);
    if (status == UCS_OK) {
        status = uct_rocm_base_cache_map_region(cache, key, key_size,
                                                address, length, mapped_va_p);
    }
    ucs_rw_spinlock_write_unlock(&uct_rocm_base_remote_cache.lock);

    return status;
}

UCS_STATIC_INIT
{
    ucs_rw_spinlock_init(&uct_rocm_base_remote_cache.lock);
    kh_init_inplace(rocm_base_rem_cache, &uct_rocm_base_remote_cache.hash);
}

UCS_STATIC_CLEANUP
{
    uct_rocm_base_cache_t *cache;

    kh_foreach_value(&uct_rocm_base_remote_cache.hash, cache,
                     { uct_rocm_base_cache_destroy(cache); })
    kh_destroy_inplace(rocm_base_rem_cache, &uct_rocm_base_remote_cache.hash);
    ucs_rw_spinlock_cleanup(&uct_rocm_base_remote_cache.lock);
}
