/**
 * Cache object header.
 *
 * After volume types have been registered, a logical object should then
 * be created, and a volume of some registered type be attached to the
 * object.
 */


#ifndef __CORE_OBJ_H__
#define __CORE_OBJ_H__


#include <ocf/ocf.h>
#include "ocf_env.h"


/** Device log for throughput measurement. */
void core_log_push_entry(double finish_time_ms, uint32_t bytes);
double core_log_query_throughput(double begin_time_ms, double end_time_ms);


int core_obj_setup(ocf_cache_t cache, ocf_core_t *core);
int core_obj_stop(ocf_core_t core);


#endif
