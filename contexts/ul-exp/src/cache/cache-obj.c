/**
 * Cache logical object implementation.
 *
 * After volume types have been registered, a logical object should then
 * be created, and a volume of some registered type be attached to the
 * object.
 */


#include <stdio.h>
#include <stdlib.h>
#include <ocf/ocf.h>

#include "queue.h"
#include "cache-vol.h"
#include "common.h"
#include "cache-obj.h"


/**
 * Callback states shared between OCF routines and callbacks during
 * cache setup. This callback function will be assigned to any OCF
 * routine executed during cache setup.
 */
struct cache_setup_callback_states {
    int *error;     /** Pointer to host's return value. */
};

static void
cache_setup_callback(ocf_cache_t cache, void *callback_states,
                     int error)
{
    struct cache_setup_callback_states *states = callback_states;

    *states->error = error;
}


/**
 * Callback states shared between OCF routines and callbacks during
 * cache stop. This callback function will be assigned to any OCF
 * routine executed during cache stop.
 */
struct cache_stop_callback_states {
    int *error;     /** Pointer to host's return value. */
};

static void
cache_stop_callback(ocf_cache_t cache, void *callback_states,
                    int error)
{
    struct cache_stop_callback_states *states = callback_states;

    *states->error = error;
}


/*========== Device log implementation BEGIN ==========*/

/**
 * Device circular log for throughput measurement. It records the
 * latest IOs through this device.
 */
struct cache_log_entry {
    double finish_time_ms;
    uint32_t bytes;
};

#define CACHE_LOG_SIZE (120000)
static struct cache_log_entry *cache_log;

static int cache_log_head;
static int cache_log_tail;

static env_rwlock cache_log_lock;

// Exposed for device logging.
extern double base_time_ms;

/**
 * Push a new IO entry into the log, possibly erase an oldest one if
 * the log is full. Returns the timestamp for this entry.
 */
void
cache_log_push_entry(double finish_time_ms, uint32_t bytes)
{
    int pos;

    env_rwlock_write_lock(&cache_log_lock);

    pos = (cache_log_tail + 1) % CACHE_LOG_SIZE;

    cache_log[pos].finish_time_ms = finish_time_ms;
    cache_log[pos].bytes = bytes;

    cache_log_tail = pos;
    if (cache_log_tail == cache_log_head)
        cache_log_head = (cache_log_head + 1) % CACHE_LOG_SIZE;

    if (cache_log_head < 0)
        cache_log_head = 0;

    if (DEVICE_LOG_ENABLE) {
        fprintf(fdevice, "cache req: @ %.3lf of %u\n",
                finish_time_ms - base_time_ms, bytes);
    }

    env_rwlock_write_unlock(&cache_log_lock);
}

/**
 * Query the log for throughput (KB/s) of given time interval.
 */
double
cache_log_query_throughput(double begin_time_ms, double end_time_ms)
{
    double kilobytes = 0.0;

    env_rwlock_read_lock(&cache_log_lock);

    if (cache_log_head >= 0) {
        int i = cache_log_tail + 1;

        do {
            i = i == 0 ? CACHE_LOG_SIZE - 1 : i - 1;

            if (cache_log[i].finish_time_ms <= begin_time_ms)
                break;

            if (cache_log[i].finish_time_ms <= end_time_ms)
                kilobytes += (double) cache_log[i].bytes / 1024.0;
        } while (i != cache_log_head);
    }

    env_rwlock_read_unlock(&cache_log_lock);

    return (kilobytes * 1000.0) / (end_time_ms - begin_time_ms);
}

/*========== Device log implementation END ==========*/


/**
 * Setup the cache object and attach cache device as a CACHE_VOL_TYPE
 * volume. Using the default cache config here.
 * Should be called BEFORE `core_setup`.
 */
int
cache_obj_setup(ocf_ctx_t ctx, ocf_cache_t *cache,
                enum bench_cache_mode cache_mode)
{
    struct ocf_mngt_cache_config cache_cfg = { .name = "cache" };
    struct ocf_mngt_cache_device_config device_cfg;
    cache_obj_priv_t *cache_obj_priv;
    struct cache_setup_callback_states callback_states;
    int ret;

    /** Let the callback state point to this functions return value. */
    callback_states.error = &ret;

    /**
     * Set cache configuration to default. Default config details can
     * be found in ocf_def.h. Look for the `_default` field in each
     * enum.
     * 
     * We are using memory to simulate a cache device, so we set the
     * volatile property to true.
     *
     * We are using our new Multi-factor cache mode.
     */
    ocf_mngt_cache_config_set_default(&cache_cfg);
    cache_cfg.metadata_volatile = true;
    
    if (cache_mode == BENCH_CACHE_MODE_PT)
        cache_cfg.cache_mode = ocf_cache_mode_pt;
    else if (cache_mode == BENCH_CACHE_MODE_WA)
        cache_cfg.cache_mode = ocf_cache_mode_wa;
    else if (cache_mode == BENCH_CACHE_MODE_WB)
        cache_cfg.cache_mode = ocf_cache_mode_wb;
    else if (cache_mode == BENCH_CACHE_MODE_WT)
        cache_cfg.cache_mode = ocf_cache_mode_wt;
    else if (cache_mode == BENCH_CACHE_MODE_MFWA)
        cache_cfg.cache_mode = ocf_cache_mode_mfwa;
    else if (cache_mode == BENCH_CACHE_MODE_MFWB)
        cache_cfg.cache_mode = ocf_cache_mode_mfwb;
    else if (cache_mode == BENCH_CACHE_MODE_MFWT)
        cache_cfg.cache_mode = ocf_cache_mode_mfwt;
    else
        return -1;

    /**
     * Set cache device configuration to default, and assign volume type
     * as CACHE_VOL_TYPE.
     */
    ocf_mngt_cache_device_config_set_default(&device_cfg);
    device_cfg.cache_line_size = ocf_cache_line_size_4;
    device_cfg.volume_type = CACHE_VOL_TYPE;
    device_cfg.perform_test = false;
    ret = ocf_uuid_set_str(&device_cfg.uuid, "cache");
    if (ret)
        return ret;

    /** Allocate cache object private data. */
    cache_obj_priv = malloc(sizeof(cache_obj_priv_t));
    if (cache_obj_priv == NULL)
        return -ENOMEM;

    /** Start the cache. */
    ret = ocf_mngt_cache_start(ctx, cache, &cache_cfg);
    if (ret) {
        free(cache_obj_priv);
        return ret;
    }

    /* Assign cache private structure to cache object. */
    ocf_cache_set_priv(*cache, cache_obj_priv);

    /**
     * Create management queue, used for asynchronous management
     * oprations, such as attaching volume or adding core object.
     */
    ret = ocf_queue_create(*cache, &cache_obj_priv->mngt_queue,
                           &queue_ops);
    if (ret) {
        ocf_mngt_cache_stop(*cache, cache_setup_callback,
                            &callback_states);
        free(cache_obj_priv);
        return ret;
    }

    /** Assign management queue to cache. */
    ocf_mngt_cache_set_mngt_queue(*cache, cache_obj_priv->mngt_queue);

    /** Create I/O queue, used for I/O submission. */
    ret = ocf_queue_create(*cache, &cache_obj_priv->io_queue,
                           &queue_ops);
    if (ret) {
        ocf_mngt_cache_stop(*cache, cache_setup_callback,
                            &callback_states);
        ocf_queue_put(cache_obj_priv->mngt_queue);
        free(cache_obj_priv);
        return ret;
    }

    /** Attach the cache volume to cache object. */
    ocf_mngt_cache_attach(*cache, &device_cfg, cache_setup_callback,
                          &callback_states);
    if (ret) {
        ocf_mngt_cache_stop(*cache, cache_setup_callback,
                            &callback_states);
        ocf_queue_put(cache_obj_priv->mngt_queue);
        free(cache_obj_priv);
        return ret;
    }

    /** Setup device log. */
    cache_log = malloc(sizeof(struct cache_log_entry) * CACHE_LOG_SIZE);

    cache_log_head = -1;
    cache_log_tail = -1;

    env_rwlock_init(&cache_log_lock);

    DEBUG("SETUP: done");

    return 0;
}

/**
 * Stop the cache.
 * Should be called AFTER `core_stop`.
 */
int
cache_obj_stop(ocf_cache_t cache)
{
    cache_obj_priv_t *cache_obj_priv;
    struct cache_stop_callback_states callback_states;
    int ret = 0;

    /** Let the callback state point to this functions return value. */
    callback_states.error = &ret;

    ocf_mngt_cache_stop(cache, cache_stop_callback,
                        &callback_states);
    if (ret)
        return ret;

    cache_obj_priv = ocf_cache_get_priv(cache);
    ocf_queue_put(cache_obj_priv->mngt_queue);
    free(cache_obj_priv);

    /** Free device log. */
    env_rwlock_destroy(&cache_log_lock);

    free(cache_log);

    DEBUG("STOP: done");

    return 0;
}
