/**
 * The `simfs` application context header.
 * 
 * The interface type `ctx_data_t` in OCF is supposed to describe the
 * OS data buffer (occupying several data pages) for a piece of data
 * being I/Oed.
 *
 * It is typedefed as `void` in OCF interface. We cast it to our context-
 * specific `simfs_data_t` to hook up with OCF interface.
 */


#ifndef __SIMFS_CTX_H__
#define __SIMFS_CTX_H__


#include <ocf/ocf.h>
#include "ocf_env.h"


/**
 * The OS data buffer structure to be used in this context.
 */
struct simfs_data {
    void *ptr;
    int offset;
    uint32_t pages;     /** Total allocated size in pages. */
    bool served;        /** Have been served by a volume?. */
};

typedef struct simfs_data simfs_data_t;


/** Might be used in workload code. */
ctx_data_t *simfs_data_alloc(uint32_t pages);
void simfs_data_free(ctx_data_t *simfs_data);

int simfs_ctx_init(ocf_ctx_t *ctx);
void simfs_ctx_cleanup(ocf_ctx_t ctx);


#endif
