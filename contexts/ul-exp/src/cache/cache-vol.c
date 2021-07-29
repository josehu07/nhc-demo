/**
 * Cache volume type implementation.
 */


#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <semaphore.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <ocf/ocf.h>

#include "simfs/simfs-ctx.h"
#include "cache-obj.h"
#include "common.h"
#include "cache-vol.h"


/** Indicates whether we should exit without finishing pending requests. */
static env_atomic should_stop;


/**
 * A double-linked list as a request submitting queue. Request submission
 * operation pushes an entry into the queue and then ACKs immediately. A
 * separate submit thread processes the queue synchronously.
 */
struct req_entry {
    struct list_head head;
    struct ocf_io *io;
    double start_time_ms;
};

static struct req_entry submit_queue;

static env_mutex submit_queue_lock;
static env_completion submit_queue_sem;


/**
 * Request header (1st message) format.
 * Message size MUST exactly match in bytes!
 */
struct __attribute__((__packed__)) req_header {
    uint32_t direction     : 32;
    uint64_t addr          : 64;
    uint32_t size          : 32;
    uint64_t start_time_us : 64;
};

static const size_t REQ_HEADER_LENGTH = 24;

static const int FLASHSIM_DIR_READ  = 0;
static const int FLASHSIM_DIR_WRITE = 1;


/**
 * Routines to read from or write to the storage device.
 */
static int
_submit_write_io(struct ocf_io *io, int sock_fd, double start_time_ms)
{
    struct req_header header;
    int rbytes, wbytes;
    uint64_t time_used_us;

    cache_vol_io_priv_t *io_priv = ocf_io_get_priv(io);
    simfs_data_t *data = io_priv->data;

    /** Request header. */
    header.direction = FLASHSIM_DIR_WRITE;
    header.addr = io->addr;
    header.size = io->bytes;
    header.start_time_us = (uint64_t) (1000 * start_time_ms);

    wbytes = write(sock_fd, &header, REQ_HEADER_LENGTH);
    if (wbytes != REQ_HEADER_LENGTH) {
        DEBUG("IO: write request header send failed");
        return 1;
    }

    /** Data to write, only if passing actual data. */
    if (flashsim_enable_data) {
        wbytes = write(sock_fd, data->ptr + data->offset + io_priv->offset,
                       header.size);
        if (wbytes != (int) header.size) {
            DEBUG("IO: write request data send failed");
            return 2;
        }
    }

    /** Processing time respond. */
    rbytes = read(sock_fd, &time_used_us, 8);
    if (rbytes != 8) {
        DEBUG("IO: write processing time recv failed");
        return 3;
    }

    /** Simulate latency here. */
    usleep(time_used_us);

    /** If haven't, record in log. */
    if (! data->served) {
        data->served = true;
        cache_log_push_entry(get_cur_time_ms(), io->bytes);

        // DEBUG(" ^W addr = 0x%08lx, len = %u, data = %.14s",
        //       io->addr, io->bytes,
        //       (char *) data->ptr + data->offset + io_priv->offset);
    }

    // DEBUG(" ^W addr = 0x%08lx, len = %u, data = %.14s",
    //       io->addr, io->bytes,
    //       (char *) data->ptr + data->offset + io_priv->offset);

    return 0;
}

static int
_submit_read_io(struct ocf_io *io, int sock_fd, double start_time_ms)
{
    struct req_header header;
    int rbytes, wbytes;
    uint64_t time_used_us;

    cache_vol_io_priv_t *io_priv = ocf_io_get_priv(io);
    simfs_data_t *data = io_priv->data;

    /** Request header. */
    header.direction = FLASHSIM_DIR_READ;
    header.addr = io->addr;
    header.size = io->bytes;
    header.start_time_us = (uint64_t) (1000 * start_time_ms);

    wbytes = write(sock_fd, &header, REQ_HEADER_LENGTH);
    if (wbytes != REQ_HEADER_LENGTH) {
        DEBUG("IO: read request header send failed");
        return 1;
    }

    /** Data read out, only if passing actual data. */
    if (flashsim_enable_data) {
        rbytes = read(sock_fd, data->ptr + data->offset + io_priv->offset,
                      header.size);
        if (rbytes != (int) header.size) {
            DEBUG("IO: read request data recv failed");
            return 2;
        }
    }

    /** Processing time respond. */
    rbytes = read(sock_fd, &time_used_us, 8);
    if (rbytes != 8) {
        DEBUG("IO: read processing time recv failed");
        return 3;
    }

    /** Simulate latency here. */
    usleep(time_used_us);

    /** If haven't, record in log. */
    if (! data->served) {
        data->served = true;
        cache_log_push_entry(get_cur_time_ms(), io->bytes);

        // DEBUG(" ^R addr = 0x%08lx, len = %u, data = %.14s",
        //       io->addr, io->bytes,
        //       (char *) data->ptr + data->offset + io_priv->offset);

        // DEBUG(" ^R time_used_us = %lu", time_used_us);
    }

    // DEBUG(" ^R addr = 0x%08lx, len = %u, data = %.14s",
    //       io->addr, io->bytes,
    //       (char *) data->ptr + data->offset + io_priv->offset);

    return 0;
}

/**
 * Submission thread runs separately. ARGS is a pointer to package id.
 */
static void *
_submit_thread_func(void *args)
{
    struct req_entry *entry;
    struct ocf_io *io;
    double start_time_ms;

    DEBUG("SUBMIT: cache submission thread launched");

    while (1) {
        /** Wait when list is empty. */
        env_completion_wait(&submit_queue_sem);

        /** Force quit. */
        if (env_atomic_read(&should_stop) != 0) {
            env_completion_complete(&submit_queue_sem);
            pthread_exit(NULL);
            return NULL;    // Not reached.
        }

        /** Extract an entry from queue head. */
        env_mutex_lock(&submit_queue_lock);

        if (list_empty(&submit_queue.head)) {
            env_mutex_unlock(&submit_queue_lock);
            continue;
        }

        entry = list_first_entry(&submit_queue.head, struct req_entry, head);
        io = entry->io;
        start_time_ms = entry->start_time_ms;

        list_del(&entry->head);
        free(entry);

        env_mutex_unlock(&submit_queue_lock);

        /** Process the request. */
        cache_vol_priv_t *vol_priv = ocf_volume_get_priv(ocf_io_get_volume(io));

        // DEBUG("IO: dir = %s, cache pos = 0x%08lx, len = %u",
        //       io->dir == OCF_WRITE ? "WR <-" : "RD ->", io->addr, io->bytes);

        switch (io->dir) {
        case OCF_WRITE:
            _submit_write_io(io, vol_priv->sock_fd, start_time_ms);
            break;
        case OCF_READ:
            _submit_read_io(io, vol_priv->sock_fd, start_time_ms);
            break;
        }

        io->end(io, 0);
    }

    // Not reached.
    return NULL;
}


/*========== Cache Volume Operations Implemention BEGIN. ==========*/

/**
 * Open cache volume.
 * Here we store uuid as volume name and connect to FlashSim socket.
 *
 * At any time, at most CACHE_PARALLELISM requests are being processed
 * concurrently. This models in-device parallelism.
 */
static int
cache_vol_open(ocf_volume_t cache_vol, void *params)
{
    const struct ocf_volume_uuid *uuid = ocf_volume_get_uuid(cache_vol);
    cache_vol_priv_t *vol_priv = ocf_volume_get_priv(cache_vol);
    struct sockaddr_un saddr;    
    int ret;

    vol_priv->name = ocf_uuid_to_str(uuid);

    /** Prepare socket here. */
    vol_priv->sock_name = cache_sock_name;
    vol_priv->sock_fd = socket(AF_LOCAL, SOCK_STREAM, 0);
    if (vol_priv->sock_fd < 0) {
        DEBUG("OPEN: socket() failed");
        return 1;
    }

    memset(&saddr, 0, sizeof(saddr));
    saddr.sun_family = AF_LOCAL;
    strncpy(saddr.sun_path, vol_priv->sock_name, sizeof(saddr.sun_path) - 1);

    ret = connect(vol_priv->sock_fd, (struct sockaddr *) &saddr,
                  sizeof(saddr));
    if (ret) {
        DEBUG("OPEN: connect() failed");
        return ret;
    }

    /** Initialize submission queue. */
    INIT_LIST_HEAD(&submit_queue.head);

    ret = env_mutex_init(&submit_queue_lock);
    if (ret) {
        DEBUG("OPEN: queue lock initialization failed");
        return ret;
    }

    env_completion_init(&submit_queue_sem);

    env_atomic_set(&should_stop, 0);

    /** Start submit thread at volume open. */
    pthread_t submit_thread_id;
    pthread_attr_t submit_thread_attr;

    ret = pthread_attr_init(&submit_thread_attr);
    if (ret) {
        DEBUG("OPEN: submit thread creation failed");
        return ret;
    }

    ret = pthread_attr_setdetachstate(&submit_thread_attr,
                                      PTHREAD_CREATE_DETACHED);
    if (ret) {
        DEBUG("OPEN: submit thread creation failed");
        return ret;
    }

    ret = pthread_create(&submit_thread_id, &submit_thread_attr,
                         _submit_thread_func, NULL);
    if (ret) {
        DEBUG("OPEN: submit thread creation failed");
        return ret;
    }

    DEBUG("OPEN: name = %s, sock = %s", vol_priv->name, vol_priv->sock_name);
    return 0;
}

/**
 * Close cache volume.
 * Here we simply close the socket.
 */
static void
cache_vol_close(ocf_volume_t cache_vol)
{
    cache_vol_priv_t *vol_priv = ocf_volume_get_priv(cache_vol);

    DEBUG("CLOSE: name = %s", vol_priv->name);

    close(vol_priv->sock_fd);

    ENV_BUG_ON(! list_empty(&submit_queue.head));

    env_mutex_destroy(&submit_queue_lock);
    env_completion_destroy(&submit_queue_sem);
}

// Exposed for device logging.
extern double base_time_ms;

/**
 * Submit an IO request to volume.
 * Here we simply push to the tail of queue.
 */
static void
cache_vol_submit_io(struct ocf_io *io)
{
    struct req_entry *entry;
    int queue_depth;

    /** Address must be page-aligned. */
    if (io->addr % flashsim_page_size != 0) {
        DEBUG("IO: unaligned addr 0x%08lx", io->addr);
        io->end(io, 1);
        return;
    }

    entry = malloc(sizeof(struct req_entry));
    if (entry == NULL) {
        DEBUG("IO: push to submit queue failed");
        return;
    }

    entry->io = io;
    entry->start_time_ms = get_cur_time_ms();

    env_mutex_lock(&submit_queue_lock);

    list_add_tail(&entry->head, &submit_queue.head);
    env_completion_complete(&submit_queue_sem);

    if (DEVICE_LOG_ENABLE) {
        sem_getvalue(&submit_queue_sem.sem, &queue_depth);
        fprintf(fdevice, "cache queue: @ %.3lf, depth = %d\n",
                entry->start_time_ms - base_time_ms, queue_depth);
    }

    env_mutex_unlock(&submit_queue_lock);
}

/**
 * Submit flush request.
 * Can be unimplemented if not needed.
 */
static void
cache_vol_submit_flush(struct ocf_io *io)
{
    io->end(io, 0);     /** Unimplemented. */
}

/**
 * Submit discard request.
 * Can be unimplemented if not needed.
 */
static void
cache_vol_submit_discard(struct ocf_io *io)
{
    io->end(io, 0);     /** Unimplemented. */
}


/**
 * Define the max I/O size for this volume.
 * Here we use 128KiB.
 */
static unsigned int
cache_vol_get_max_io_size(ocf_volume_t cache_vol)
{
    return CACHE_VOL_MAX_IO_SIZE;
}

/**
 * Get volume capacity.
 */
static uint64_t
cache_vol_get_length(ocf_volume_t cache_vol)
{
    return cache_capacity_bytes;
}


/**
 * Define how to setup a single I/O structure given OS data buffer
 * structure.
 */
static int
cache_vol_io_set_data(struct ocf_io *io, ctx_data_t *simfs_data,
                      uint32_t offset)
{
    cache_vol_io_priv_t *io_priv = ocf_io_get_priv(io);

    /** This offset is meant to control I/O in finer granularity. */
    io_priv->data = simfs_data;
    io_priv->offset = offset;

    return 0;
}

/**
 * Define how to retrieve OS buffer structured data from a single
 * I/O structure.
 */
static ctx_data_t *
cache_vol_io_get_data(struct ocf_io *io)
{
    cache_vol_io_priv_t *io_priv = ocf_io_get_priv(io);

    return io_priv->data;
}


/**
 * This structure assigns the above implementations to the OCF volume
 * interface. This structure essentially defines a volume type, which
 * can be used when setting up volumes during context initialization.
 */
const struct ocf_volume_properties cache_vol_properties = {
    .name = "Cache Volume",
    
    .io_priv_size = sizeof(cache_vol_io_priv_t),
    .volume_priv_size = sizeof(cache_vol_priv_t),

    .caps = {
        .atomic_writes = 0,
    },

    .ops = {
        .open = cache_vol_open,
        .close = cache_vol_close,
        .submit_io = cache_vol_submit_io,
        .submit_flush = cache_vol_submit_flush,
        .submit_discard = cache_vol_submit_discard,
        .get_max_io_size = cache_vol_get_max_io_size,
        .get_length = cache_vol_get_length,
    },

    .io_ops = {
        .set_data = cache_vol_io_set_data,
        .get_data = cache_vol_io_get_data,
    },
};

/*========== Cache Volume Operations Implemention END. ==========*/


/**
 * Indicate that submission threads should stop, without finishing pending
 * requests in queue.
 */
void
cache_vol_force_stop()
{
    struct req_entry *entry;

    env_mutex_lock(&submit_queue_lock);

    while (! list_empty(&submit_queue.head)) {
        entry = list_first_entry(&submit_queue.head,
                                 struct req_entry, head);
        list_del(&entry->head);
        free(entry);
    }

    env_atomic_inc(&should_stop);

    env_completion_complete(&submit_queue_sem);

    env_mutex_unlock(&submit_queue_lock);
}


/**
 * Registers the above structure as volume type CACHE_VOL_TYPE in
 * this OCF context.
 * Should be called just AFTER context initialization.
 */
int
cache_vol_register(ocf_ctx_t ctx)
{
    int ret = ocf_ctx_register_volume_type(ctx, CACHE_VOL_TYPE,
                                           &cache_vol_properties);

    DEBUG("REGISTER: as type = %d", CACHE_VOL_TYPE);

    return ret;
}

/**
 * Unregisters cache volume type.
 * Should be called just BEFORE context cleanup.
 */
void
cache_vol_unregister(ocf_ctx_t ctx)
{
    ocf_ctx_unregister_volume_type(ctx, CACHE_VOL_TYPE);

    DEBUG("UNREGISTER: done");
}
