/**
 * The multi-factor caching algorithm monitor.
 *
 * Dynamically monitors and tweaks `data_admit` & `load_admit` switches
 * on the fly.
 */

/*========== [Orthus FLAG BEGIN] ==========*/

#include <stdbool.h>
#include "common.h"
#include "cache/cache-obj.h"
#include "core/core-obj.h"
#include "ocf/ocf.h"
#include "mf_monitor.h"


/** Indicates whether the contexts stops the monitor. */
static env_atomic should_stop;


/** `data_admit` switch, protected by a global rwlock. */
static bool global_data_admit = true;

/** `load_admit` switch, protected by a global rwlock. */
static double global_load_admit = 1.0;

/** Reader-writer lock to protect `data_admit`. */
static env_rwlock data_admit_lock;

/** Reader-writer lock to protect `load_admit`. */
static env_rwlock load_admit_lock;


/**
 * Set switch value with writer lock.
 */
static void
monitor_set_data_admit(bool data_admit)
{
    env_rwlock_write_lock(&data_admit_lock);
    global_data_admit = data_admit;
    env_rwlock_write_unlock(&data_admit_lock);
}

static void
monitor_set_load_admit(double load_admit)
{
    env_rwlock_write_lock(&load_admit_lock);
    global_load_admit = load_admit;
    env_rwlock_write_unlock(&load_admit_lock);
}

/**
 * For OCF mf policy to query the switch values.
 */
bool
monitor_query_data_admit()
{
    bool data_admit;

    env_rwlock_read_lock(&data_admit_lock);
    data_admit = global_data_admit;
    env_rwlock_read_unlock(&data_admit_lock);

    return data_admit;
}

double
monitor_query_load_admit()
{
    double load_admit;

    env_rwlock_read_lock(&load_admit_lock);
    load_admit = global_load_admit;
    env_rwlock_read_unlock(&load_admit_lock);

    return load_admit;
}


/*========== Multi-factor algorithm logic BEGIN ==========*/

/** Consider cache is stable if miss ratio within OLD_RATIO +- X. */
static const double WAIT_STABLE_THRESHOLD = 0.0015;

/** Sleep X microseconds when detecting cache stability. */
static const int WAIT_STABLE_SLEEP_INTERVAL_US = 100000;

/** Consider workload change when miss ratio > BASE_RATIO + X. */
static const double WORKLOAD_CHANGE_THRESHOLD = 0.2;

/** `load_admit` tuning step size. */
static const double LOAD_ADMIT_TUNING_STEP = 0.01;

/** Measure throughput for a `load_admit` value for X microseconds. */
static const int MEASURE_THROUGHPUT_INTERVAL_US = 25000;

/**
 * Query the stat component for read (partial + full) miss ratio info.
 */
static inline double
_get_miss_ratio(ocf_core_t core)
{
    if (env_atomic_read(&should_stop) != 0) {
        env_rwlock_destroy(&data_admit_lock);
        env_rwlock_destroy(&load_admit_lock);
        pthread_exit(NULL);
    }

    return ocf_core_get_read_miss_ratio(core);
}

/**
 * Query the context device object for throughput stats.
 */
static inline double
_get_throughput()
{
    double cur_time_ms = get_cur_time_ms();
    double begin_time_ms = cur_time_ms
                           - (MEASURE_THROUGHPUT_INTERVAL_US / 1000.0);

    return cache_log_query_throughput(begin_time_ms, cur_time_ms)
           + core_log_query_throughput(begin_time_ms, cur_time_ms);
}

/**
 * Wait until cache hit rate is stable. Returns the final miss ratio.
 */
static double
monitor_wait_stable(ocf_core_t core)
{
    double last_miss_ratio = -0.1;
    double miss_ratio = _get_miss_ratio(core);

    while (miss_ratio < last_miss_ratio - WAIT_STABLE_THRESHOLD
           || miss_ratio > last_miss_ratio + WAIT_STABLE_THRESHOLD) {
        usleep(WAIT_STABLE_SLEEP_INTERVAL_US);
        
        last_miss_ratio = miss_ratio;
        miss_ratio = _get_miss_ratio(core);

        if (MONITOR_LOG_ENABLE) {
            fprintf(fmonitor, "  (wait) miss ratio = %.5lf -> %.5lf\n",
                    last_miss_ratio, miss_ratio);
        }
    }

    return miss_ratio;
}

/**
 * Set `load_admit` to a value for a while and measure the throughput.
 */
static double
monitor_measure_throughput(double load_admit)
{
    monitor_set_load_admit(load_admit);
    usleep(MEASURE_THROUGHPUT_INTERVAL_US);
    return _get_throughput();
}

/**
 * Repeatedly tune `load_admit` ratio until a workload change is
 * considered happened.
 */
static void
monitor_tune_load_admit(double base_miss_ratio, ocf_core_t core)
{
    double la1, la2, la3;
    double tp1, tp2, tp3;
    bool second_chance = true;
    long long int iteration = 0;

    while (1) {
        iteration++;

        /** Get middle ratio (current `load_admit`) throughput. */
        la2 = monitor_query_load_admit();
        if (MONITOR_LOG_ENABLE && iteration % 10 == 0) {
            fprintf(fmonitor, "  (tune) iter #%lld: load_admit = %.3lf\n",
                    iteration, la2);
        }
        tp2 = monitor_measure_throughput(la2);

        /** Get higher ratio throughput. */
        la3 = la2 + LOAD_ADMIT_TUNING_STEP;
        tp3 = la3 > 1.0 ? -0.1 : monitor_measure_throughput(la3);

        /** Get lower ratio throughput. */
        la1 = la2 - LOAD_ADMIT_TUNING_STEP;
        tp1 = la1 < 0.0 ? -0.1 : monitor_measure_throughput(la1);

        monitor_set_load_admit(la2);    /** Recover. */

        /** Slope following loop. */
        while (1) {
            /**
             * Workload change check:
             * If detected workload change, quit and re-optimize.
             */
            double miss_ratio = _get_miss_ratio(core);
            if (miss_ratio > base_miss_ratio + WORKLOAD_CHANGE_THRESHOLD) {
                if (MONITOR_LOG_ENABLE)
                    fprintf(fmonitor, "  (tune) miss ratio too high, quit\n");
                return;
            }

            /**
             * Middle ratio yields best throughput, goto intensity check.
             */
            if (tp2 >= tp1 && tp2 >= tp3) {
                monitor_set_load_admit(la2);
                break;
            }

            /**
             * Higher ratio yields best throughput, then shift to higher
             * `load_admit` value.
             */
            if (tp3 >= tp1 && tp3 >= tp2) {
                if (la3 >= 1.0) {
                    monitor_set_load_admit(1.0);
                    break;
                } else {
                    la1 = la2; tp1 = tp2;
                    la2 = la3; tp2 = tp3;
                    la3 = la3 + LOAD_ADMIT_TUNING_STEP;
                    tp3 = la3 > 1.0 ? -0.1 : monitor_measure_throughput(la3);
                    continue;
                }
            }

            /**
             * Lower ratio yields best throughput, then shift to lower
             * `load_admit` value.
             */
            if (tp1 >= tp2 && tp1 >= tp3) {
                if (la1 <= 0.0) {
                    monitor_set_load_admit(0.0);
                    break;
                } else {
                    la3 = la2; tp3 = tp2;
                    la2 = la1; tp2 = tp1;
                    la1 = la1 - LOAD_ADMIT_TUNING_STEP;
                    tp1 = la1 < 0.0 ? -0.1 : monitor_measure_throughput(la1);
                    continue;
                }
            }
        }

        /**
         * Intensity check:
         * If client's request intensity cannot fill cache bandwidth, then fall
         * back to classic caching.
         */
        if (monitor_query_load_admit() == 1.0) {
            if (second_chance) {    /** Give a second chance. */
                second_chance = false;
                continue;
            } else {
                if (MONITOR_LOG_ENABLE)
                    fprintf(fmonitor, "  (tune) load_admit stays 100%%, "
                                      "quit\n");
                return;
            }
        }
    }
}

/**
 * Monitor thread logic.
 */
static void *
monitor_func(void *core_ptr)
{
    ocf_core_t core = core_ptr;

    while (1) {
        double base_miss_ratio;

        /** Start a new workload with classic caching. */
        if (MONITOR_LOG_ENABLE)
            fprintf(fmonitor, "  (fall) start classic caching\n");
        monitor_set_data_admit(true);
        monitor_set_load_admit(1.0);

        /** Wait until cache is stable. */
        base_miss_ratio = monitor_wait_stable(core);
        if (MONITOR_LOG_ENABLE)
            fprintf(fmonitor, "  (wait) cache is stable\n");

        /** Turn off `data_admit` and start `load_admit` tuning. */
        monitor_set_data_admit(false);
        if (MONITOR_LOG_ENABLE) {
            fprintf(fmonitor, "  (tune) turn off data_admit & start "
                              "tuning\n");
        }
        monitor_tune_load_admit(base_miss_ratio, core);
    }

    return NULL;
}

/*========== Multi-factor algorithm logic END ==========*/


/**
 * Setup multi-factor switches and sart the monitor thread.
 */
int
ocf_mngt_mf_monitor_init(ocf_core_t core)
{
    pthread_t monitor_thread_id;
    pthread_attr_t monitor_thread_attr;
    int ret;

    env_atomic_set(&should_stop, 0);

    global_data_admit = true;
    global_load_admit = 1.0;

    env_rwlock_init(&data_admit_lock);
    env_rwlock_init(&load_admit_lock);

    /** Monitor runs as an infinite loop, so set to detached. */
    ret = pthread_attr_init(&monitor_thread_attr);
    if (ret)
        return ret;

    ret = pthread_attr_setdetachstate(&monitor_thread_attr,
                                      PTHREAD_CREATE_DETACHED);
    if (ret)
        return ret;

    /** Create the monitor thread. */
    ret = pthread_create(&monitor_thread_id, &monitor_thread_attr,
                         monitor_func, (void *) core);
    if (ret)
        return ret;

    return 0;
}

/**
 * For the context to gracefully stop the monitor thread.
 */
void
ocf_mngt_mf_monitor_stop()
{
    env_atomic_inc(&should_stop);
}

/*========== [Orthus FLAG END] ==========*/
