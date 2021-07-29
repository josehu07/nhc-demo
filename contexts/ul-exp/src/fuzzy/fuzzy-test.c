/**
 * Fuzzy test.
 *
 * Performs random reads & writes and checks correctness. Assumes callbacks
 * are sequential, in-order, & synchronous.
 */


#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <semaphore.h>
#include <ocf/ocf.h>

#include "simfs/simfs-ctx.h"
#include "cache/cache-obj.h"
#include "core/core-vol.h"
#include "common.h"
#include "fuzzy-test.h"


/** Absolute buffer holding data we expect. */
static char *abs_buf;

/** Bitmap of which pages have been written at least once. */
static uint8_t *bit_map;

/** Counters. */
static int total_reads_count = 0;
static int valid_reads_count = 0;

static int total_num_ios = 0;


/**
 * A double-linked list for remembering what data a read is expected
 * to get from device. For verification purpose.
 */
struct expected_page {
    struct list_head head;
    int idx;
    struct ocf_io *io;
    char data[PAGE_SIZE];
};

static struct expected_page expects;
static env_mutex expects_mutex;


/**
 * These are to ensure that all requests are processed before the `perform_`
 * func returns.
 */
static int finished_requests = 0;
static env_mutex finished_requests_mutex;
static env_completion finished_requests_sem;


/**
 * Do the write IO to absolute buffer.
 */
static void
write_to_absolute(struct ocf_io *io)
{
    simfs_data_t *data = ocf_io_get_data(io);

    memcpy(abs_buf + io->addr, data->ptr + data->offset, io->bytes);
}

/**
 * Remember expected page data for a read IO. Adds an entry to the
 * `expects` list.
 */
static void
record_expected_page(struct ocf_io *io, int idx)
{
    struct expected_page *expect = malloc(sizeof(struct expected_page));
    
    expect->idx = idx;
    expect->io = io;
    memcpy(expect->data, abs_buf + io->addr, io->bytes);

    env_mutex_lock(&expects_mutex);
    list_add(&expect->head, &expects.head);
    env_mutex_unlock(&expects_mutex);
}

/**
 * Compare expected with result data read from device, then free
 * the recorded expected page data.
 */
static void
validate_read_result(struct ocf_io *io)
{
    struct expected_page *expect;
    struct list_head *iter;
    simfs_data_t *data = ocf_io_get_data(io);

    /** Locate the expected page record for this IO. */
    env_mutex_lock(&expects_mutex);
    list_for_each(iter, &expects.head) {
        expect = list_entry(iter, struct expected_page, head);

        if (expect->io == io) {
            list_del(&expect->head);
            env_mutex_unlock(&expects_mutex);

            total_reads_count++;

            if (memcmp(expect->data, data->ptr + data->offset,
                       io->bytes) != 0) {
                fprintf(stderr, " !! IO #%d: addr = 0x%08lx, len = %u\n"
                                "    Expect: %.14s\n"
                                "    Actual: %.14s\n",
                        expect->idx, io->addr, io->bytes, expect->data,
                        (char *) data->ptr + data->offset);
            } else
                valid_reads_count++;

            free(expect);
            return;
        }
    }

    env_mutex_unlock(&expects_mutex);
    fprintf(stderr, " !! IO: core pos = 0x%08lx, len = %u\n"
                    "    Unable to find expected page record\n",
            io->addr, io->bytes);
}


/**
 * Callback functions to be called when operation completes.
 */
static void
write_cmpl_callback(struct ocf_io *io, int error)
{
    simfs_data_t *data = ocf_io_get_data(io);

    if (error == 0) {
        DEBUG(" ~W addr = 0x%08lx, len = %u, data = %.14s",
              io->addr, io->bytes, (char *) data->ptr + data->offset);
    } else
        DEBUG("WR COMPLETE: error = %d", error);

    /** Free data buffer and I/O structure in callback. */
    simfs_data_free(data);
    ocf_io_put(io);

    /** Increment number of finished requests. */
    env_mutex_lock(&finished_requests_mutex);
    
    finished_requests++;
    if (finished_requests == total_num_ios)
        env_completion_complete(&finished_requests_sem);

    env_mutex_unlock(&finished_requests_mutex);
}

static void
read_cmpl_callback(struct ocf_io *io, int error)
{
    simfs_data_t *data = ocf_io_get_data(io);

    /** Do verification on completed reads. */
    if (error == 0) {
        DEBUG(" ~R addr = 0x%08lx, len = %u, data = %.14s",
              io->addr, io->bytes, (char *) data->ptr + data->offset);

        validate_read_result(io);
    } else 
        DEBUG("RD COMPLETE: error = %d", error);

    /** Free data buffer and I/O structure in callback. */
    simfs_data_free(data);
    ocf_io_put(io);

    /** Increment number of finished requests. */
    env_mutex_lock(&finished_requests_mutex);

    finished_requests++;
    if (finished_requests == total_num_ios)
        env_completion_complete(&finished_requests_sem);

    env_mutex_unlock(&finished_requests_mutex);
}


/**
 * Wrapper function for I/O submission.
 */
static int
submit_io(ocf_core_t core, simfs_data_t *simfs_data, uint64_t addr,
          uint32_t len, int dir, ocf_end_io_t callback_func, int idx)
{
    ocf_cache_t cache = ocf_core_get_cache(core);
    cache_obj_priv_t *cache_obj_priv = ocf_cache_get_priv(cache);
    struct ocf_io* io;

    /** Allocate new I/O in queue. */
    io = ocf_core_new_io(core, cache_obj_priv->io_queue, addr,
                         len, dir, 0, 0);
    if (io == NULL)
        return -ENOMEM;

    /** Assign data to I/O. */
    ocf_io_set_data(io, simfs_data, 0);

    /** Assign completion callback function. */
    ocf_io_set_cmpl(io, NULL, NULL, callback_func);

    /** For correctness verification. */
    if (io->dir == OCF_WRITE)
        write_to_absolute(io);
    else
        record_expected_page(io, idx);

    /** Submit this I/O. */
    ocf_core_submit_io(io);

    return 0;
}


/**
 * Perform fuzzy test for a given number of IOs.
 */
int
perform_workload_fuzzy(ocf_core_t core, int num_ios)
{
    int i, ret;

    printf("\nDoing fuzzy testing...\n");

    /** Must have ENABLE_DATA == true when doing this fuzzy testing. */
    if (! flashsim_enable_data) {
        fprintf(stderr, "Fuzzy testing requires PAGE_ENABLE_DATA option "
                        "on.\n");
        return -1;
    }

    /** Must do at least 12000 IOs. */
    if (num_ios < 12000) {
        fprintf(stderr, "Fuzzy testing requires at least 12000 IOs.\n");
        return -2;
    }

    /** Initialize the expected pages list for read verification. */
    INIT_LIST_HEAD(&expects.head);

    ret = env_mutex_init(&expects_mutex);
    if (ret)
        return ret;

    /** Initialize counters. */
    total_num_ios = num_ios;

    total_reads_count = 0;
    valid_reads_count = 0;

    finished_requests = 0;

    ret = env_mutex_init(&finished_requests_mutex);
    if (ret)
        return ret;

    env_completion_init(&finished_requests_sem);

    /**
     * Allocate absolute buffer to hold the data we expect, same size
     * as the backend core device.
     */
    abs_buf = malloc(core_capacity_bytes);
    if (abs_buf == NULL)
        return -ENOMEM;

    memset(abs_buf, 0, core_capacity_bytes);

    bit_map = malloc((core_capacity_bytes / PAGE_SIZE) / 8);
    if (bit_map == NULL)
        return -ENOMEM;

    memset(bit_map, 0, (core_capacity_bytes / PAGE_SIZE) / 8);

    printf("\nIssuing IO requests...\n\n");

    /** Loop and perform random IOs. */
    for (i = 0; i < num_ios; ++i) {
        int j, k, dir;
        uint64_t page_no, addr;
        uint32_t size = PAGE_SIZE;
        simfs_data_t *data = simfs_data_alloc(1);

        if (data == NULL)
            return -ENOMEM;

        /**
         * First 10000 IOs are writes (to somewhat fill the device).
         * Subsequent IOs are ~20% writes and ~80% reads.
         */
        if (i < 10000)
            dir = OCF_WRITE;
        else
            dir = (rand() % 10) < 2 ? OCF_WRITE : OCF_READ;

        /** Write: Put in ID + random alpha char data in each sector. */
        if (dir == OCF_WRITE) {
            page_no = rand() % (core_capacity_bytes / PAGE_SIZE);
            addr = page_no * PAGE_SIZE;

            for (j = 0; j < size / 512; ++j) {
                char id[20];
                char *sec_base = (char *) (data->ptr + data->offset)
                                 + (j * 512);

                snprintf(id, 20, "%08d-%03d", i, j);

                *(sec_base + 0) = '<';
                memcpy(sec_base + 1, id, 12);
                *(sec_base + 13) = '>';

                for (k = 14; k < 511; ++k)
                    *(sec_base + k) = (rand() % 26) + 97;

                *(sec_base + 511) = '\0';
            }

            ret = submit_io(core, data, addr, size, dir,
                            write_cmpl_callback, i);
            if (ret)
                return ret;

            bit_map[page_no / 8] |= 1 << (page_no % 8);

        /** Read: only choose from written pages. */
        } else {
            do {
                page_no = rand() % (core_capacity_bytes / PAGE_SIZE);
            } while ((bit_map[page_no / 8] & (1 << (page_no % 8))) == 0);
            addr = page_no * PAGE_SIZE;

            ret = submit_io(core, data, addr, size, dir,
                            read_cmpl_callback, i);
            if (ret)
                return ret;
        }

        DEBUG("Perform IO #%d: %s - 0x%08lx", i,
              dir == OCF_WRITE ? "WR" : "RD", addr);
        if ((i + 1) % 1000 == 0)
            printf("  ... # %d / %d IOs\n", i + 1, num_ios);

        usleep(500);    // 1 request per 0.5 ms.
    }

    /** Wait until all pending requests have been processed. */
    printf("  Waiting for verifications to complete...\n");
    env_completion_wait(&finished_requests_sem);

    /** Check whether 100% of the reads are consistent. */
    ENV_BUG_ON(total_reads_count <= 0);
    ENV_BUG_ON(! list_empty(&expects.head));

    env_mutex_destroy(&expects_mutex);

    free(abs_buf);
    free(bit_map);

    printf("\nResult:\n\n");

    if (valid_reads_count != total_reads_count) {
        printf("  --- FAIL ---  %d / %d valid reads :(\n",
               valid_reads_count, total_reads_count);
        return -3; 
    } else {
        printf("  --- PASS ---  %d / %d valid reads :)\n",
               valid_reads_count, total_reads_count);
        return 0;
    }
}
