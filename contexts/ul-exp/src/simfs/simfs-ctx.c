/**
 * Defines the `simfs` context-specific operations to fulfill OCF
 * interface requirements.
 * 
 * The interface type `ctx_data_t` in OCF is supposed to describe the
 * OS data buffer (occupying several data pages) for a piece of data
 * being I/Oed.
 *
 * It is typedefed as `void` in OCF interface. We cast it to our context-
 * specific `simfs_data_t` to hook up with OCF interface.
 */


#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <execinfo.h>
#include <ocf/ocf.h>
#include "ocf_env.h"

#include "common.h"
#include "simfs-ctx.h"


/**
 * Calculate valid size not exceeding allocated size of buffer, starting
 * from current offset.
 * Used as a subroutine in several operations below.
 */
static uint32_t
valid_size_from_offset(simfs_data_t *data, uint32_t size)
{
    uint32_t available_size, valid_size;

    available_size = data->pages * PAGE_SIZE - data->offset;
    valid_size = available_size < size ? available_size : size;

    return valid_size;
}

/**
 * Calculate valid size not exceeding allocated size of buffer, starting
 * from current offset.
 * Used as a subroutine in several operations below.
 */
static uint32_t
valid_size_from_begin(simfs_data_t *data, uint32_t size)
{
    uint32_t available_size, valid_size;

    available_size = data->pages * PAGE_SIZE;
    valid_size = available_size < size ? available_size : size;

    return valid_size;
}


/*========== OCF Context Operations Implemention BEGIN. ==========*/

/**
 * Allocate the OS data structure for an I/O, occupying specified number
 * of pages.
 * 
 * This function is not set to static as we might use it in the workload
 * code in main.c.
 */
ctx_data_t *
simfs_data_alloc(uint32_t pages)
{
    simfs_data_t *data;

    data = malloc(sizeof(simfs_data_t));

    data->ptr = malloc(pages * PAGE_SIZE);
    data->offset = 0;
    data->pages = pages;

    data->served = true;    // Only set to false on user-issued data.

    memset(data->ptr, 0, pages * PAGE_SIZE);

    return data;
}

/**
 * Free the OS data structure.
 */
void
simfs_data_free(ctx_data_t *simfs_data)
{
    simfs_data_t *data = simfs_data;

    if (data != NULL) {
        free(data->ptr);
        free(data);
    }
}

/**
 * Supposed to set protection of data pages against swapping.
 * Can be unimplemented if not needed.
 */
static int
simfs_data_mlock(ctx_data_t *simfs_data)
{
    return 0;   /** Unimplemented. */
}

/**
 * Stop protecting data pages against swapping.
 * Can be unimplemented if not needed.
 */
static void
simfs_data_munlock(ctx_data_t *simfs_data)
{
    return;     /** Unimplemented. */
}

/**
 * Read data from OS data buffer into destination app location.
 */
static uint32_t
simfs_data_read(void *dst, ctx_data_t *simfs_data, uint32_t size)
{
    uint32_t read_size;

    simfs_data_t *data = simfs_data;

    read_size = valid_size_from_offset(data, size);
    memcpy(dst, data->ptr + data->offset, read_size);

    return read_size;
}

/**
 * Write data from source app location into OS data buffer.
 */
static uint32_t
simfs_data_write(ctx_data_t *simfs_data, const void *src, uint32_t size)
{
    uint32_t write_size;

    simfs_data_t *data = simfs_data;

    write_size = valid_size_from_offset(data, size);
    memcpy(data->ptr + data->offset, src, write_size);

    return write_size;
}

/**
 * Fill data buffer with zeros.
 */
static uint32_t
simfs_data_zero(ctx_data_t *simfs_data, uint32_t size)
{
    uint32_t zero_size;

    simfs_data_t *data = simfs_data;

    zero_size = valid_size_from_offset(data, size);
    memset(data->ptr + data->offset, 0, zero_size);

    return zero_size;
}

/**
 * Seek on data buffer, changing the offset.
 */
static uint32_t
simfs_data_seek(ctx_data_t *simfs_data, ctx_data_seek_t seek,
                 uint32_t size)
{
    uint32_t seek_size;

    simfs_data_t *data = simfs_data;

    switch (seek) {
    case ctx_data_seek_begin:
        seek_size = valid_size_from_begin(data, size);
        data->offset = seek_size;
        break;
    case ctx_data_seek_current:
        seek_size = valid_size_from_offset(data, size);
        data->offset += seek_size;
        break;
    default:
        seek_size = 0;
    }

    return seek_size;
}

/**
 * Copy from one data buffer to another. NOT performing size checks.
 */
static uint64_t
simfs_data_copy(ctx_data_t *simfs_data_dst, ctx_data_t *simfs_data_src,
              uint64_t dst_offset, uint64_t src_offset, uint64_t bytes)
{
    simfs_data_t *data_dst = simfs_data_dst;
    simfs_data_t *data_src = simfs_data_src;

    memcpy(data_dst->ptr + dst_offset, data_src->ptr + src_offset, bytes);

    return bytes;
}

/**
 * Supposed to perform secure erase of data (e.g., fill with zeros).
 * Can be unimplemented if not needed.
 */
static void
simfs_data_secure_erase(ctx_data_t *simfs_data)
{
    return;
}

/**
 * Initialize cleaner thread.
 * Cleaning refers to the background flushing process running by the
 * cache system automatically, controlled by a cleaning policy.
 */
static int
simfs_cleaner_init(ocf_cleaner_t cleaner)
{
    return 0;   /** Unimplemented. */
}

/**
 * Kick off cleaner thread.
 */
static void
simfs_cleaner_kick(ocf_cleaner_t cleaner)
{
    return;     /** Unimplemented. */
}

/**
 * Stop cleaner thread.
 */
static void
simfs_cleaner_stop(ocf_cleaner_t cleaner)
{
    return;     /** Unimplemented. */
}

/**
 * Initialize metadata updater thread. 
 */
static int
simfs_metadata_updater_init(ocf_metadata_updater_t metadata_updater)
{
    return 0;   /** Unimplemented. */
}

/**
 * Kick off metadata updater thread.
 */
static void
simfs_metadata_updater_kick(ocf_metadata_updater_t metadata_updater)
{
    return;     /** Unimplemented. */
}

/**
 * Stop metadata updater thread.
 */
static void
simfs_metadata_updater_stop(ocf_metadata_updater_t metadata_updater)
{
    return;     /** Unimplemented. */
}

/**
 * Provide interface for printing to log use by OCF internal functions.
 * The lower level, the more urgent.
 */
static int
simfs_logger_print(ocf_logger_t logger, ocf_logger_lvl_t lvl,
                 const char *fmt, va_list args)
{
    FILE *logfile;

    if (lvl > log_info)
        return 0;
    else if (!OCF_LOGGER_INFO_MSG && lvl == log_info)
        return 0;

    logfile = lvl <= log_warn ? stderr : stdout;

    return vfprintf(logfile, fmt, args);
}

#define STACK_TRACE_DEPTH (16)    /** Backtracing stack depth. */

/**
 * Provide interface for printing current stack trace. Copied from the
 * `simple` example.
 */
static int
simfs_logger_dump_stack(ocf_logger_t logger)
{
    void *trace[STACK_TRACE_DEPTH];
    char **messages = NULL;
    int i, size;

    size = backtrace(trace, STACK_TRACE_DEPTH);
    messages = backtrace_symbols(trace, size);

    printf("[stack trace]>>>\n");
    for (i = 0; i < size; ++i)
        printf("%s\n", messages[i]);
    printf("<<<[stack trace]\n");
    
    free(messages);

    return 0;
}


/**
 * This structure assigns the above implementations to the OCF interface.
 * Interface functions are splitted into four categories.
 */
static const struct ocf_ctx_config simfs_ctx_cfg = {
    .name = "Linux FS Context",

    .ops = {
        .data = {
            .alloc = simfs_data_alloc,
            .free = simfs_data_free,
            .mlock = simfs_data_mlock,
            .munlock = simfs_data_munlock,
            .read = simfs_data_read,
            .write = simfs_data_write,
            .zero = simfs_data_zero,
            .seek = simfs_data_seek,
            .copy = simfs_data_copy,
            .secure_erase = simfs_data_secure_erase,
        },

        .cleaner = {
            .init = simfs_cleaner_init,
            .kick = simfs_cleaner_kick,
            .stop = simfs_cleaner_stop,
        },

        .metadata_updater = {
            .init = simfs_metadata_updater_init,
            .kick = simfs_metadata_updater_kick,
            .stop = simfs_metadata_updater_stop,
        },

        .logger = {
            .print = simfs_logger_print,
            .dump_stack = simfs_logger_dump_stack,
        },
    },
};

/*========== OCF Context Operations Implemention END. ==========*/


/**
 * Initialize the `simfs` context, assigning the above operation
 * implementations to the OCF interface.
 */
int
simfs_ctx_init(ocf_ctx_t *ctx)
{
    int ret;

    ret = ocf_ctx_create(ctx, &simfs_ctx_cfg);
    if (ret)
        return ret;

    DEBUG("INIT: done");

    return 0;
}

/**
 * Clean up the context.
 */
void
simfs_ctx_cleanup(ocf_ctx_t ctx)
{
    ocf_ctx_put(ctx);

    DEBUG("CLEANUP: done");
}
