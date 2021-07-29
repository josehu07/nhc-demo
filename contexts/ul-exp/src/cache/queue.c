/**
 * Customized queue implementation.
 */


#include <ocf/ocf.h>

#include "queue.h"


/*========== Customized queue implementation BEGIN. ==========*/

/**
 * Trigger queue asynchronously. Asynchronous kick is basically used
 * in management queue & cleaner queue only.
 */
static void
queue_kick_async(ocf_queue_t queue)
{
    ocf_queue_run(queue);   /** Unimplemented. */
}

/**
 * Trigger queue synchronously. In some environments kicking queue
 * synchronously may reduce latency, so OCF will call this variant of
 * queue kick where possible.
 */
static void
queue_kick_sync(ocf_queue_t queue)
{
    ocf_queue_run(queue);
}

/**
 * Stop queue thread.
 * Here left unimplemented as we have no effective asynchronous impl.
 */
static void
queue_stop(ocf_queue_t queue)
{
    return;     /** Unimplemented. */
}


/**
 * This structure assigns the above customized implementations to OCF
 * queue interface.
 */
struct ocf_queue_ops queue_ops = {
    .kick_sync = queue_kick_sync,
    .kick = queue_kick_async,
    .stop = queue_stop,
};

/*========== Customized queue implementation END. ==========*/
