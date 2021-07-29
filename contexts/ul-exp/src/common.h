/**
 * Common headers across context code.
 */


#ifndef __COMMON_H__
#define __COMMON_H__


#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>


extern FILE *fdevice;
extern FILE *fmonitor;

extern bool flashsim_enable_data;
extern unsigned long flashsim_page_size;

extern const char *cache_sock_name;
extern const char *core_sock_name;

extern uint64_t cache_capacity_bytes;
extern uint64_t core_capacity_bytes;


/**
 * Debug printing.
 */
extern const bool OCF_LOGGER_INFO_MSG;
extern const bool CTX_PRINT_DEBUG_MSG;

extern const bool DEVICE_LOG_ENABLE;
extern const bool MONITOR_LOG_ENABLE;

#define __FILEBASE__ (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 \
                                             : __FILE__)

#define DEBUG(fmt, args...) if (CTX_PRINT_DEBUG_MSG)        \
                                printf("[%s:%d] " fmt "\n", \
                                       __FILEBASE__, __LINE__, ##args)


/**
 * Get the global time in ms unit.
 */
double get_cur_time_ms();


/**
 * Enumerate of possible cache modes.
 */
enum bench_cache_mode {
    BENCH_CACHE_MODE_PT,    /** Pass-through. */
    BENCH_CACHE_MODE_WA,    /** Write-around. */
    BENCH_CACHE_MODE_WB,    /** Write-back. */
    BENCH_CACHE_MODE_WT,    /** Write-back. */
    BENCH_CACHE_MODE_MFWA,  /** Multi-factor with write-around. */
    BENCH_CACHE_MODE_MFWB,  /** Multi-factor with write-back. */
    BENCH_CACHE_MODE_MFWT,  /** Multi-factor with write-through. */
};


#endif
