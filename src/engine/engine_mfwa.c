/**
 * Multi-factor cache mode (with write-around) implementation.
 *
 * Writes always follow Write-Around (for now). Reads swtich between
 * cache & core according to `load_admit`. Reads populate lines into
 * cache only if `data_admit` is on (i.e., in workload probing stage).
 */

/*========== [Orthus FLAG BEGIN] ==========*/

#include <stdbool.h>
#include "ocf/ocf.h"
#include "../ocf_cache_priv.h"
#include "../ocf_request.h"
#include "../utils/utils_io.h"
#include "../utils/utils_cache_line.h"
#include "../utils/utils_part.h"
#include "../concurrency/ocf_concurrency.h"
#include "../metadata/metadata.h"
#include "engine_debug.h"
#include "engine_rd.h"
#include "engine_pt.h"
#include "engine_inv.h"
#include "engine_bf.h"
#include "engine_common.h"
#include "cache_engine.h"
#include "engine_mfwa.h"
#include "mf_monitor.h"


#define OCF_ENGINE_DEBUG_IO_NAME "mfwa"


/**
 * These two switches should be controlled by a monitor, but for
 * now hardcoded into engine.
 */
static inline bool data_admit_allow()
{
    return monitor_query_data_admit();
}

static inline bool load_admit_allow()
{
    double load_admit = monitor_query_load_admit();

    return (((double) rand()) / RAND_MAX) <= load_admit; 
}


/**
 * Below are MFC with write-around - read implementation.
 */
static void _ocf_read_mfwa_to_cache_cmpl(struct ocf_request *req, int error)
{
    if (error)
        req->error |= error;

    if (req->error)
        inc_fallback_pt_error_counter(req->cache);

    /**
     * Handle callback-caller race to let only one of the two complete the
     * request. Also, complete original request only if this is the last
     * sub-request to complete
     */
    if (env_atomic_dec_return(&req->req_remaining) == 0) {
        OCF_DEBUG_RQ(req, "TO_CACHE completion");

        /** If error, fallback to PT. */
        if (req->error) {
            ocf_core_stats_cache_error_update(req->core, OCF_READ);
            ocf_engine_push_req_front_pt(req);
        
        } else {
            ocf_req_unlock(req);
            req->complete(req, req->error);
            ocf_req_put(req);
        }
    }
}

static inline void _ocf_read_mfwa_submit_to_cache(struct ocf_request *req)
{
    env_atomic_set(&req->req_remaining, ocf_engine_io_count(req));

    ocf_submit_cache_reqs(req->cache, req, OCF_READ, 0, req->byte_length,
                          ocf_engine_io_count(req),
                          _ocf_read_mfwa_to_cache_cmpl);
}

static void _ocf_read_mfwa_to_core_cmpl_do_promote(struct ocf_request *req,
                                                   int error)
{
    struct ocf_cache *cache = req->cache;

    if (error)
        req->error = error;

    /**
     * Handle callback-caller race to let only one of the two complete the
     * request. Also, complete original request only if this is the last
     * sub-request to complete.
     */
    if (env_atomic_dec_return(&req->req_remaining) == 0) {
        OCF_DEBUG_RQ(req, "TO_CORE completion");

        /**
         * If error, do not submit this request to backfill thread.
         * Stop it here.
         */
        if (req->error) {
            req->complete(req, req->error);

            req->info.core_error = 1;
            ocf_core_stats_core_error_update(req->core, OCF_READ);

            ctx_data_free(cache->owner, req->cp_data);
            req->cp_data = NULL;

            /* Invalidate metadata */
            ocf_engine_invalidate(req);

            return;
        }

        /**
         * Copy pages to copy vec, since this is the one needed by
         * the above layer. Then, complete request and start backfill.
         */
        ctx_data_cpy(cache->owner, req->cp_data, req->data, 0, 0,
                     req->byte_length);
        req->complete(req, req->error);
        ocf_engine_backfill(req);
    }
}

static void _ocf_read_mfwa_to_core_cmpl_no_promote(struct ocf_request *req,
                                                   int error)
{
    struct ocf_cache *cache = req->cache;

    if (error)
        req->error = error;

    /**
     * Handle callback-caller race to let only one of the two complete the
     * request. Also, complete original request only if this is the last
     * sub-request to complete.
     */
    if (env_atomic_dec_return(&req->req_remaining) == 0) {
        OCF_DEBUG_RQ(req, "TO_CORE completion");

        /**
         * If error, do not submit this request to backfill thread.
         * Stop it here.
         */
        if (req->error) {
            req->complete(req, req->error);

            req->info.core_error = 1;
            ocf_core_stats_core_error_update(req->core, OCF_READ);

            ctx_data_free(cache->owner, req->cp_data);
            req->cp_data = NULL;

            /* Invalidate metadata */
            ocf_engine_invalidate(req);

            return;
        }

        /** Not doing promotion, so end here. */
        req->complete(req, req->error);
        ocf_req_put(req);
    }
}

static inline void _ocf_read_mfwa_submit_to_core(struct ocf_request *req,
                                                 bool promote)
{
    struct ocf_cache *cache = req->cache;
    int ret;

    env_atomic_set(&req->req_remaining, 1);

    /**
     * Doing promotion. Allocate `cp_data` region for backfilling
     * purpose. Submit read request to volume and assign
     * `do_promote` version completion callback.
     */
    if (promote) {
        req->cp_data = ctx_data_alloc(cache->owner,
                                      BYTES_TO_PAGES(req->byte_length));
        if (!req->cp_data) {
            _ocf_read_mfwa_to_core_cmpl_do_promote(req, -OCF_ERR_NO_MEM);
            return;
        }

        ret = ctx_data_mlock(cache->owner, req->cp_data);
        if (ret) {
            _ocf_read_mfwa_to_core_cmpl_do_promote(req, -OCF_ERR_NO_MEM);
            return;
        }

        /** Submit read request to core device. */
        ocf_submit_volume_req(&req->core->volume, req,
                              _ocf_read_mfwa_to_core_cmpl_do_promote);

    /** Not doing promotion. */
    } else {
        ocf_submit_volume_req(&req->core->volume, req,
                              _ocf_read_mfwa_to_core_cmpl_no_promote);
    }
}

static int _ocf_read_mfwa_do(struct ocf_request *req)
{
    /** Get OCF request - increase reference counter */
    ocf_req_get(req);

    /**
     * Probably some cache lines are assigned into wrong
     * partition. Need to move it to new one.
     */
    if (req->info.re_part) {
        OCF_DEBUG_RQ(req, "Re-Part");
        ocf_req_hash_lock_wr(req);
        ocf_part_move(req);
        ocf_req_hash_unlock_wr(req);
    }

    /**
     * Actual read logic beginning here.
     */
    if (ocf_engine_is_hit(req)) {

        /** Hit && p <= load_admit. */
        if (req->load_admit_allowed) {
            OCF_DEBUG_RQ(req, "Submit");
            _ocf_read_mfwa_submit_to_cache(req);

        /** Hit && p > load_admit. */
        } else {
            OCF_DEBUG_RQ(req, "Submit");
            _ocf_read_mfwa_submit_to_core(req, false);
        }

    } else {

        /**
         * Miss && data_admit is on.
         * Only in this condition, we allow promotion to cache.
         */
        if (req->data_admit_allowed) {
            if (req->map->rd_locked) {  /** Not properly write-locked. */
                OCF_DEBUG_RQ(req, "Switching to PT");
                ocf_read_pt_do(req);
                return 0;
            }

            if (req->info.dirty_any) {  /** Dirty req - should not happen. */
                ocf_req_hash_lock_rd(req);
                ocf_engine_clean(req);
                ocf_req_hash_unlock_rd(req);
                ocf_req_put(req);
                return 0;
            }
            
            /** Set valid bits map. */
            ocf_req_hash_lock_rd(req);
            ocf_set_valid_map_info(req);
            ocf_req_hash_unlock_rd(req);

            OCF_DEBUG_RQ(req, "Submit");
            _ocf_read_mfwa_submit_to_core(req, true);

        /** Miss && data_admit is off. */
        } else {
            OCF_DEBUG_RQ(req, "Submit");
            _ocf_read_mfwa_submit_to_core(req, false);
        }
    }

    /**
     * Update statistics.
     */
    ocf_engine_update_request_stats(req);
    ocf_engine_update_block_stats(req);

    /** Put OCF request - decrease reference counter */
    ocf_req_put(req);

    return 0;
}

/** Lock type should match the algorithm logic. */
static enum ocf_engine_lock_type ocf_read_mfwa_get_lock_type(struct ocf_request *req)
{
    if (ocf_engine_is_hit(req)) {
        if (req->load_admit_allowed)
            return ocf_engine_lock_read;
        else
            return ocf_engine_lock_none;
    } else {
        if (req->data_admit_allowed)
            return ocf_engine_lock_write;
        else
            return ocf_engine_lock_none;
    }
}

static const struct ocf_io_if _io_if_read_mfwa_resume = {
    .read = _ocf_read_mfwa_do,
    .write = _ocf_read_mfwa_do,
};

static const struct ocf_engine_callbacks _read_mfwa_engine_callbacks =
{
    .get_lock_type = ocf_read_mfwa_get_lock_type,
    .resume = ocf_engine_on_resume,
};

/**
 * Multi-factor read with write-around.
 *
 * If fully hit && p <= `load_admit`, we read from cache. Otherwise, we
 * read from core.
 *
 * When miss and reading from core, we promote core lines into cache only
 * if the `data_admit` switch is on. Promotion decision is not implemented
 * into a new promotion policy because the promotion policy interface seems
 * redundant and too complicated for our algo's logic.
 */
int ocf_read_mfwa(struct ocf_request *req)
{
    int lock = OCF_LOCK_NOT_ACQUIRED;
    struct ocf_cache *cache = req->cache;

    ocf_io_start(&req->ioi.io);

    /** There are conditions to bypass IO. */
    if (env_atomic_read(&cache->pending_read_misses_list_blocked)) {
        ocf_get_io_if(ocf_cache_mode_pt)->read(req);
        return 0;
    }

    /** Get OCF request - increase reference counter */
    ocf_req_get(req);

    /**
     * Query the current multi-factor config and assign `load_admit` &
     * `data_admit` behavior to this request.
     */
    req->data_admit_allowed = data_admit_allow();
    req->load_admit_allowed = load_admit_allow();

    /** Set resume call backs. */
    req->io_if = &_io_if_read_mfwa_resume;

    lock = ocf_engine_prepare_clines(req, &_read_mfwa_engine_callbacks);

    if (!req->info.mapping_error) {
        if (lock >= 0) {
            if (lock != OCF_LOCK_ACQUIRED) {
                /** Lock was not acquired, need to wait for resume */
                OCF_DEBUG_RQ(req, "NO LOCK");
            } else {
                /** Lock was acquired can perform IO. */
                _ocf_read_mfwa_do(req);
            }
        } else {
            OCF_DEBUG_RQ(req, "LOCK ERROR %d", lock);
            req->complete(req, lock);
            ocf_req_put(req);
        }
    } else {
        ocf_req_clear(req);
        ocf_get_io_if(ocf_cache_mode_pt)->read(req);
    }

    /** Put OCF request - decrease reference counter */
    ocf_req_put(req);

    return 0;
}

/*========== [Orthus FLAG END] ==========*/
