/**
 * Simulation context of using the OCF library.
 * 
 * Upwards: simulating a simple paged FS application context `simfs`.
 * 
 * Downwards: using FlashSim to simulate two SSD drives.
 */


#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <ocf/ocf.h>


/** Options. */
const bool CTX_PRINT_DEBUG_MSG = false;
const bool OCF_LOGGER_INFO_MSG = false;

const bool DEVICE_LOG_ENABLE  = false;
const bool MONITOR_LOG_ENABLE = false;


/** Global parameters. */
const char *cache_sock_name = "cache-sock";
const char *core_sock_name  = "core-sock";

uint64_t cache_capacity_bytes = 0;
uint64_t core_capacity_bytes  = 0;

bool flashsim_enable_data;
unsigned long flashsim_page_size = 4096;

FILE *fdevice  = NULL;
FILE *fmonitor = NULL;


#include "simfs/simfs-ctx.h"
#include "cache/cache-vol.h"
#include "cache/cache-obj.h"
#include "core/core-vol.h"
#include "core/core-obj.h"
#include "fuzzy/fuzzy-test.h"
#include "common.h"


static void
error(const char *msg, int error)
{
    fprintf(stderr, "ERROR: %s, code = %d\n", msg, error);
    exit(1);
}


/**
 * Enumerate of possible benchmarking experiments.
 * Be sure to add into this list when a new one was implemented.
 */
typedef int (*benchmark_t) (ocf_core_t, int, char **);

#include "bench/throughput.h"

static char *bench_names[] = {
    "throughput",
    "MAX_BENCH_NAME",
};

static benchmark_t bench_funcs[] = {
    bench_throughput,
    NULL,
};


/**
 * Controlling global time in ms.
 */
static struct timespec boot_timespec;

double
get_cur_time_ms()
{
    struct timespec cur_timespec;

    clock_gettime(CLOCK_REALTIME, &cur_timespec);

    return (cur_timespec.tv_sec - boot_timespec.tv_sec) * 1000.0
           + (cur_timespec.tv_nsec - boot_timespec.tv_nsec) / 1000000.0;
}


/**
 * Display collected statistics.
 */
static void
_print_statistics(struct ocf_stats_usage *stats_usage,
                 struct ocf_stats_requests *stats_reqs,
                 struct ocf_stats_blocks *stats_blocks,
                 struct ocf_stats_errors *stats_errors)
{
    printf("\nStatistics:\n\n"
           "   usage | cache |  occupied   %8lu pages    %3lu.%02lu %%\n"
           "         |       |      free   %8lu pages    %3lu.%02lu %%\n"
           "         |       |     clean   %8lu pages    %3lu.%02lu %%\n"
           "         |       |     dirty   %8lu pages    %3lu.%02lu %%\n"
           "\n"
           "  blocks | cache |   read ->   %8lu pages    %3lu.%02lu %%\n"
           "         |       |  write <-   %8lu pages    %3lu.%02lu %%\n"
           "         |       |     total   %8lu pages    %3lu.%02lu %%\n"
           "         |  core |   read ->   %8lu pages    %3lu.%02lu %%\n"
           "         |       |  write <-   %8lu pages    %3lu.%02lu %%\n"
           "         |       |     total   %8lu pages    %3lu.%02lu %%\n"
           "         |     total           %8lu pages    %3lu.%02lu %%\n"
           "\n"
           "    reqs |  read |     hit $   %8lu reqs     %3lu.%02lu %%\n"
           "         |       | part miss   %8lu reqs     %3lu.%02lu %%\n"
           "         |       | full miss   %8lu reqs     %3lu.%02lu %%\n"
           "         |       |     total   %8lu reqs     %3lu.%02lu %%\n"
           "         | write |     hit $   %8lu reqs     %3lu.%02lu %%\n"
           "         |       | part miss   %8lu reqs     %3lu.%02lu %%\n"
           "         |       | full miss   %8lu reqs     %3lu.%02lu %%\n"
           "         |       |     total   %8lu reqs     %3lu.%02lu %%\n"
           "         |  pass |   read ->   %8lu reqs     %3lu.%02lu %%\n"
           "         |       |  write <-   %8lu reqs     %3lu.%02lu %%\n"
           "         |     total           %8lu reqs     %3lu.%02lu %%\n"
           "\n"
           "  errors | cache |   read ->   %8lu errors   %3lu.%02lu %%\n"
           "         |       |  write <-   %8lu errors   %3lu.%02lu %%\n"
           "         |  core |   read ->   %8lu errors   %3lu.%02lu %%\n"
           "         |       |  write <-   %8lu errors   %3lu.%02lu %%\n"
           "         |     total           %8lu errors   %3lu.%02lu %%\n",
           stats_usage->occupancy.value,
           stats_usage->occupancy.fraction / 100,
           stats_usage->occupancy.fraction % 100,
           stats_usage->free.value,
           stats_usage->free.fraction / 100,
           stats_usage->free.fraction % 100,
           stats_usage->clean.value,
           stats_usage->clean.fraction / 100,
           stats_usage->clean.fraction % 100,
           stats_usage->dirty.value,
           stats_usage->dirty.fraction / 100,
           stats_usage->dirty.fraction % 100,
           stats_blocks->cache_volume_rd.value,
           stats_blocks->cache_volume_rd.fraction / 100,
           stats_blocks->cache_volume_rd.fraction % 100,
           stats_blocks->cache_volume_wr.value,
           stats_blocks->cache_volume_wr.fraction / 100,
           stats_blocks->cache_volume_wr.fraction % 100,
           stats_blocks->cache_volume_total.value,
           stats_blocks->cache_volume_total.fraction / 100,
           stats_blocks->cache_volume_total.fraction % 100,
           stats_blocks->core_volume_rd.value,
           stats_blocks->core_volume_rd.fraction / 100,
           stats_blocks->core_volume_rd.fraction % 100,
           stats_blocks->core_volume_wr.value,
           stats_blocks->core_volume_wr.fraction / 100,
           stats_blocks->core_volume_wr.fraction % 100,
           stats_blocks->core_volume_total.value,
           stats_blocks->core_volume_total.fraction / 100,
           stats_blocks->core_volume_total.fraction % 100,
           stats_blocks->volume_total.value,
           stats_blocks->volume_total.fraction / 100,
           stats_blocks->volume_total.fraction % 100,
           stats_reqs->rd_hits.value,
           stats_reqs->rd_hits.fraction / 100,
           stats_reqs->rd_hits.fraction % 100,
           stats_reqs->rd_partial_misses.value,
           stats_reqs->rd_partial_misses.fraction / 100,
           stats_reqs->rd_partial_misses.fraction % 100,
           stats_reqs->rd_full_misses.value,
           stats_reqs->rd_full_misses.fraction / 100,
           stats_reqs->rd_full_misses.fraction % 100,
           stats_reqs->rd_total.value,
           stats_reqs->rd_total.fraction / 100,
           stats_reqs->rd_total.fraction % 100,
           stats_reqs->wr_hits.value,
           stats_reqs->wr_hits.fraction / 100,
           stats_reqs->wr_hits.fraction % 100,
           stats_reqs->wr_partial_misses.value,
           stats_reqs->wr_partial_misses.fraction / 100,
           stats_reqs->wr_partial_misses.fraction % 100,
           stats_reqs->wr_full_misses.value,
           stats_reqs->wr_full_misses.fraction / 100,
           stats_reqs->wr_full_misses.fraction % 100,
           stats_reqs->wr_total.value,
           stats_reqs->wr_total.fraction / 100,
           stats_reqs->wr_total.fraction % 100,
           stats_reqs->rd_pt.value,
           stats_reqs->rd_pt.fraction / 100,
           stats_reqs->rd_pt.fraction % 100,
           stats_reqs->wr_pt.value,
           stats_reqs->wr_pt.fraction / 100,
           stats_reqs->wr_pt.fraction % 100,
           stats_reqs->total.value,
           stats_reqs->total.fraction / 100,
           stats_reqs->total.fraction % 100,
           stats_errors->cache_volume_rd.value,
           stats_errors->cache_volume_rd.fraction / 100,
           stats_errors->cache_volume_rd.fraction % 100,
           stats_errors->cache_volume_wr.value,
           stats_errors->cache_volume_wr.fraction / 100,
           stats_errors->cache_volume_wr.fraction % 100,
           stats_errors->core_volume_rd.value,
           stats_errors->core_volume_rd.fraction / 100,
           stats_errors->core_volume_rd.fraction % 100,
           stats_errors->core_volume_wr.value,
           stats_errors->core_volume_wr.fraction / 100,
           stats_errors->core_volume_wr.fraction % 100,
           stats_errors->total.value,
           stats_errors->total.fraction / 100,
           stats_errors->total.fraction % 100);
}


/**
 * Read cache and core device config files.
 */
static void
_read_cache_device_config()
{
    char *line = NULL;
    size_t len = 0;
    ssize_t rlen = 0;

    FILE *fcache = fopen("cache-ssd.conf", "r");
    if (fcache == NULL)
        error("Cannot open `cache-ssd.conf`", 2);

    uint64_t flash_size = 0, package_size = 0, die_size = 0, plane_size = 0,
             block_size = 0, page_size = 0;

    while ((rlen = getline(&line, &len, fcache)) != -1) {
        if (rlen > 8 && (! strncmp(line, "SSD_SIZE", 8)))
            sscanf(line, "SSD_SIZE %lu\n", &flash_size);
        else if (rlen > 12 && (! strncmp(line, "PACKAGE_SIZE", 12)))
            sscanf(line, "PACKAGE_SIZE %lu\n", &package_size);
        else if (rlen > 8 && (! strncmp(line, "DIE_SIZE", 8)))
            sscanf(line, "DIE_SIZE %lu\n", &die_size);
        else if (rlen > 10 && (! strncmp(line, "PLANE_SIZE", 10)))
            sscanf(line, "PLANE_SIZE %lu\n", &plane_size);
        else if (rlen > 10 && (! strncmp(line, "BLOCK_SIZE", 10)))
            sscanf(line, "BLOCK_SIZE %lu\n", &block_size);
        else if (rlen > 9 && (! strncmp(line, "PAGE_SIZE", 9)))
            sscanf(line, "PAGE_SIZE %lu\n", &page_size);
    }

    cache_capacity_bytes = flash_size * package_size * die_size
                           * plane_size * block_size * page_size;
    cache_capacity_bytes = (uint64_t) (cache_capacity_bytes
                                       * 0.125);    /** Only use 1/8. */
    if (cache_capacity_bytes <= 0)
        error("Invalid cache SSD capacity", 2);
    printf("  Cache 1/8 capacity: %ld bytes\n", cache_capacity_bytes);
}

static void
_read_core_device_config()
{
    char *line = NULL;
    size_t len = 0;
    ssize_t rlen = 0;

    FILE *fcore = fopen("core-ssd.conf", "r");
    if (fcore == NULL)
        error("Cannot open `core-ssd.conf`", 3);

    uint64_t flash_size = 0, package_size = 0, die_size = 0, plane_size = 0,
             block_size = 0, page_size = 0;

    while ((rlen = getline(&line, &len, fcore)) != -1) {
        if (rlen > 8 && (! strncmp(line, "SSD_SIZE", 8)))
            sscanf(line, "SSD_SIZE %lu\n", &flash_size);
        else if (rlen > 12 && (! strncmp(line, "PACKAGE_SIZE", 12)))
            sscanf(line, "PACKAGE_SIZE %lu\n", &package_size);
        else if (rlen > 8 && (! strncmp(line, "DIE_SIZE", 8)))
            sscanf(line, "DIE_SIZE %lu\n", &die_size);
        else if (rlen > 10 && (! strncmp(line, "PLANE_SIZE", 10)))
            sscanf(line, "PLANE_SIZE %lu\n", &plane_size);
        else if (rlen > 10 && (! strncmp(line, "BLOCK_SIZE", 10)))
            sscanf(line, "BLOCK_SIZE %lu\n", &block_size);
        else if (rlen > 9 && (! strncmp(line, "PAGE_SIZE", 9)))
            sscanf(line, "PAGE_SIZE %lu\n", &page_size);
        else if (rlen > 16 && (! strncmp(line, "PAGE_ENABLE_DATA", 16))) {
            int tmp;
            sscanf(line, "PAGE_ENABLE_DATA %d\n", &tmp);
            flashsim_enable_data = (tmp == 1);
        }
    }

    if (page_size <= 0)
        error("Invalid FlashSim page size", 3);
    flashsim_page_size = page_size;

    core_capacity_bytes = flash_size * package_size * die_size
                          * plane_size * block_size * page_size;
    core_capacity_bytes = (uint64_t) (core_capacity_bytes
                                      * 0.125);     /** Only use 1/8. */
    if (core_capacity_bytes <= 0)
        error("Invalid core SSD capacity", 3);
    printf("  Core 1/8 capacity: %ld bytes\n", core_capacity_bytes);

    printf("  FlashSim page size: %ld bytes\n", flashsim_page_size);
    printf("  FlashSim enable data: %s\n", flashsim_enable_data ? "true"
                                                                : "false");
}


/**
 * Prompt usage and exit with error.
 */
static inline void
prompt_usage_exit()
{
    fprintf(stderr, "Usage:\n"
                    "  1) ./bench <mode> fuzzy                    "
                    "  # For fuzzy testing\n"
                    "  2) ./bench <mode> <bench_name> [bench_args]"
                    "  # For benchmarking\n"
                    "Where:\n"
                    "  mode := pt|wa|wb|wt|mfwa|mfwb|mfwt\n"
                    "  bench_name & bench_args are defined by benchmarks\n");
    exit(1);
}


/**
 * Unified entrance for doing benchmarking.
 */
static int
perform_workload_bench(ocf_core_t core, char *bench_name, int num_args,
                       char **bench_args)
{
    int i = 0;
    benchmark_t bench_func = NULL;

    while (strcmp(bench_name, bench_names[i])) {
        if (! strcmp(bench_names[i], "MAX_BENCH_NAME")) {
            fprintf(stderr, "Cannot find benchmark handle for \'%s\'\n",
                    bench_name);
            prompt_usage_exit();
        }

        ++i;
    }

    bench_func = bench_funcs[i];

    return bench_func(core, num_args, bench_args);
}


/**
 * Main entrance for a round of testing.
 */
int
main(int argc, char *argv[])
{
    ocf_ctx_t ctx;
    ocf_cache_t cache;
    ocf_core_t core;
    struct ocf_stats_usage stats_usage;
    struct ocf_stats_requests stats_reqs;
    struct ocf_stats_blocks stats_blocks;
    struct ocf_stats_errors stats_errors;

    bool fuzzy_testing = false;
    enum bench_cache_mode cache_mode;
    int num_args = -1, ret, i;
    char *bench_name = NULL;
    char **bench_args = NULL;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /** 0. Setting up... */
    printf("\nMain setup parameters:\n\n");

    /** FlashSim sockets. */
    cache_sock_name = "cache-sock";
    core_sock_name  = "core-sock";

    /** Get cache mode and arguments for this round of experiment. */
    if (argc < 3)
        prompt_usage_exit();

    if (! strncmp(argv[1], "pt", 2))
        cache_mode = BENCH_CACHE_MODE_PT;
    else if (! strncmp(argv[1], "wa", 2))
        cache_mode = BENCH_CACHE_MODE_WA;
    else if (! strncmp(argv[1], "wb", 2))
        cache_mode = BENCH_CACHE_MODE_WB;
    else if (! strncmp(argv[1], "wt", 2))
        cache_mode = BENCH_CACHE_MODE_WT;
    else if (! strncmp(argv[1], "mfwa", 4))
        cache_mode = BENCH_CACHE_MODE_MFWA;
    else if (! strncmp(argv[1], "mfwb", 4))
        cache_mode = BENCH_CACHE_MODE_MFWB;
    else if (! strncmp(argv[1], "mfwt", 4))
        cache_mode = BENCH_CACHE_MODE_MFWT;
    else
        prompt_usage_exit();
    printf("  Using cache mode: %s\n", argv[1]);

    if (! strncmp(argv[2], "fuzzy", 5)) {
        fuzzy_testing = true;
    } else {
        fuzzy_testing = false;
        bench_name = argv[2];

        num_args = argc - 3;
        if (num_args > 0) {
            bench_args = malloc(sizeof(char *) * num_args);
            for (i = 0; i < num_args; ++i)
                bench_args[i] = argv[i + 3];
        }
    }

    /** Record boot timespec. */
    clock_gettime(CLOCK_REALTIME, &boot_timespec);

    /** Read device config files. */
    _read_cache_device_config();
    _read_core_device_config();

    ENV_BUG_ON(flashsim_page_size != PAGE_SIZE);

    /** Random seed. */
    srand(time(NULL));

    /** Logging locations. */
    fdevice  = fopen("logs/log-device.txt" , "w+");
    fmonitor = fopen("logs/log-monitor.txt", "w+");

    /** 1. Initialize OCF context. */
    ret = simfs_ctx_init(&ctx);
    if (ret)
        error("Unable to initialize app context", ret);

    /** 2. Register two volume types. */
    ret = cache_vol_register(ctx);
    if (ret)
        error("Unable to register cache volume type", ret);

    ret = core_vol_register(ctx);
    if (ret)
        error("Unable to register core volume type", ret);

    /** 3. Setup cache object. */
    ret = cache_obj_setup(ctx, &cache, cache_mode);
    if (ret)
        error("Unable to initialize cache", ret);

    /** 4. Setup core object. */
    ret = core_obj_setup(cache, &core);
    if (ret)
        error("Unable to initialize core", ret);

    /** 5. Init and start the monitor. */
    if (cache_mode == BENCH_CACHE_MODE_MFWA
        || cache_mode == BENCH_CACHE_MODE_MFWB
        || cache_mode == BENCH_CACHE_MODE_MFWT) {
        ret = ocf_mngt_mf_monitor_init(core);
        if (ret)
            error("Unable to start monitor thread", ret);
    }

    /** 6. Perform workload. */
    if (fuzzy_testing)
        ret = perform_workload_fuzzy(core, 30000);
    else
        ret = perform_workload_bench(core, bench_name, num_args, bench_args);
    if (ret)
      error("Error when performing workload", ret);

    /** 7. Collect & show statistics. */
    ocf_stats_collect_core(core, &stats_usage, &stats_reqs,
                           &stats_blocks, &stats_errors);
    _print_statistics(&stats_usage, &stats_reqs,
                      &stats_blocks, &stats_errors);

    /** 8. Stop the multi-factor monitor. */
    if (cache_mode == BENCH_CACHE_MODE_MFWA
        || cache_mode == BENCH_CACHE_MODE_MFWB
        || cache_mode == BENCH_CACHE_MODE_MFWT)
        ocf_mngt_mf_monitor_stop();

    /** 9. Force device volume submission threads to stop. */
    cache_vol_force_stop();
    core_vol_force_stop();

    /** 10. Stop and detach core from cache. */
    // ret = core_obj_stop(core);
    // if (ret)
    //     error("Unable to stop core", ret);

    /** 11. Stop the cache. */
    // ret = cache_obj_stop(cache);
    // if (ret)
    //     error("Unable to stop cache", ret);

    /** 12. Unregister volume types. */
    // core_vol_unregister(ctx);
    // cache_vol_unregister(ctx);

    /** 13. Cleanup this context. */
    // simfs_ctx_cleanup(ctx);

    fclose(fdevice);
    fclose(fmonitor);

    return 0;
}
