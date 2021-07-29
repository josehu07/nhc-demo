/**
 * Benchmark - System action on given intensity.
 */


#ifndef __THROUGHPUT_H__
#define __THROUGHPUT_H__


#include <ocf/ocf.h>
#include "ocf_env.h"


int bench_throughput(ocf_core_t core, int num_args, char **bench_args);


#endif
