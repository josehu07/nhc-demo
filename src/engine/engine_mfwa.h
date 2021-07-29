/**
 * Multi-factor cache mode (with write-around) implementation.
 *
 * Writes always follow Write-Around (for now). Reads swtich between
 * cache & core according to `load_admit`. Reads populate lines into
 * cache only if `data_admit` is on (i.e., in workload probing stage).
 *
 * Monitor logic is implemented in `mf_monitor.c`. Must ensure that
 * the monitor has been initialized and started through
 * `ocf_mngt_mf_monitor_init()`.
 */

/*========== [Orthus FLAG BEGIN] ==========*/

#ifndef ENGINE_MFWA_H_
#define ENGINE_MFWA_H_


#include "../ocf_request.h"


int ocf_read_mfwa(struct ocf_request *req);


#endif /* ENGINE_MFWA_H_ */

/*========== [Orthus FLAG END] ==========*/
